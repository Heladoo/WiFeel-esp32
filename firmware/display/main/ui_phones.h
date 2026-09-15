/**
 * "Phones" tile: nearby-device summary, smartwatch-vitals styled — a big
 * ring gauge for the headline metric, small icon tiles for secondary
 * counts, icon-led rows for individual devices. Deliberately light on
 * text (see app_main.c's build_home_screen() for the sibling Home tile's
 * plainer style) — vendor is a colored dot, source is a Bluetooth/Wi-Fi
 * glyph, "on home Wi-Fi" is implied by which icon a row uses rather than
 * a separate label.
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
