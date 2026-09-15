#include "ui_phones.h"

#include <stdio.h>

#define PHONES_MAX_ROWS 6

/* Reuses the app's existing palette (see app_main.c's COLOR_S1/COLOR_S3)
 * so a vendor's dot reads as part of the same system rather than a new
 * ad-hoc color set. */
#define COLOR_BG         0x101418
#define COLOR_HEADLINE   0x4FA8E8 /* matches COLOR_S1 */
#define COLOR_WIFI_TILE  0xE8A33D /* matches COLOR_S3 */
/* Was 0x5A6B73 (~3.3:1 against COLOR_BG) — a UX pass flagged it below
 * WCAG AA's 4.5:1 for body text, and different from Home tile's own
 * "muted" color (0x8FA3AD, ~7:1) despite meaning the same thing. Unified
 * on the more legible value. */
#define COLOR_MUTED      0x8FA3AD
#define COLOR_TEXT       0xE8EEF2
#define COLOR_VENDOR_APPLE     0xE8EEF2
#define COLOR_VENDOR_SAMSUNG   0x4FA8E8
#define COLOR_VENDOR_GOOGLE    0x3DAA6E
#define COLOR_VENDOR_MICROSOFT 0xE8A33D
/* Other and Unknown share COLOR_MUTED (was 0x5A6B73 and 0x3A4750 — the
 * latter computes to ~1.9:1 contrast, nearly unreadable, which undercut
 * the whole point of spelling out "Unknown" instead of "?" per live
 * feedback). Both mean "no confident classification," so sharing one
 * legible color is fine — nothing distinguishes them but the label text
 * anyway. */
#define COLOR_VENDOR_OTHER      COLOR_MUTED
#define COLOR_VENDOR_UNKNOWN    COLOR_MUTED

static lv_obj_t *s_headline_icon;
static lv_obj_t *s_headline_count;
static lv_obj_t *s_headline_caption;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_count;
static lv_obj_t *s_wifi_caption;
static lv_obj_t *s_empty_label;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *type_icon;
    lv_obj_t *vendor_label;
    lv_obj_t *range_label;
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

/* Short label, not just a color — a bare colored dot needs a legend to mean
 * anything (reported live: "different color which i dont know their
 * meanings"). "Unknown" spelled out rather than "?" (reported live: "dont
 * show '?' show Unknown or similar") — this is the classifier's honest
 * answer when an advert/frame carried no manufacturer data we recognize,
 * not an error. */
static const char *vendor_label_text(uint8_t vendor)
{
    switch ((wifeel_vendor_t)vendor) {
        case WIFEEL_VENDOR_APPLE:     return "Apple";
        case WIFEEL_VENDOR_SAMSUNG:   return "Samsung";
        case WIFEEL_VENDOR_GOOGLE:    return "Google";
        case WIFEEL_VENDOR_MICROSOFT: return "MSFT";
        case WIFEEL_VENDOR_OTHER:     return "Other";
        default:                      return "Unknown";
    }
}

/* Which device-type glyph to draw. LVGL's built-in symbol set has no
 * literal phone/PC icons, so this leans on the closest available meaning:
 * a handset for "phone" (matches the headline stat's own icon), a keyboard
 * for "computer" (Microsoft vendor entries are Windows PCs — Swift Pair
 * BLE beacons today; Wi-Fi-sourced entries can't reach this case yet, see
 * phone_like's doc comment in wifeel_proto.h), and a generic connector
 * glyph for anything else — including every Wi-Fi-sourced entry today,
 * since there's no Wi-Fi vendor/type classifier yet (wifi_sniff.c). */
static const char *type_icon_symbol(uint8_t vendor, uint8_t phone_like)
{
    if (phone_like) {
        return LV_SYMBOL_CALL;
    }
    if ((wifeel_vendor_t)vendor == WIFEEL_VENDOR_MICROSOFT) {
        return LV_SYMBOL_KEYBOARD;
    }
    return LV_SYMBOL_USB;
}

/* Distance is only reliable to a rough zone even after calibration (log-
 * distance path loss from a single RSSI sample is noisy) — showing a bare
 * "0.0m" for anything closer than ~1m read as broken (reported live:
 * "currently distance show 0 for all"), so the closest zone is spelled out
 * instead of rounded to a misleadingly precise number. */
static void range_text(char *out, size_t out_len, uint8_t distance_dm)
{
    if (distance_dm == 255) {
        snprintf(out, out_len, "Unknown");
    } else if (distance_dm < 10) {
        snprintf(out, out_len, "<1m");
    } else if (distance_dm < 30) {
        snprintf(out, out_len, "%.1fm", (double)distance_dm / 10.0);
    } else if (distance_dm < 60) {
        snprintf(out, out_len, "3-6m");
    } else {
        snprintf(out, out_len, "6m+");
    }
}

static void set_headline_dimmed(bool dimmed)
{
    lv_opa_t opa = dimmed ? LV_OPA_40 : LV_OPA_COVER;
    lv_obj_set_style_opa(s_headline_icon, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_headline_count, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_wifi_icon, opa, LV_PART_MAIN);
    lv_obj_set_style_opa(s_wifi_count, opa, LV_PART_MAIN);
}

