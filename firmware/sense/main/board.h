#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Bump on every behavior change — printed in the boot banner and reported
 *  in wifeel_msg_state_t.fw_version so the display and serial logs always
 *  show what's actually running (see CLAUDE.md's flashing conventions). */
#define WIFEEL_SENSE_FW_VERSION "0.1.0-dev"

/**
 * Board-specific bring-up: on the Seeed XIAO ESP32-C6, selects the
 * external u.FL antenna via the onboard RF switch (GPIO3 LOW enables RF
 * switch control, GPIO14 HIGH selects external over the ceramic antenna —
 * see CLAUDE.md's pin reference). Must run before esp_wifi_init() /
 * esp_wifi_start() or the radio comes up on the wrong antenna.
 *
 * No-op (returns ESP_OK) on targets other than esp32c6, so this firmware
 * still builds for a plain esp32c6-devkitc if one is ever used for bench
 * testing without a XIAO board.
 */
esp_err_t board_init(void);

/** True if board_init() selected the external antenna (always true on the
 *  XIAO C6 target; false elsewhere) — surfaced in the boot banner per the
 *  plan's milestone 4 check ("boot banner shows antenna = external"). */
bool board_has_external_antenna(void);
