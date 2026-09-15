#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_amoled.h"
#include "touch.h"
#include "link.h"
#include "ui_phones.h"

/* Bump on every behavior change — matches firmware/sense's board.h
 * convention (see CLAUDE.md). */
#define WIFEEL_DISPLAY_FW_VERSION "0.1.0-dev"

/* Treat a STATE broadcast as stale after this long without a fresh one.
 * The hub broadcasts at LINK_RATE_HZ=3 (link.c), so a few missed beats is
 * normal; much longer than that means the hub rebooted, disconnected, or
 * is out of range. */
#define LINK_STALE_MS 4000
#define UI_UPDATE_INTERVAL_MS 333 /* matches the hub's ~3Hz broadcast rate */
#define CHART_POINT_COUNT 80      /* ~26s of history at this update rate (user asked to double the original ~13s) */

/* Dark background for the whole app — set on the root screen and the
 * tileview itself, not just each tile, so no default-theme light
 * background can show through (e.g. during a swipe, or at the tileview's
 * own edges). */
#define COLOR_BG 0x101418

/* Same colors used for the S1/S3 legend text and chart series, so they
 * read as one system. */
#define COLOR_S1 0x4FA8E8 /* blue */
#define COLOR_S3 0xE8A33D /* amber — matches the indicator's "motion" color */

static const char *TAG = "app_main";

/* UI objects the update task needs to keep touching — created once in
 * build_home_screen(), read/written only while holding the LVGL lock. */
static lv_obj_t *s_indicator;
static lv_obj_t *s_score_label;
static lv_obj_t *s_network_label;
static lv_obj_t *s_link_label;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_chart_s1;
static lv_chart_series_t *s_chart_s3;

/** Builds the Home tile's widgets under `home_tile` (one tile of the
 *  top-level lv_tileview — see build_screen()). Unchanged from before the
 *  Phones tile existed, aside from creating on `home_tile` instead of the
 *  screen directly. */
static void build_home_screen(lv_obj_t *home_tile)
{
    lv_obj_t *scr = home_tile;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFeel");
    lv_obj_set_style_text_color(title, lv_color_hex(0x5A6B73), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    s_network_label = lv_label_create(scr);
    lv_label_set_text(s_network_label, "");
    lv_obj_set_style_text_color(s_network_label, lv_color_hex(0x8FA3AD), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_network_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_network_label, LV_ALIGN_TOP_MID, 0, 52);

    /* Motion indicator: a plain circle whose fill color is the whole
     * story — grey (no data yet / link lost), green (still), amber
     * (motion). Deliberately not an arc/gauge yet; that's real UI polish
     * for a later milestone, this is "prove live data reaches the
     * screen". Shrunk from an earlier version to make room for the
     * trend chart below it. */
    s_indicator = lv_obj_create(scr);
    lv_obj_set_size(s_indicator, 120, 120);
    lv_obj_set_style_radius(s_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_indicator, lv_color_hex(0x3A4750), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_indicator, 0, LV_PART_MAIN);
    lv_obj_align(s_indicator, LV_ALIGN_CENTER, 0, -110);

    s_score_label = lv_label_create(scr);
    lv_label_set_text(s_score_label, "--");
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_score_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align_to(s_score_label, s_indicator, LV_ALIGN_CENTER, 0, 0);

    /* Trend chart: S1 (router->hub) and S3 (display->hub) motion scores
     * over the last ~13s, so it's visible which sensing stream is
     * actually driving a detection rather than just the fused number. */
    lv_obj_t *legend_s1 = lv_label_create(scr);
    lv_label_set_text(legend_s1, "S1 router");
    lv_obj_set_style_text_color(legend_s1, lv_color_hex(COLOR_S1), LV_PART_MAIN);
    lv_obj_set_style_text_font(legend_s1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(legend_s1, LV_ALIGN_CENTER, -75, 5);

    lv_obj_t *legend_s3 = lv_label_create(scr);
    lv_label_set_text(legend_s3, "S3 display");
    lv_obj_set_style_text_color(legend_s3, lv_color_hex(COLOR_S3), LV_PART_MAIN);
    lv_obj_set_style_text_font(legend_s3, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(legend_s3, LV_ALIGN_CENTER, 75, 5);

    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 320, 130);
    lv_obj_align(s_chart, LV_ALIGN_CENTER, 0, 85);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(s_chart, CHART_POINT_COUNT);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x181E24), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR); /* hide per-point marker dots, keep the line */
    s_chart_s1 = lv_chart_add_series(s_chart, lv_color_hex(COLOR_S1), LV_CHART_AXIS_PRIMARY_Y);
    s_chart_s3 = lv_chart_add_series(s_chart, lv_color_hex(COLOR_S3), LV_CHART_AXIS_PRIMARY_Y);

    s_link_label = lv_label_create(scr);
    lv_label_set_text(s_link_label, "waiting for hub...");
    lv_obj_set_style_text_color(s_link_label, lv_color_hex(0x8FA3AD), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_link_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_link_label, LV_ALIGN_BOTTOM_MID, 0, -26);
}