void ui_phones_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);

    /* Headline: nearby *phones* only (ble_phone_count already excludes
     * non-phone BLE devices — PCs, accessories, unclassified — on the hub
     * side; see devices.c). Plain icon + number, no gauge: reported live,
     * "phone indicator - no need for pi chart." */
    s_headline_icon = lv_label_create(parent);
    lv_label_set_text(s_headline_icon, LV_SYMBOL_CALL);
    lv_obj_set_style_text_color(s_headline_icon, lv_color_hex(COLOR_HEADLINE), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_headline_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_headline_icon, LV_ALIGN_CENTER, -46, -172);

    s_headline_count = lv_label_create(parent);
    lv_label_set_text(s_headline_count, "--");
    lv_obj_set_style_text_color(s_headline_count, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_headline_count, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(s_headline_count, LV_ALIGN_CENTER, 4, -180);

    s_headline_caption = lv_label_create(parent);
    lv_label_set_text(s_headline_caption, "nearby phones");
    lv_obj_set_style_text_color(s_headline_caption, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_headline_caption, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_headline_caption, LV_ALIGN_CENTER, 0, -128);

    /* Secondary stat: devices confirmed on the home Wi-Fi network — kept as
     * its own number rather than added to the headline, since the two
     * sources can't be correlated to the same physical device (see
     * devices.h's privacy-boundary comment), and it isn't phone-filtered
     * (any Wi-Fi client counts, not just phones). */
    s_wifi_icon = lv_label_create(parent);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COLOR_WIFI_TILE), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_wifi_icon, LV_ALIGN_CENTER, -30, -90);

    s_wifi_count = lv_label_create(parent);
    lv_label_set_text(s_wifi_count, "--");
    lv_obj_set_style_text_color(s_wifi_count, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_count, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_wifi_count, LV_ALIGN_CENTER, 4, -92);

    s_wifi_caption = lv_label_create(parent);
    lv_label_set_text(s_wifi_caption, "on Wi-Fi");
    lv_obj_set_style_text_color(s_wifi_caption, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_caption, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_wifi_caption, LV_ALIGN_CENTER, 0, -58);

    /* All tracked devices, closest first (see devices.c's distance-ranked
     * sort) — each row: type glyph (phone/computer/other), vendor, and a
     * range zone. The source glyph (BLE vs Wi-Fi) that used to sit before
     * the type icon was dropped per live feedback: "too small to
     * understand anyways." */
    static const int16_t row_y0 = -18;
    static const int16_t row_h = 30;
    for (int i = 0; i < PHONES_MAX_ROWS; i++) {
        row_widgets_t *r = &s_rows[i];
        r->row = lv_obj_create(parent);
        lv_obj_remove_style_all(r->row);
        lv_obj_set_size(r->row, 230, row_h);
        lv_obj_align(r->row, LV_ALIGN_CENTER, 0, row_y0 + i * row_h);

        r->type_icon = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->type_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(r->type_icon, LV_ALIGN_LEFT_MID, 0, 0);

        r->vendor_label = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->vendor_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(r->vendor_label, LV_ALIGN_LEFT_MID, 24, 0);

        r->range_label = lv_label_create(r->row);
        lv_obj_set_style_text_font(r->range_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(r->range_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
        lv_obj_align(r->range_label, LV_ALIGN_LEFT_MID, 128, 0);

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
    /* Same staleness convention as the Home tile (LINK_STALE_MS there) — a
     * hub that's gone quiet shouldn't leave a confidently-lit headline on
     * screen showing a number that's no longer true. */
    bool stale = !have || age_ms > 4000;
    set_headline_dimmed(stale);

    if (!have) {
        lv_label_set_text(s_headline_count, "--");
        lv_label_set_text(s_wifi_count, "--");
        for (int i = 0; i < PHONES_MAX_ROWS; i++) {
            lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text_fmt(s_headline_count, "%u", devices->ble_phone_count);
    lv_label_set_text_fmt(s_wifi_count, "%u", devices->wifi_client_count);

    uint8_t n = devices->n_entries > PHONES_MAX_ROWS ? PHONES_MAX_ROWS : devices->n_entries;
    for (uint8_t i = 0; i < n; i++) {
        const wifeel_msg_device_entry_t *e = &devices->entries[i];
        row_widgets_t *r = &s_rows[i];

        const char *ticon = type_icon_symbol(e->vendor, e->phone_like);
        lv_label_set_text(r->type_icon, ticon);
        lv_obj_set_style_text_color(r->type_icon, lv_color_hex(vendor_color(e->vendor)), LV_PART_MAIN);

        lv_label_set_text(r->vendor_label, vendor_label_text(e->vendor));
        lv_obj_set_style_text_color(r->vendor_label, lv_color_hex(vendor_color(e->vendor)), LV_PART_MAIN);

        char range[12];
        range_text(range, sizeof(range), e->distance_dm);
        lv_label_set_text(r->range_label, range);

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
