#include "link.h"

#include <inttypes.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ping_gw.h"

static const char *TAG = "link";

/* ~100 Hz, matching firmware/sense's ping_gw usage — the hub's CSI
 * capture on this link (S3) needs traffic to tap, same reasoning as
 * JOIN mode. */
#define LINK_PING_INTERVAL_MS 10

/* Backstop for a bug observed live: the hub's SoftAP link can go dead
 * (e.g. the hub's STA side reconnects/roams, forcing the AP to follow
 * its channel) without WIFI_EVENT_STA_DISCONNECTED ever firing here —
 * association state says "still connected" while ping_gw's sends fail
 * forever. Rather than trust the disconnect event alone, poll how long
 * it's been since the last successful ping reply and force a reconnect
 * if that stretches out too long. At 100Hz, a healthy link sees a
 * success at least every ~10ms, so a multi-second stall is an
 * unambiguous signal, not noise. */
#define LINK_WATCHDOG_CHECK_INTERVAL_MS 2000
#define LINK_WATCHDOG_STALL_MS 5000

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static wifeel_msg_state_t s_latest_state;
static bool s_have_state;
static int64_t s_latest_state_us;

static esp_netif_t *s_sta_netif;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    wifeel_msg_type_t type;
    uint16_t seq;
    const void *payload;
    size_t payload_len;

    if (!wifeel_proto_unpack(data, (size_t)len, &type, &seq, &payload, &payload_len)) {
        return; /* corrupt/foreign frame — silently drop, not our protocol */
    }
    if (type != WIFEEL_MSG_STATE || payload_len < sizeof(wifeel_msg_state_t)) {
        return;
    }

    portENTER_CRITICAL(&s_state_mux);
    memcpy(&s_latest_state, payload, sizeof(s_latest_state));
    s_have_state = true;
    s_latest_state_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_mux);
}

bool link_get_latest_state(wifeel_msg_state_t *out, uint32_t *age_ms_out)
{
    bool have;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_mux);
    have = s_have_state;
    if (have && out) {
        memcpy(out, &s_latest_state, sizeof(*out));
    }
    int64_t age_us = now - s_latest_state_us;
    portEXIT_CRITICAL(&s_state_mux);

    if (have && age_ms_out) {
        *age_ms_out = (uint32_t)(age_us / 1000);
    }
    return have;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected from hub, retrying");
        ping_gw_stop();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "connected to hub's SoftAP");
        if (s_sta_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
                esp_err_t err = ping_gw_start(&ip_info.gw, LINK_PING_INTERVAL_MS);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "ping_gw_start failed: %s", esp_err_to_name(err));
                }
            }
        }
    }
}

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_WATCHDOG_CHECK_INTERVAL_MS));

        if (!ping_gw_is_running()) {
            continue; /* not connected yet, or a real disconnect already handled it */
        }
        uint32_t stalled_ms = ping_gw_ms_since_last_success();
        if (stalled_ms < LINK_WATCHDOG_STALL_MS) {
            continue;
        }

        ESP_LOGW(TAG, "no successful ping in %" PRIu32 " ms — link looks dead but no "
                 "disconnect event fired; forcing a reconnect", stalled_ms);
        ping_gw_stop();
        /* esp_wifi_disconnect() should itself trigger WIFI_EVENT_STA_DISCONNECTED,
         * whose handler already does ping_gw_stop()+esp_wifi_connect() — but if
         * the driver's internal state disagrees with what we're observing here
         * (the whole reason this watchdog exists) that event might not come, so
         * call esp_wifi_connect() directly too rather than wait on it. */
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
}

esp_err_t link_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFEEL_LINK_AP_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFEEL_LINK_AP_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(&on_recv));

    BaseType_t ok = xTaskCreate(&watchdog_task, "link_watchdog", 2560, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "failed to start link watchdog task");
    }

    ESP_LOGI(TAG, "joining hub SoftAP \"%s\"", WIFEEL_LINK_AP_SSID);
    return ESP_OK;
}