/** Home tile + Phones tile, side by side in one horizontally-swipeable
 *  lv_tileview. Dark background is set on the screen AND the tileview
 *  itself (not just each tile) — LVGL's default theme background is
 *  light, and without this it showed through around/behind the tileview
 *  (reported live: "background changed to white"). */
static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *home_tile = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
    lv_obj_t *phones_tile = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);

    build_home_screen(home_tile);
    ui_phones_create(phones_tile);
}

static void ui_update_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_UPDATE_INTERVAL_MS));

        wifeel_msg_state_t state;
        uint32_t age_ms = 0;
        bool have = link_get_latest_state(&state, &age_ms);

        wifeel_msg_devices_t devices;
        uint32_t devices_age_ms = 0;
        bool have_devices = link_get_latest_devices(&devices, &devices_age_ms);

        if (!bsp_amoled_lvgl_lock(100)) {
            continue; /* UI busy this tick, try again next time rather than block */
        }

        ui_phones_update(have_devices, &devices, devices_age_ms);

        if (!have) {
            lv_obj_set_style_bg_color(s_indicator, lv_color_hex(0x3A4750), LV_PART_MAIN);
            lv_label_set_text(s_score_label, "--");
            lv_label_set_text(s_link_label, "waiting for hub...");
        } else if (age_ms > LINK_STALE_MS) {
            lv_obj_set_style_bg_color(s_indicator, lv_color_hex(0x3A4750), LV_PART_MAIN);
            lv_label_set_text(s_score_label, "--");
            lv_label_set_text_fmt(s_link_label, "link lost (%" PRIu32 "s ago)", age_ms / 1000);
        } else {
            uint32_t color = state.motion_flag ? COLOR_S3 : 0x3DAA6E /* green, "still" */;
            lv_obj_set_style_bg_color(s_indicator, lv_color_hex(color), LV_PART_MAIN);
            lv_label_set_text_fmt(s_score_label, "%u", state.motion_score);
            lv_label_set_text(s_network_label, state.ssid[0] ? state.ssid : "(hub not connected)");
            lv_label_set_text_fmt(s_link_label, "hub fw %s  \xc2\xb7  %" PRIu32 "ms ago",
                                   state.fw_version, age_ms);

            lv_chart_set_next_value(s_chart, s_chart_s1, state.motion_score_streams[WIFEEL_STREAM_ROUTER_TO_HUB]);
            lv_chart_set_next_value(s_chart, s_chart_s3, state.motion_score_streams[WIFEEL_STREAM_DISPLAY_TO_HUB]);
        }

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
    esp_err_t touch_err = touch_init();
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "touch_init failed: %s (continuing without touch)", esp_err_to_name(touch_err));
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " WiFeel display  fw=%s  target=%s", WIFEEL_DISPLAY_FW_VERSION, CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, " free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "==================================================");

    if (bsp_amoled_lvgl_lock(-1)) {
        build_screen();
        bsp_amoled_lvgl_unlock();
    }

    esp_err_t link_err = link_init();
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "link_init failed: %s", esp_err_to_name(link_err));
    }

    xTaskCreate(&ui_update_task, "ui_update", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
}
