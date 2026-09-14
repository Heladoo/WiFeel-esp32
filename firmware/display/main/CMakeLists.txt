idf_component_register(
    SRCS
        "app_main.c"
        "bsp_amoled.c"
        "touch.c"
        "espnow_test.c"
    INCLUDE_DIRS "."
    REQUIRES
        "esp_lcd"
        "driver"
        "esp_driver_i2c"
        "esp_timer"
        "nvs_flash"
        "esp_wifi"
        "esp_netif"
        "esp_event"
)
