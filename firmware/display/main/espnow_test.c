#include "espnow_test.h"

#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espnow_test";
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    uint32_t counter;
    uint32_t t_ms;
} sound_payload_t;

static void send_task(void *arg)
{
    uint32_t rate_hz = (uint32_t)(intptr_t)arg;
    uint32_t period_ms = rate_hz > 0 ? (1000 / rate_hz) : 100;
    sound_payload_t payload = {0};

    for (;;) {
        payload.counter++;
        payload.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)&payload, sizeof(payload));
        if (err != ESP_OK && payload.counter % 100 == 1) {
            ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

esp_err_t espnow_test_start(uint8_t channel, uint32_t rate_hz)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG, "broadcasting ESP-NOW sounding frames on channel %u at %" PRIu32 " Hz", channel, rate_hz);
    xTaskCreate(&send_task, "espnow_snd", 3072, (void *)(intptr_t)rate_hz, tskIDLE_PRIORITY + 3, NULL);
    return ESP_OK;
}
