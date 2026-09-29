/* Plain Wi-Fi STA — no CSI, no AP, no ESP-NOW. Auto-reconnects. */
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wifi_sta";
static esp_netif_t *s_netif;
static volatile bool s_connected;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
    }
}

esp_err_t wifi_sta_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_sta_start(void)
{
    wifi_config_t cfg = {0};
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no stored credentials");
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = sizeof(cfg.sta.ssid);
    esp_err_t e1 = nvs_get_str(h, "ssid", (char *)cfg.sta.ssid, &n);
    n = sizeof(cfg.sta.password);
    nvs_get_str(h, "pass", (char *)cfg.sta.password, &n);   /* empty = open network */
    nvs_close(h);
    if (e1 != ESP_OK) return ESP_ERR_NOT_FOUND;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    return esp_wifi_start();
}

bool wifi_sta_is_connected(void) { return s_connected; }

bool wifi_sta_get_ip(char *out, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) return false;
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    return true;
}
