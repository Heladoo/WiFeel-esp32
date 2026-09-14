/**
 * TEMPORARY diagnostic: a plain ESP-NOW receiver (no CSI involved at all)
 * to check whether the hub receives another board's ESP-NOW broadcasts at
 * the protocol level — isolating "frames aren't reaching us" from "frames
 * reach us but CSI doesn't tap them" (see docs/boards.md's DIRECT-mode
 * findings).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initializes ESP-NOW (Wi-Fi must already be started, e.g. by wifi_mgr)
 *  and logs every received frame's source MAC + length. Does not touch
 *  the current Wi-Fi channel — call after 'sniff' has already tuned to
 *  the target channel. */
esp_err_t espnow_rx_test_start(void);

#ifdef __cplusplus
}
#endif
