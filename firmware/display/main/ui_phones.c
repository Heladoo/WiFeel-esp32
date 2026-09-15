#include "ui_phones.h"

#include <stdio.h>

#define PHONES_MAX_ROWS 6
#define RING_MAX_PHONES 5 /* gauge fills up by this count; more just shows full */

/* Reuses the app's existing palette (see app_main.c's COLOR_S1/COLOR_S3)
 * so a vendor's dot reads as part of the same system rather than a new
 * ad-hoc color set. */
#define COLOR_BG         0x101418
#define COLOR_RING       0x4FA8E8 /* matches COLOR_S1 */
#define COLOR_WIFI_TILE  0xE8A33D /* matches COLOR_S3 */
#define COLOR_TRACK      0x2A3238 /* gauge background track */
#define COLOR_MUTED      0x5A6B73 /* matches the Home title's muted color */
#define COLOR_TEXT       0xE8EEF2
#define COLOR_VENDOR_APPLE     0xE8EEF2
#define COLOR_VENDOR_SAMSUNG   0x4FA8E8
#define COLOR_VENDOR_GOOGLE    0x3DAA6E
#define COLOR_VENDOR_MICROSOFT 0xE8A33D
#define COLOR_VENDOR_OTHER     0x5A6B73
#define COLOR_VENDOR_UNKNOWN   0x3A4750

static lv_obj_t *s_ring;
static lv_obj_t *s_ring_icon;
static lv_obj_t *s_ring_count;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_count;
static lv_obj_t *s_empty_label;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *source_icon;
    lv_obj_t *vendor_label;
    lv_obj_t *distance_label;
} row_widgets_t;

static row_widgets_t s_rows[PHONES_MAX_ROWS];

static uint32_t vendor_color(uint8_t vendor)
{
    switch ((wifeel_vendor_t)vendor) {
        case WIFEEL_VENDOR_APPLE:     return COLOR_VENDOR_APPLE;
        case WIFEEL_VENDOR_SAMSUNG:   return COLOR_VENDOR_SAMSUNG;
        case WIFEEL_VENDOR_GOOGLE:    return COLOR_VENDOR_GOOGLE;
        case WIFEEL_VENDOR_MICROSOFT: return COLOR_VENDOR_MICROSOFT;
        case WIFEEL_VENDOR_OTHER:     return COLOR_VENDOR_OTHER;
        default:                      return COLOR_VENDOR_UNKNOWN;
    }
}

/* Short label, not just a color — a bare colored dot needs a legend to
 * mean anything (reported live: "different color which i dont know
 * their meanings"). This is the row's one bit of real text, standing in
 * for a per-brand icon LVGL's built-in symbol set doesn't have. */
static const char *vendor_abbrev(uint8_t vendor)
{
    switch ((wifeel_vendor_t)vendor) {
        case WIFEEL_VENDOR_APPLE:     return "Appl";
        case WIFEEL_VENDOR_SAMSUNG:   return "Sams";
        case WIFEEL_VENDOR_GOOGLE:    return "Gogl";
        case WIFEEL_VENDOR_MICROSOFT: return "MSFT";
        case WIFEEL_VENDOR_OTHER:     return "Othr";
        default:                      return "?";
    }
}

static void set_tile_dimmed(bool dimmed)
{
    lv_opa_t opa = dimmed ? LV_OPA_40 : LV_OPA_COVER;
    lv_obj_set_style_opa(s_ring, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_ring_icon, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_ring_count, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_wifi_icon, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_wifi_count, opa, LV_PART_MAIN);
}

