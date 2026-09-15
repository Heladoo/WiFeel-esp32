/**
 * P1 (highest priority): motion detection fused from TWO independent CSI
 * streams — S1 (router->hub) and S3 (display->hub, over the hub's own
 * SoftAP the display joins — see wifi_mgr.h) — each using
 * wifeel_csi_stream_get_fast_jitter(). See docs/boards.md's CSI findings:
 * real CSI arrives sparsely (a few Hz), so this works off the fast
 * per-sample EMA, not the slower ring-buffer window; and the fast-jitter
 * metric itself is a temporal amplitude-diff, not the gain-invariant
 * "turbulence" statistic that was tried and found unreliable on real
 * hardware.
 *
 * Each stream gets its own adaptive noise floor (a real motion spike is
 * transient and shouldn't drag the floor up with it; only sustained
 * elevated jitter is treated as the new baseline). The fused score is the
 * max of the two streams' scores — motion detected by either sensing
 * point counts, since S1 and S3 cover different physical areas of the
 * room (the router and the display are in different locations).
 *
 * Thresholds are first-pass estimates, not rigorously calibrated — see
 * the `motion` console command in console_cmds.c for live per-stream
 * readings while testing. Expect to revise MOTION_SCORE_DELTA_RANGE and
 * the hysteresis bounds with more data.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts a background task that polls both streams' fast jitter every
 *  MOTION_UPDATE_INTERVAL_MS and updates the fused score/flag. Call once,
 *  after csi_mgr_init(). */
esp_err_t motion_init(void);

/** 0-100, smoothed, fused across S1 and S3 (max of the two). 0 = no
 *  detected motion, 100 = strongest jitter (relative to its stream's own
 *  adaptive floor) seen so far on whichever stream is currently higher. */
uint8_t motion_get_score(void);

/** Hysteresis-debounced boolean on the fused score: true once it has
 *  crossed the enter threshold, stays true until it drops below the
 *  (lower) exit threshold. */
bool motion_get_flag(void);

/** The ENTER threshold (MOTION_ENTER_SCORE) motion_get_flag() turns true
 *  at — exposed so link.c can send it to the display instead of the
 *  display hardcoding a second copy of the constant. */
uint8_t motion_get_enter_threshold(void);

/** Per-stream scores (0-100, before fusion) — exposed for live tuning and
 *  to see which stream is actually driving a MOTION flag. */
uint8_t motion_get_s1_score(void);
uint8_t motion_get_s3_score(void);

/** S1's raw fast-jitter EMA value and adaptive floor — exposed for live
 *  tuning (the `motion` console command prints these). */
float motion_get_raw_jitter(void);
float motion_get_floor(void);

/** Same as above, for S3. Both read as 0 whenever the display isn't
 *  currently connected to the hub's SoftAP (S3 has no data then). */
float motion_get_s3_raw_jitter(void);
float motion_get_s3_floor(void);

#ifdef __cplusplus
}
#endif
