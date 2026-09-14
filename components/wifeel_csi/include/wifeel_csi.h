/**
 * WiFeel shared CSI capture and feature extraction.
 *
 * Used on both boards: the hub runs it for its own streams (S1 router->hub,
 * S3 display->hub), and the display runs the SAME code for its streams (S2
 * router->display, S4 hub->display) before sending the results to the hub
 * as a wifeel_msg_features_t (see wifeel_proto.h) — raw CSI never leaves
 * either board.
 *
 * Wraps ESP-IDF's esp_wifi CSI callback (wifi_csi_info_t, from
 * esp_wifi_types.h) so callers never touch the raw radio struct directly:
 * register one wifeel_csi_stream_t per source MAC you care about, feed it
 * from a single esp_wifi_set_csi_rx_cb() dispatcher, and pull features out
 * on a timer.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Target grid rate for resampled amplitude data. Chosen to comfortably
 *  cover the 0.1-0.6 Hz breathing band (Nyquist >> 0.6 Hz) while keeping
 *  the per-stream ring buffer small on the no-PSRAM display board. */
#define WIFEEL_CSI_GRID_HZ        20
#define WIFEEL_CSI_WINDOW_SECONDS 2
#define WIFEEL_CSI_RING_LEN       (WIFEEL_CSI_GRID_HZ * WIFEEL_CSI_WINDOW_SECONDS)

/** Number of subcarrier groups features.subcarrier_variance is bucketed
 *  into (see wifeel_msg_features_t) — kept small so the ESP-NOW payload
 *  and RAM footprint stay bounded regardless of how many subcarriers the
 *  underlying PHY reports (HT20 vs HE20 differ). */
#define WIFEEL_CSI_SUBCARRIER_GROUPS 8

/** One registered CSI source: a physical stream (S1-S4) identified by the
 *  peer's MAC address. Opaque — create with wifeel_csi_stream_init(),
 *  never construct directly (its internal buffers depend on build config). */
typedef struct wifeel_csi_stream wifeel_csi_stream_t;

typedef struct {
    wifeel_stream_id_t id;
    uint8_t peer_mac[6];      /* source MAC to accept frames from */
    bool    has_baseline;     /* set once wifeel_csi_calibrate() has run */
} wifeel_csi_stream_config_t;

/**
 * Allocate and initialize a stream tracker. Does not touch the radio —
 * call this once per stream you want to track (e.g. twice on the hub, for
 * S1 and S3), then feed it CSI frames via wifeel_csi_stream_feed() from
 * your esp_wifi_set_csi_rx_cb() callback.
 *
 * Returns NULL on allocation failure.
 */
wifeel_csi_stream_t *wifeel_csi_stream_init(const wifeel_csi_stream_config_t *config);

void wifeel_csi_stream_deinit(wifeel_csi_stream_t *stream);

/**
 * Feed one raw CSI frame to a stream. Call this for every frame your
 * esp_wifi_set_csi_rx_cb() callback receives that you've dispatched to
 * this stream (match `info->mac` against the stream's configured
 * peer_mac before calling — this function does not filter for you, so
 * one dispatcher can fan a single callback out to several streams).
 *
 * Cheap and safe to call from the Wi-Fi task/ISR context the CSI
 * callback runs in: only copies amplitude data into a lock-free ring
 * buffer, no floating point or allocation happens here. Feature
 * extraction (wifeel_csi_stream_get_features()) is the heavier pass and
 * should be called from a normal task instead.
 */
void wifeel_csi_stream_feed(wifeel_csi_stream_t *stream, const wifi_csi_info_t *info);

/**
 * Resets the stream's baseline to the amplitude/variance levels observed
 * over the next `duration_ms` of fed frames (call this, then keep feeding
 * frames for that long with the room empty — see the plan's presence
 * calibration flow). Non-blocking: this only arms the calibration window;
 * wifeel_csi_stream_is_calibrating() reports when it's done.
 */
esp_err_t wifeel_csi_stream_calibrate_start(wifeel_csi_stream_t *stream, uint32_t duration_ms);
bool      wifeel_csi_stream_is_calibrating(const wifeel_csi_stream_t *stream);

/**
 * Call once duration_ms (as passed to wifeel_csi_stream_calibrate_start)
 * has elapsed on the caller's own timer, to fold the accumulated sums into
 * the stream's baseline and clear the in-progress flag. This component
 * intentionally owns no FreeRTOS timer itself — the presence-calibration
 * UX (countdown, cancel, "please leave the room" prompt) belongs to
 * firmware/sense's presence.c, not here.
 */
void wifeel_csi_stream_calibrate_finish(wifeel_csi_stream_t *stream);

/**
 * Compute a wifeel_msg_features_t snapshot from the stream's current 2s
 * window (see WIFEEL_CSI_WINDOW_SECONDS). Safe to call from a normal task
 * on a timer (the plan uses a 1s hop). Returns false if the stream hasn't
 * accumulated a full window yet (e.g. right after init/reconnect) — in
 * that case *out is left untouched and callers should skip this cycle.
 */
bool wifeel_csi_stream_get_features(const wifeel_csi_stream_t *stream, wifeel_msg_features_t *out);

/** Frames actually accepted into this stream in the last 1s window used by
 *  wifeel_csi_stream_get_features()'s pkt_rate — exposed separately so
 *  link-health UI (Status page) can read it without recomputing features. */
float wifeel_csi_stream_get_pkt_rate(const wifeel_csi_stream_t *stream);

/**
 * Fast-reacting jitter estimate for motion detection (P1), updated once
 * per real sample rather than over wifeel_csi_stream_get_features()'s
 * multi-sample ring window (at real, sparse CSI arrival rates — a few Hz,
 * not the nominal 20Hz grid — that window can span many seconds, too slow
 * to react to someone walking by).
 *
 * This is an EMA of the change in raw mean amplitude between consecutive
 * samples. A gain-invariant alternative (spatial coefficient of variation
 * across subcarriers, matching francescopace/espectre's documented
 * approach) was tried and found to give zero response to confirmed real
 * motion in live testing on this hardware — see wifeel_csi.c's
 * bucket_finalize_and_push() and docs/boards.md for the full story. This
 * plain amplitude-diff version is the one with actual positive evidence
 * of detecting real walk-by motion; callers needing gain-invariance
 * should track their own adaptive baseline (see motion.c) rather than
 * assume this value is normalized.
 *
 * Returns 0 before the first two samples have arrived.
 */
float wifeel_csi_stream_get_fast_jitter(const wifeel_csi_stream_t *stream);

#ifdef __cplusplus
}
#endif