void ui_phones_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);

    /* Ring gauge: the headline "vitals" stat (BLE phones nearby), styled
     * like a watch face complication rather than Home's plain filled
     * circle — deliberately different so the two tiles don't look like
     * the same screen. Fills up to RING_MAX_PHONES, then stays full. The
     * phone icon inside doubles as the page's own identifier, so there's
     * no separate header. */
    s_ring = lv_arc_create(parent);
    lv_obj_set_size(s_ring, 160, 160);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, -95);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, RING_MAX_PHONES);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_MAIN);

    s_ring_icon = lv_label_create(parent);
    lv_label_set_text(s_ring_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_ring_icon, lv_color_hex(COLOR_RING), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ring_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align_to(s_ring_icon, s_ring, LV_ALIGN_CENTER, 0, -32);

    s_ring_count = lv_label_create(parent);
    lv_label_set_text(s_ring_count, "--");
    lv_obj_set_style_text_color(s_ring_count, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ring_count, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align_to(s_ring_count, s_ring, LV_ALIGN_CENTER, 0, 8);

    /* Secondary stat: devices confirmed on the home Wi-Fi network — kept
     * as its own number rather than added to the ring's count, since the
     * two sources can't be correlated to the same physical device (see
     * devices.h's privacy-boundary comment). */
    s_wifi_icon = lv_label_create(parent);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_TILE), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_wifi_icon, LV_ALIGN_CENTER, -18, 32);

    s_wifi_count = lv_label_create(parent);
    lv_label_set_text(s_wifi_count, "--");
    lv_obj_set_style_text_color(s_wifi_count, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_count, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_wifi_count, LV_ALIGN_CENTER, 18, 30);

    /* Nearest devices, closest first — each row: source (Bluetooth/Wi-Fi
     * glyph), the vendor as a short colored label (a bare color dot isn't
     * self-explanatory — no per-brand icon exists in LVGL's symbol set,
     * so a short name is the closest thing to an icon here), and the
     * distance (no icon conveys a continuous number, so this is real
     * text too). */
    static const int16_t row_y0 = 78;
    static const int16_t row_h = 26;
    for (int i = 0; i < PHONES_MAX_ROWS; i++) {
        row_widgets_t *r = &s_rows[i];
        r->row = lv_obj_create(parent);
        lv_obj_remove_style_all(r->row);
        lv_obj_set_size(r->row, 200, row_h);
        lv_obj_align(r->row, LV_ALIGN_CENTER, 0, row_y0 + i * row_h);

        r->source_icon = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->source_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(r->source_icon, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
        lv_obj_align(r->source_icon, LV_ALIGN_LEFT_MID, 0, 0);

        r->vendor_label = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->vendor_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(r->vendor_label, LV_ALIGN_LEFT_MID, 24, 0);

        r->distance_label = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->distance_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(r->distance_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
        lv_obj_align(r->distance_label, LV_ALIGN_LEFT_MID, 90, 0);

        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }

    s_empty_label = lv_label_create(parent);
    lv_label_set_text(s_empty_label, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_empty_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, row_y0 + row_h);
}

void ui_phones_update(bool have, const wifeel_msg_devices_t *devices, uint32_t age_ms)
{
    /* Same staleness convention as the Home tile (LINK_STALE_MS there) —
     * a hub that's gone quiet shouldn't leave a confidently-lit ring on
     * screen showing a number that's no longer true. */
    bool stale = !have || age_ms > 4000;
    set_tile_dimmed(stale);

    if (!have) {
        lv_label_set_text(s_ring_count, "--");
        lv_label_set_text(s_wifi_count, "--");
        lv_arc_set_value(s_ring, 0);
        for (int i = 0; i < PHONES_MAX_ROWS; i++) {
            lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text_fmt(s_ring_count, "%u", devices->ble_phone_count);
    lv_label_set_text_fmt(s_wifi_count, "%u", devices->wifi_client_count);
    int32_t ring_val = devices->ble_phone_count > RING_MAX_PHONES ? RING_MAX_PHONES : devices->ble_phone_count;
    lv_arc_set_value(s_ring, ring_val);

    uint8_t n = devices->n_entries > PHONES_MAX_ROWS ? PHONES_MAX_ROWS : devices->n_entries;
    for (uint8_t i = 0; i < n; i++) {
        const wifeel_msg_device_entry_t *e = &devices->entries[i];
        row_widgets_t *r = &s_rows[i];

        lv_label_set_text(r->source_icon, e->source == WIFEEL_DEV_SRC_WIFI ? LV_SYMBOL_WIFI : LV_SYMBOL_BLUETOOTH);
        lv_label_set_text(r->vendor_label, vendor_abbrev(e->vendor));
        lv_obj_set_style_text_color(r->vendor_label, lv_color_hex(vendor_color(e->vendor)), LV_PART_MAIN);
        if (e->distance_dm == 255) {
            lv_label_set_text(r->distance_label, "?");
        } else {
            lv_label_set_text_fmt(r->distance_label, "%.1fm", (double)e->distance_dm / 10.0);
        }
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
    for (uint8_t i = n; i < PHONES_MAX_ROWS; i++) {
        lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }

    if (n == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
}
