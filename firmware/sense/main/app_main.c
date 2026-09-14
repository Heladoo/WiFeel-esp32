#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "wifi_mgr.h"
#include "csi_mgr.h"
#include "console_cmds.h"

static const char *TAG = "app_main";

/* Prints S1 pkt/s periodically once connected, so a plain boot-log capture
 * (tools/serial_log.py, no interactive 'status' needed) is enough to check
 * milestone 4's "S1 >= 80 pkt/s" target — see the esp32-firmware-flashing
 * skill's post-flash verification step and CLAUDE.md. */
static void feature_log_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!wifi_mgr_is_connected() || !csi_mgr_is_enabled()) {
            continue;
        }
        wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
        float pkt_rate = wifeel_csi_stream_get_pkt_rate(s1);
        wifeel_msg_features_t feat;
        if (wifeel_csi_stream_get_features(s1, &feat)) {
            ESP_LOGI(TAG, "S1: %.1f pkt/s  mean_amp=%.2f wander=%.2f jitter=%.2f",
                     (double)pkt_rate, (double)feat.mean_amplitude, (double)feat.wander,
                     (double)feat.jitter_energy);
        } else {
            ESP_LOGI(TAG, "S1: %.1f pkt/s  (warming up)", (double)pkt_rate);
        }
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
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(csi_mgr_init());

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " WiFeel sense (hub)  fw=%s  target=%s", WIFEEL_SENSE_FW_VERSION, CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, " antenna: %s", board_has_external_antenna() ? "external" : "onboard");
    ESP_LOGI(TAG, " free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "==================================================");

    xTaskCreate(&feature_log_task, "feature_log", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);

    ESP_ERROR_CHECK(console_start());
}
