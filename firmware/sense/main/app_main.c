#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_mac.h"

#include "board.h"
#include "wifi_mgr.h"
#include "csi_mgr.h"
#include "ping_gw.h"
#include "motion.h"
#include "presence.h"
#include "link.h"
#include "console_cmds.h"

static const char *TAG = "app_main";

/* ~100 Hz per the plan's JOIN mode design. A 50ms (~20Hz) rate was tried
 * to test whether the router's ~8s reconnect cycle was a reaction to ping
 * load; the cycle's timing didn't change at all, so this was reverted
 * (see docs/boards.md). */
#define JOIN_PING_INTERVAL_MS 10

/* Backstop for a bug observed live: S1's gateway ping can get stuck
 * retrying (and failing) forever without WIFI_EVENT_STA_DISCONNECTED
 * ever firing — seen to cascade into the hub being too busy/starved to
 * complete the SoftAP's WPA2 handshake with the display, breaking S3
 * too even though the hub itself never crashed. Poll how long it's been
 * since the last successful ping and force a reconnect if that
 * stretches out too long, rather than trusting the disconnect event
 * alone (same fix as firmware/display/main/link.c's watchdog). */
#define S1_WATCHDOG_CHECK_INTERVAL_MS 2000
#define S1_WATCHDOG_STALL_MS 5000

/*
 * Fires on EVERY successful Wi-Fi connection (see wifi_mgr_set_connected_cb)
 * — an explicit `join` console command, but also ESP-IDF's automatic
 * reconnect on boot using NVS-persisted credentials, which happens with no
 * app code asking for it. Without this living here, CSI capture and the
 * gateway ping only ever got armed by cmd_join() itself, so the very
 * common case of "board reboots and reconnects on its own" silently never
 * captured any CSI — found by testing on real hardware, not guessed. */
static void on_wifi_connected(void)
{
    uint8_t bssid[6];
    esp_ip4_addr_t gw = {0};
    if (wifi_mgr_get_ap_bssid(bssid) != ESP_OK || wifi_mgr_get_gateway_ip(&gw) != ESP_OK) {
        ESP_LOGW(TAG, "connected but AP info not ready yet, skipping CSI/ping setup");
        return;
    }

    esp_err_t err = csi_mgr_set_source_mac(WIFEEL_STREAM_ROUTER_TO_HUB, bssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi_mgr_set_source_mac failed: %s", esp_err_to_name(err));
    }
    err = csi_mgr_set_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi_mgr_set_enabled failed: %s", esp_err_to_name(err));
    }
    err = ping_gw_start(&gw, JOIN_PING_INTERVAL_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ping_gw_start failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "CSI+ping configured for AP " MACSTR, MAC2STR(bssid));
}

/* S3 (display->hub): fires when the display associates to / leaves the
 * hub's own SoftAP (see wifi_mgr.h). Second, independent sensing vantage
 * point alongside S1 — see motion.c for how the two get fused. */
static void on_ap_peer_connected(const uint8_t mac[6])
{
    esp_err_t err = csi_mgr_set_source_mac(WIFEEL_STREAM_DISPLAY_TO_HUB, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi_mgr_set_source_mac (S3) failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "S3 now tracking display " MACSTR, MAC2STR(mac));
}

static void on_ap_peer_disconnected(const uint8_t mac[6])
{
    static const uint8_t zero_mac[6] = {0};
    (void)mac;
    csi_mgr_set_source_mac(WIFEEL_STREAM_DISPLAY_TO_HUB, zero_mac);
    ESP_LOGI(TAG, "S3 stopped (display disconnected)");
}

static void s1_ping_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(S1_WATCHDOG_CHECK_INTERVAL_MS));

        if (!ping_gw_is_running()) {
            continue; /* not connected yet, or a real disconnect already handled it */
        }
        uint32_t stalled_ms = ping_gw_ms_since_last_success();
        if (stalled_ms < S1_WATCHDOG_STALL_MS) {
            continue;
        }

        ESP_LOGW(TAG, "S1: no successful ping in %" PRIu32 " ms — forcing reconnect", stalled_ms);
        ping_gw_stop();
        wifi_mgr_force_reconnect();
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init());
    /* Must be registered before wifi_mgr_init() — see wifi_mgr.h — so a
     * same-boot auto-reconnect (or the display connecting to our SoftAP)
     * can't fire before these are set. */
    wifi_mgr_set_connected_cb(&on_wifi_connected);
    wifi_mgr_set_ap_peer_connected_cb(&on_ap_peer_connected);
    wifi_mgr_set_ap_peer_disconnected_cb(&on_ap_peer_disconnected);
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(csi_mgr_init());
    ESP_ERROR_CHECK(motion_init());
    ESP_ERROR_CHECK(presence_init());
    ESP_ERROR_CHECK(link_init());

    BaseType_t watchdog_ok = xTaskCreate(&s1_ping_watchdog_task, "s1_ping_watchdog", 2560,
                                          NULL, tskIDLE_PRIORITY + 1, NULL);
    if (watchdog_ok != pdPASS) {
        ESP_LOGW(TAG, "failed to start S1 ping watchdog task");
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " WiFeel sense (hub)  fw=%s  target=%s", WIFEEL_SENSE_FW_VERSION, CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, " antenna: %s", board_has_external_antenna() ? "external" : "onboard");
    ESP_LOGI(TAG, " free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "==================================================");

    /* No automatic periodic status logging: it was found on real hardware
     * to make the console genuinely unusable (log lines interleave with
     * typed input over the single shared USB-Serial-JTAG port). Use the
     * `status` console command on demand instead — cheap to run, and it
     * doesn't fight the user for the terminal. */
    ESP_ERROR_CHECK(console_start());
}
