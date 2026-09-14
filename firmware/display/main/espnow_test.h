/**
 * TEMPORARY diagnostic (validating the DIRECT-mode architecture before
 * building the real ESP-NOW link, milestone 5): broadcasts small ESP-NOW
 * frames at a fixed rate on a fixed channel, so the hub can point its
 * `sniff` console command at this board's MAC and check whether CSI
 * capture works reliably for ESP-NOW traffic between the two WiFeel
 * boards — unlike a STA's own AP traffic, neither board is "associated"
 * to the other, so this traffic should take the general promiscuous path
 * that JOIN mode's own-AP traffic conspicuously didn't (see docs/boards.md
 * and the plan's CSI findings).
 *
 * Replace with the real bidirectional ESP-NOW link (wifeel_proto-based
 * discovery, channel-follow, SET_SOURCE etc.) once this is validated.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sets Wi-Fi to STA mode on `channel`, brings up ESP-NOW, and starts
 *  broadcasting a small counter payload at `rate_hz`. */
esp_err_t espnow_test_start(uint8_t channel, uint32_t rate_hz);

#ifdef __cplusplus
}
#endif
