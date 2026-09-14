/**
 * ESP-NOW link, hub side: broadcasts a wifeel_msg_state_t (wifeel_proto.h)
 * at 1Hz carrying live sensing results to the display.
 *
 * Scoped down from the plan's full link design for now: broadcast (not
 * paired unicast), no discovery/HELLO handshake, and a hardcoded channel
 * matching wherever the hub is currently associated — good enough to get
 * real data on the display's screen; replace with proper pairing +
 * channel-follow once the display's own network picker exists (milestone
 * 5). Only motion (P1) is real right now; every other STATE field is a
 * documented placeholder — see the comments in link_task().
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts broadcasting STATE messages. Call after esp_wifi has been
 *  started (wifi_mgr_init()) and after motion_init(). */
esp_err_t link_init(void);

#ifdef __cplusplus
}
#endif
