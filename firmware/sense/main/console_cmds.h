#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the esp_console REPL and register WiFeel's development commands.
 * Only `join` and `status` exist so far (milestone 4: hub bring-up).
 * `passive`, `direct`, `calib`, and `stream` (per the plan's console.c
 * design) are added alongside the source/presence/calibration logic they
 * depend on in later milestones — see CLAUDE.md / the plan for the order.
 */
esp_err_t console_start(void);

#ifdef __cplusplus
}
#endif
