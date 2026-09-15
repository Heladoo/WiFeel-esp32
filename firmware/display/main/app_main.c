#include <inttypes.h>
#include <stdio.h>
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
#define CHART_WINDOW_S ((CHART_POINT_COUNT * UI_UPDATE_INTERVAL_MS) / 1000)

/* Mirrors firmware/sense/main/link.c's LINK_RATE_HZ — not a shared
 * constant (the two firmware images don't share main/ code, only
 * components/), just the same number, for the bottom status line. */
#define HUB_BROADCAST_HZ 3

/* Dark background for the whole app — set on the root screen and the
 * tileview itself, not just each tile, so no default-theme light
 * background can show through (e.g. during a swipe, or at the tileview's
 * own edges). */
#define COLOR_BG 0x101418

/* Same colors used for the S1/S3 legend text and chart series, so they
 * read as one system. */
#define COLOR_S1 0x4FA8E8 /* blue */
#define COLOR_S3 0xE8A33D /* amber — matches the "motion" stat's active color */
#define COLOR_GREEN 0x3DAA6E  /* "still" / "present" */
#define COLOR_GREY  0x3A4750  /* no data / empty */
#define COLOR_MUTED 0x8FA3AD
/* Presence's own "elevated" color (state MOTION) — deliberately NOT
 * COLOR_S3: that amber already means three other things on this screen
 * (the S3 chart series, and the motion stat's own active color), and a UX
 * pass flagged reusing it a fourth time for a different concept (presence
 * inferring motion vs the motion detector itself) as likely to blur
 * together for someone pattern-matching color to meaning at a glance. */
#define COLOR_PRESENCE_ACTIVE 0x3ABEBE /* teal */

static const char *TAG = "app_main";

/* UI objects the update task needs to keep touching — created once in
 * build_home_screen(), read/written only while holding the LVGL lock. */
static lv_obj_t *s_stat_phones_icon;
static lv_obj_t *s_stat_phones_count;
static lv_obj_t *s_stat_presence_icon;
static lv_obj_t *s_stat_presence_count;
static lv_obj_t *s_stat_motion_icon;
static lv_obj_t *s_stat_motion_count;
static lv_obj_t *s_network_label;
static lv_obj_t *s_link_label;
static lv_obj_t *s_legend_s1;
static lv_obj_t *s_legend_s3;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_chart_s1;
static lv_chart_series_t *s_chart_s3;

/** One "vitals" stat: icon above a number above a small caption, all
 *  vertically stacked off the icon so only the icon's own position needs
 *  to be checked against the round display's bezel (see build_home_screen()'s
 *  comment on the stat row's placement). Returns the icon and count labels
 *  via out params; the caption is static text, created but not kept. */
