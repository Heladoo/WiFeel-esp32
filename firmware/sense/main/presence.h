/**
 * P2: presence detection — is anyone in the room, moving or still.
 *
 * Motion (motion.c) alone can't do this: a person sitting reading barely
 * disturbs the fast-jitter signal motion.c watches. Presence instead
 * watches *wander* — sustained deviation of mean CSI amplitude from a
 * calibrated empty-room baseline (wifeel_msg_features_t.wander, from
 * wifeel_csi_stream_get_features()) — which stays elevated as long as a
 * body is physically present and reflecting/absorbing the signal
 * differently than an empty room, movement or not.
 *
 * Requires calibration (presence_calibrate_start()) with the room
 * actually empty before it means anything — wander reads 0 on an
 * uncalibrated stream (see wifeel_csi.h), which this module reports as
 * "not calibrated" (presence_is_calibrated()) rather than silently
 * treating an uncalibrated 0 as a confident EMPTY reading.
 *
 * Like motion.c, fuses S1 (router->hub) and S3 (display->hub) by taking
 * whichever stream has calibration data and the higher wander — same
 * "either sensing point counts" reasoning. Thresholds below are
 * first-pass estimates (see the plan's honesty conventions) — not yet
 * validated against a real "person sitting still" test, only reasoned
 * about from the wander metric's definition.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts a background task that polls motion + wander and updates the
 *  presence state. Call once, after motion_init() (presence defers to
 *  motion's flag for the MOTION state). */
esp_err_t presence_init(void);

/** EMPTY / PRESENT_STILL / MOTION (wifeel_proto.h). Hold-time debounced —
 *  see presence.c's PRESENCE_HOLD_MS. Reads EMPTY (not a distinct
 *  "unknown") whenever nothing is calibrated yet — check
 *  presence_is_calibrated() to tell the two apart. */
wifeel_presence_state_t presence_get_state(void);

/** True once at least one of S1/S3 has completed calibration. Until
 *  then, presence_get_state() can only report EMPTY or MOTION (never a
 *  meaningful PRESENT_STILL), since wander is undefined without a
 *  baseline. */
bool presence_is_calibrated(void);

/**
 * Starts empty-room calibration on every currently-tracked stream (S1,
 * and S3 if the display is connected). The room must actually be empty
 * for the whole duration — this sets the "normal, nobody home" baseline
 * that wander is measured against. Non-blocking; poll
 * presence_is_calibrating() / presence_calibrate_remaining_ms() for UI
 * progress. Safe to call again later to redo calibration (e.g. after
 * furniture moves).
 */
esp_err_t presence_calibrate_start(uint32_t duration_ms);
bool      presence_is_calibrating(void);
uint32_t  presence_calibrate_remaining_ms(void);

/** Fused wander value (max of S1/S3, whichever are calibrated) and each
 *  stream's own — exposed for live tuning, same pattern as motion.h. */
float presence_get_wander(void);
float presence_get_s1_wander(void);
float presence_get_s3_wander(void);

#ifdef __cplusplus
}
#endif
