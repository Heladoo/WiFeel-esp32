/* Minimal app: dark round-safe UI with a swipeable 2-tile view. */
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_amoled.h"
#include "touch.h"

#define FW_VERSION "0.1.0"
#define COLOR_BG    0x101418
#define COLOR_TEXT  0xE8EEF2
#define COLOR_MUTED 0x8FA3AD   /* ~7:1 contrast on COLOR_BG; don't go darker for text */

static const char *TAG = "app_main";
static lv_obj_t *s_clock_label;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Set the dark bg on the tileview AND each tile, or the default light
     * theme shows through during swipes. */
    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *t0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);

    s_clock_label = make_label(t0, "0", &lv_font_montserrat_48, COLOR_TEXT);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align_to(make_label(t0, "uptime (s)", &lv_font_montserrat_14, COLOR_MUTED),
                    s_clock_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    lv_obj_align(make_label(t1, "tile 2", &lv_font_montserrat_28, COLOR_TEXT), LV_ALIGN_CENTER, 0, 0);
}

static void ui_update_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!bsp_amoled_lvgl_lock(100)) {
            continue;              /* never block the UI task forever; retry next tick */
        }
        lv_label_set_text_fmt(s_clock_label, "%" PRIu32, (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ));
        bsp_amoled_lvgl_unlock();
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

    ESP_ERROR_CHECK(bsp_amoled_init());
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch_init failed (continuing without touch)");
    }

    /* Boot banner: version + target + free heap (needed for post-flash checks). */
    ESP_LOGI(TAG, "app fw=%s target=%s free heap=%" PRIu32, FW_VERSION, CONFIG_IDF_TARGET,
             (uint32_t)esp_get_free_heap_size());

    if (bsp_amoled_lvgl_lock(-1)) {
        build_screen();
        bsp_amoled_lvgl_unlock();
    }
    xTaskCreate(ui_update_task, "ui_update", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
}