static void create_stat(lv_obj_t *parent, int16_t x, int16_t y, const char *icon,
                         const char *caption, lv_obj_t **icon_out, lv_obj_t **count_out)
{
    lv_obj_t *icon_label = lv_label_create(parent);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_CENTER, x, y);

    lv_obj_t *count_label = lv_label_create(parent);
    lv_label_set_text(count_label, "--");
    lv_obj_set_style_text_color(count_label, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align_to(count_label, icon_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    lv_obj_t *caption_label = lv_label_create(parent);
    lv_label_set_text(caption_label, caption);
    lv_obj_set_style_text_color(caption_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(caption_label, count_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    *icon_out = icon_label;
    *count_out = count_label;
}

/** Builds the Home tile's widgets under `home_tile` (one tile of the
 *  top-level lv_tileview — see build_screen()). Three top "vitals" stats
 *  (phones nearby, presence, motion) replace the old single big motion
 *  circle+score — reported live: "top indicators: number of phones BT,
 *  number of humen presence detected, is motion detected now." */
static void build_home_screen(lv_obj_t *home_tile)
{
    lv_obj_t *scr = home_tile;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Stat row sits close to the top of the round display — only the
     * icons' own y (-185) needs to clear the bezel width at that height;
     * each stat's number/caption cascade downward from its icon via
     * align_to, into progressively wider parts of the circle, so they
     * can't clip once the icon itself fits. */
    create_stat(scr, -110, -185, LV_SYMBOL_CALL, "phones", &s_stat_phones_icon, &s_stat_phones_count);
    create_stat(scr, 0, -185, LV_SYMBOL_EYE_OPEN, "presence", &s_stat_presence_icon, &s_stat_presence_count);
    create_stat(scr, 110, -185, LV_SYMBOL_SHUFFLE, "motion", &s_stat_motion_icon, &s_stat_motion_count);

    /* Trend chart: S1 (router->hub) and S3 (display->hub) motion scores
     * over the last CHART_WINDOW_S seconds, so it's visible which sensing
     * stream is actually driving a detection rather than just the fused
     * number. Grid lines + axis labels ("x/y grid units - time in sec,
     * amplitude values") and a live current-value per legend (reported
     * live) are added below. */
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 340, 170);
    lv_obj_align(s_chart, LV_ALIGN_CENTER, 0, 55);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(s_chart, CHART_POINT_COUNT);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 3, 3); /* quartile grid lines, both axes */
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x181E24), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR); /* hide per-point marker dots, keep the line */
    s_chart_s1 = lv_chart_add_series(s_chart, lv_color_hex(COLOR_S1), LV_CHART_AXIS_PRIMARY_Y);
    s_chart_s3 = lv_chart_add_series(s_chart, lv_color_hex(COLOR_S3), LV_CHART_AXIS_PRIMARY_Y);

    /* Axis labels, all four inset INSIDE the chart's own rectangle rather
     * than placed outside it (one corner each: TL/BL/BR, "0" nudged right
     * of "-Ns" so the two bottom-left labels don't overlap) — a UX pass
     * found the previous OUT_BOTTOM_* placement left under 2px of
     * clearance against the round bezel at this chart size; inset labels
     * are guaranteed safe wherever the chart itself already fits, since
     * they never extend past its already-verified bounding box. */
    lv_obj_t *y_top = lv_label_create(scr);
    lv_label_set_text(y_top, "100");
    lv_obj_set_style_text_color(y_top, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(y_top, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(y_top, s_chart, LV_ALIGN_TOP_LEFT, 2, 1);

    char oldest_label[8];
    snprintf(oldest_label, sizeof(oldest_label), "-%ds", CHART_WINDOW_S);
    lv_obj_t *x_left = lv_label_create(scr);
    lv_label_set_text(x_left, oldest_label);
    lv_obj_set_style_text_color(x_left, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(x_left, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(x_left, s_chart, LV_ALIGN_BOTTOM_LEFT, 2, -1);

    lv_obj_t *y_bottom = lv_label_create(scr);
    lv_label_set_text(y_bottom, "0");
    lv_obj_set_style_text_color(y_bottom, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(y_bottom, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(y_bottom, x_left, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_obj_t *x_right = lv_label_create(scr);
    lv_label_set_text(x_right, "now");
    lv_obj_set_style_text_color(x_right, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(x_right, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(x_right, s_chart, LV_ALIGN_BOTTOM_RIGHT, -2, -1);

    /* Legend, doubling as each trace's current value (reported live: "show
     * current value for each trace line") — text is rewritten every tick
     * in ui_update_task(). */
    s_legend_s1 = lv_label_create(scr);
    lv_label_set_text(s_legend_s1, "S1 router --");
    lv_obj_set_style_text_color(s_legend_s1, lv_color_hex(COLOR_S1), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_legend_s1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(s_legend_s1, s_chart, LV_ALIGN_OUT_TOP_LEFT, 0, -4);

    s_legend_s3 = lv_label_create(scr);
    lv_label_set_text(s_legend_s3, "S3 display --");
    lv_obj_set_style_text_color(s_legend_s3, lv_color_hex(COLOR_S3), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_legend_s3, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(s_legend_s3, s_chart, LV_ALIGN_OUT_TOP_RIGHT, 0, -4);

    /* Bottom grey status area: network, firmware version, refresh rate.
     * Anchored below the chart (which is itself already verified to fit
     * the round bezel) rather than to fixed screen coordinates. */
    s_network_label = lv_label_create(scr);
    lv_label_set_text(s_network_label, "");
    lv_obj_set_style_text_color(s_network_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_network_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_network_label, LV_LABEL_LONG_DOT); /* SSID can be up to 32 chars */
    lv_obj_set_width(s_network_label, 220);
    lv_obj_set_style_text_align(s_network_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(s_network_label, s_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);

    s_link_label = lv_label_create(scr);
    lv_label_set_text(s_link_label, "waiting for hub...");
    lv_obj_set_style_text_color(s_link_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_link_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(s_link_label, s_network_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
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

static void set_stat_color(lv_obj_t *icon, lv_obj_t *count, uint32_t color)
{
    lv_obj_set_style_text_color(icon, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(count, lv_color_hex(color), LV_PART_MAIN);
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

        /* Phones stat: nearby phones only — devices.c's ble_phone_count
         * already excludes non-phone BLE devices (PCs, accessories),
         * matching the Phones tile's own headline number. */
        if (have_devices && devices_age_ms <= LINK_STALE_MS) {
            lv_label_set_text_fmt(s_stat_phones_count, "%u", devices.ble_phone_count);
            set_stat_color(s_stat_phones_icon, s_stat_phones_count, COLOR_S1);
        } else {
            lv_label_set_text(s_stat_phones_count, "--");
            set_stat_color(s_stat_phones_icon, s_stat_phones_count, COLOR_GREY);
        }

        if (!have) {
            lv_label_set_text(s_stat_presence_count, "--");
            set_stat_color(s_stat_presence_icon, s_stat_presence_count, COLOR_GREY);
            lv_label_set_text(s_stat_motion_count, "--");
            set_stat_color(s_stat_motion_icon, s_stat_motion_count, COLOR_GREY);
            lv_label_set_text(s_link_label, "waiting for hub...");
        } else if (age_ms > LINK_STALE_MS) {
            lv_label_set_text(s_stat_presence_count, "--");
            set_stat_color(s_stat_presence_icon, s_stat_presence_count, COLOR_GREY);
            lv_label_set_text(s_stat_motion_count, "--");
            set_stat_color(s_stat_motion_icon, s_stat_motion_count, COLOR_GREY);
            lv_label_set_text_fmt(s_link_label, "link lost (%" PRIu32 "s ago)", age_ms / 1000);
        } else {
            /* Presence: 0/1, not a real headcount — people_count (P3) isn't
             * built yet (see firmware/sense/main/link.c, which sends it as
             * an explicit 0 placeholder). presence_state (P2) IS real and
             * calibrated, so this shows "is someone here" rather than
             * fabricating a number people_count can't yet back up. */
            bool present = state.presence_state != WIFEEL_PRESENCE_EMPTY;
            lv_label_set_text_fmt(s_stat_presence_count, "%u", present ? 1u : 0u);
            uint32_t presence_color = !present ? COLOR_GREY :
                (state.presence_state == WIFEEL_PRESENCE_MOTION ? COLOR_PRESENCE_ACTIVE : COLOR_GREEN);
            set_stat_color(s_stat_presence_icon, s_stat_presence_count, presence_color);

            lv_label_set_text_fmt(s_stat_motion_count, "%u", state.motion_score);
            uint32_t motion_color = state.motion_flag ? COLOR_S3 : COLOR_GREEN;
            set_stat_color(s_stat_motion_icon, s_stat_motion_count, motion_color);

            lv_label_set_text(s_network_label, state.ssid[0] ? state.ssid : "(hub not connected)");
            lv_label_set_text_fmt(s_link_label, "fw %s  \xc2\xb7  %uHz", state.fw_version, HUB_BROADCAST_HZ);

            uint8_t s1_val = state.motion_score_streams[WIFEEL_STREAM_ROUTER_TO_HUB];
            uint8_t s3_val = state.motion_score_streams[WIFEEL_STREAM_DISPLAY_TO_HUB];
            lv_label_set_text_fmt(s_legend_s1, "S1 router %u", s1_val);
            lv_label_set_text_fmt(s_legend_s3, "S3 display %u", s3_val);
            lv_chart_set_next_value(s_chart, s_chart_s1, s1_val);
            lv_chart_set_next_value(s_chart, s_chart_s3, s3_val);
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
