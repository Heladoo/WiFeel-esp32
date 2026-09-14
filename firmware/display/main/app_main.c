#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "bsp_amoled.h"
#include "touch.h"
#include "espnow_test.h"

/* TEMPORARY (see espnow_test.h): fixed test channel. Channel 6 (isolated
 * from any AP) gave ambiguous, hard-to-reproduce sniff results — retest on
 * channel 9 instead, matching HUB-1's router, so the hub can observe this
 * without disconnecting/retuning at all (see docs/boards.md). */
#define ESPNOW_TEST_CHANNEL 9
#define ESPNOW_TEST_RATE_HZ 50

/* Bump on every behavior change — matches firmware/sense's board.h
 * convention (see CLAUDE.md). */
#define WIFEEL_DISPLAY_FW_VERSION "0.1.0-dev"

static const char *TAG = "app_main";

static void build_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFeel");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text_fmt(ver, "display fw %s", WIFEEL_DISPLAY_FW_VERSION);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x8FA3AD), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *heap = lv_label_create(scr);
    lv_label_set_text_fmt(heap, "%" PRIu32 " KB free", (uint32_t)(esp_get_free_heap_size() / 1024));
    lv_obj_set_style_text_color(heap, lv_color_hex(0x5A6B73), LV_PART_MAIN);
    lv_obj_set_style_text_font(heap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(heap, LV_ALIGN_CENTER, 0, 55);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_amoled_init());
    esp_err_t touch_err = touch_init();
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "touch_init failed: %s (continuing without touch)", esp_err_to_name(touch_err));
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " WiFeel display  fw=%s  target=%s", WIFEEL_DISPLAY_FW_VERSION, CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, " free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "==================================================");

    if (bsp_amoled_lvgl_lock(-1)) {
        build_boot_screen();
        bsp_amoled_lvgl_unlock();
    }

    esp_err_t espnow_err = espnow_test_start(ESPNOW_TEST_CHANNEL, ESPNOW_TEST_RATE_HZ);
    if (espnow_err != ESP_OK) {
        ESP_LOGE(TAG, "espnow_test_start failed: %s", esp_err_to_name(espnow_err));
    }
}
