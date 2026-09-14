/**
 * P1 (highest priority): motion detection from S1's fast jitter estimate
 * (wifeel_csi_stream_get_fast_jitter). See docs/boards.md's CSI findings —
 * real CSI arrives sparsely (a few Hz), so this works off the fast
 * per-sample EMA, not the slower ring-buffer window.
 *
 * Thresholds below are first-pass estimates, not calibrated — this is
 * meant to be tuned against real "walk past the hub" tests (see
 * `motion` console command in console_cmds.c for live raw+score
 * readings while testing). Expect to revise MOTION_SCORE_MAX_JITTER and
 * the hysteresis bounds once real data is in.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts a background task that polls S1's fast jitter every
 *  MOTION_UPDATE_INTERVAL_MS and updates the score/flag. Call once, after
 *  csi_mgr_init(). */
esp_err_t motion_init(void);

/** 0-100, smoothed. 0 = no detected motion, 100 = strongest jitter seen
 *  relative to MOTION_SCORE_MAX_JITTER. */
uint8_t motion_get_score(void);

/** Hysteresis-debounced boolean: true once score has crossed the enter
 *  threshold, stays true until it drops below the (lower) exit threshold. */
bool motion_get_flag(void);

/** The raw fast-jitter EMA value motion_get_score() is derived from —
 *  exposed for live tuning (the `motion` console command prints this). */
float motion_get_raw_jitter(void);

#ifdef __cplusplus
}
#endif
