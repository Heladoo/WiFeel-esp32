#include "wifi_mgr.h"

#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifeel_proto.h" /* WIFEEL_LINK_AP_SSID / WIFEEL_LINK_AP_PASSWORD */

static const char *TAG = "wifi_mgr";

static EventGroupHandle_t s_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

static esp_netif_t *s_sta_netif;
static bool s_initialized;
static bool s_connected;
static wifi_mgr_connected_cb_t s_connected_cb;
static bool s_auto_reconnect = true;
static wifi_mgr_ap_peer_cb_t s_ap_peer_connected_cb;
static wifi_mgr_ap_peer_cb_t s_ap_peer_disconnected_cb;

void wifi_mgr_set_connected_cb(wifi_mgr_connected_cb_t cb)
{
    s_connected_cb = cb;
}

void wifi_mgr_set_ap_peer_connected_cb(wifi_mgr_ap_peer_cb_t cb)
{
    s_ap_peer_connected_cb = cb;
}

void wifi_mgr_set_ap_peer_disconnected_cb(wifi_mgr_ap_peer_cb_t cb)
{
    s_ap_peer_disconnected_cb = cb;
}

void wifi_mgr_set_auto_reconnect(bool enabled)
{
    s_auto_reconnect = enabled;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_auto_reconnect) {
            ESP_LOGW(TAG, "disconnected, retrying");
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "disconnected (auto-reconnect suppressed)");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        if (s_connected_cb) {
            s_connected_cb();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "AP: station " MACSTR " connected", MAC2STR(ev->mac));
        if (s_ap_peer_connected_cb) {
            s_ap_peer_connected_cb(ev->mac);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *ev = (const wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "AP: station " MACSTR " disconnected", MAC2STR(ev->mac));
        if (s_ap_peer_disconnected_cb) {
            s_ap_peer_disconnected_cb(ev->mac);
        }
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    /* APSTA: a station (for wifi_mgr_join(), the user's real router) and
     * the hub's own SoftAP (for the display link) running simultaneously
     * on the one radio — standard ESP-IDF coexistence mode. The SoftAP
     * automatically follows whatever channel the STA ends up on. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, WIFEEL_LINK_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, WIFEEL_LINK_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 1; /* only the display is expected to join */
    ap_config.ap.channel = 0;        /* auto — APSTA locks this to the STA's channel anyway */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    /* Power-save off: modem sleep gates both CSI capture and our high-rate
     * ping traffic (see the plan's JOIN mode design). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_mgr_join(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_initialized || !ssid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_connected) {
        wifi_mgr_disconnect();
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password && password[0]) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "joined %s", ssid);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "join %s timed out after %" PRIu32 " ms", ssid, timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void wifi_mgr_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
}

void wifi_mgr_force_reconnect(void)
{
    ESP_LOGW(TAG, "forcing a reconnect");
    s_connected = false;
    /* The STA_DISCONNECTED handler reconnects (auto-reconnect is on). An
     * extra esp_wifi_connect() here raced with it ("sta is connecting,
     * return error" in the log). */
    esp_wifi_disconnect();
}

bool wifi_mgr_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_mgr_get_ap_bssid(uint8_t bssid_out[6])
{
    if (!s_connected) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(bssid_out, ap_info.bssid, 6);
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ap_channel(uint8_t *channel_out)
{
    if (!s_connected) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return err;
    }
    *channel_out = ap_info.primary;
    return ESP_OK;
}

esp_err_t wifi_mgr_get_gateway_ip(esp_ip4_addr_t *gw_out)
{
    if (!s_connected || !s_sta_netif) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    *gw_out = ip_info.gw;
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ip(esp_ip4_addr_t *ip_out)
{
    if (!s_connected || !s_sta_netif) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    *ip_out = ip_info.ip;
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ap_ssid(char *out, size_t out_len)
{
    if (!s_connected) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return err;
    }
    /* ap_info.ssid is a fixed uint8_t[33] buffer, not guaranteed
     * NUL-terminated when the SSID uses the full 32 bytes — copy with an
     * explicit bound and terminate ourselves rather than assuming. */
    size_t n = sizeof(ap_info.ssid);
    if (n > out_len - 1) {
        n = out_len - 1;
    }
    memcpy(out, ap_info.ssid, n);
    out[n] = '\0';
    return ESP_OK;
}
