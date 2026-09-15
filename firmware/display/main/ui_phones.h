/**
 * "Phones" tile: nearby-device summary, smartwatch-vitals styled — a big
 * headline number (phones only) and a secondary Wi-Fi-clients number, both
 * plain icon+number stats (no gauge — see app_main.c's build_home_screen()
 * for the sibling Home tile's similar plain-number style), then a table of
 * every tracked device sorted closest-first: source glyph (BLE/Wi-Fi),
 * type glyph (phone/computer/other), vendor, and a range zone.
 */
#pragma once

#include "lvgl.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Builds the tile's widgets under `parent` (one tile of the home
 *  lv_tileview — see app_main.c). Call once, holding the LVGL lock. */
void ui_phones_create(lv_obj_t *parent);

/** Refreshes the tile from the latest WIFEEL_MSG_DEVICES broadcast.
 *  `have` false means none has ever arrived; age_ms is only meaningful
 *  when `have` is true. Call periodically, holding the LVGL lock. */
void ui_phones_update(bool have, const wifeel_msg_devices_t *devices, uint32_t age_ms);

#ifdef __cplusplus
}
#endif
