#include "espnow_rx_test.h"

#include "esp_now.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "espnow_rx_test";
static bool s_started;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)data;
    ESP_LOGI(TAG, "ESP-NOW frame from " MACSTR " len=%d rssi=%d", MAC2STR(info->src_addr), len,
              info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}

esp_err_t espnow_rx_test_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_now_register_recv_cb(&on_recv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "ESP-NOW RX test started (no CSI involved)");
    return ESP_OK;
}
