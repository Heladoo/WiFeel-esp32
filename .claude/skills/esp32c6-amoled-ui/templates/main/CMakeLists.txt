idf_component_register(
    SRCS "app_main.c" "bsp_amoled.c" "touch.c"
    # Add "wifi_sta.c" "web_server.c" only for the web-access variant.
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_i2c esp_timer nvs_flash
    # Web variant also needs: esp_wifi esp_netif esp_event esp_http_server
)
