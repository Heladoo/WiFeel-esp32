/**
 * Link, display side: joins the hub's own SoftAP as a plain Wi-Fi station
 * (SSID/password: WIFEEL_LINK_AP_SSID in wifeel_proto.h — see
 * firmware/sense/main/wifi_mgr.h for why: real STA association produces
 * data-frame traffic the hub can CSI-tap, unlike raw ESP-NOW broadcasts,
 * which don't produce CSI at all on this hardware), then listens for
 * wifeel_msg_state_t broadcasts from the hub over ESP-NOW on that same
 * link.
 *
 * Also starts pinging the hub once connected — same reasoning as
 * firmware/sense/main/ping_gw.h: the hub's CSI capture on this link (S3)
 * needs traffic to tap, an idle association alone isn't enough.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Brings up Wi-Fi in STA mode, joins the hub's SoftAP (auto-reconnects
 *  on disconnect), starts pinging it once connected, and starts listening
 *  for STATE broadcasts. Non-blocking — connection happens in the
 *  background; link_get_latest_state() simply has nothing to report until
 *  the first STATE message arrives. */
esp_err_t link_init(void);

/**
 * Copies the most recently received STATE into *out. Returns false if
 * nothing has ever been received. `*age_ms_out` (if non-NULL) is set to
 * how long ago it arrived — the caller decides what counts as stale.
 */
bool link_get_latest_state(wifeel_msg_state_t *out, uint32_t *age_ms_out);

#ifdef __cplusplus
}
#endif
