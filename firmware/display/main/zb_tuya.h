/**
 * Mirrors WiFeel's state onto a Tuya Zigbee hub so it can be viewed remotely
 * in the Tuya / Smart Life app — the app/cloud provide the remote access and
 * auth, so nothing on the home network is exposed.
 *
 * The display board joins the user's existing Tuya hub as a Zigbee End Device
 * and presents three endpoints:
 *   EP1  On/Off light      "Motion"       on = motion detected
 *   EP2  On/Off light      "Presence"     on = someone present (not EMPTY)
 *   EP3  Dimmable light    "Motion score" brightness % = motion score 0-100
 * Tuya has no generic sensor-value tile for a homemade device, so the app shows
 * these as switches/a dimmer. The app can toggle them; those writes are ignored
 * and the true value is pushed back within a fraction of a second.
 *
 * Belongs on the display, never the hub: Zigbee (802.15.4) can't coexist with
 * a Wi-Fi SoftAP, which the hub runs. See experiments/zigbee_tuya_probe/ for
 * what it took to get a Tuya hub to adopt the device, and docs/boards.md.
 *
 * Pairing: put the hub into pairing mode ("Add device" in the app) within 10
 * minutes of boot. An already-paired display rejoins on its own. Long-press
 * the BOOT button (~6 s) to forget the pairing and open a fresh 10-minute
 * pairing window.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the Zigbee stack in its own task. Call once, after Wi-Fi is up
 *  (link_init()) — the two radios are configured to coexist. */
esp_err_t zb_tuya_init(void);

/**
 * Feeds the latest hub state. `fresh` false (never received, or the hub
 * went quiet) means "hold whatever the Tuya tiles last showed" rather than
 * push a guess — a hub outage frozen on the last value is still misleading,
 * but pushing "no motion" would be actively reassuring, which is worse.
 * Cheap and non-blocking; safe to call every UI tick.
 */
void zb_tuya_update(bool fresh, const wifeel_msg_state_t *state);

#ifdef __cplusplus
}
#endif
