#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Start Wi-Fi station mode and connect. Credentials come from NVS
 * (namespace "wifi", keys "ssid"/"pass") — provision them once with
 * wifi_sta_save_credentials(); never hardcode a real home password. */
esp_err_t wifi_sta_start(void);
esp_err_t wifi_sta_save_credentials(const char *ssid, const char *pass);
bool wifi_sta_is_connected(void);
bool wifi_sta_get_ip(char *out, size_t len);   /* "192.168.x.y" */
