/**
 * Nearby-device tracking (phones seen over BLE and Wi-Fi).
 *
 * Privacy boundary: raw MAC addresses stay in this board's RAM only. They
 * are never written to NVS, never sent over ESP-NOW, and never printed —
 * everything outside the hub identifies a device by devices_id_hash(),
 * which uses a salt that changes on every boot.
 *
 * Radio callbacks (ble_scan.c, and wifi_sniff.c once it exists) call
 * devices_observe() to report a sighting; it queues the observation and
 * returns immediately rather than touching the device table directly, so
 * a radio ISR/task is never blocked on table bookkeeping.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Seeds the per-boot hash salt and starts the background tracking task.
 *  Call after Wi-Fi is started (esp_random() is only truly random once the
 *  RF subsystem is running) and after NVS is initialized. */
esp_err_t devices_init(void);

/** 16-bit id for a MAC address, stable only until the next reboot. */
uint16_t devices_id_hash(const uint8_t mac[6]);

/** Reports one sighting with a usable RSSI reading — updates the entry's
 *  smoothed distance estimate. Non-blocking — drops the observation if the
 *  internal queue is full rather than stalling the calling radio task.
 *  `phone_like` is the caller's own classifier verdict (e.g.
 *  ble_scan_classify()'s return value) and feeds ble_phone_count in the
 *  summary; it's ignored for WIFEEL_DEV_SRC_WIFI, which doesn't have a
 *  phone/not-phone signal yet. `connected` is only meaningful for
 *  WIFEEL_DEV_SRC_WIFI; pass false for BLE. */
void devices_observe(wifeel_dev_source_t source, const uint8_t mac[6],
                      wifeel_vendor_t vendor, bool phone_like, int8_t rssi, bool connected);

/**
 * Reports that a device is still present WITHOUT a usable RSSI reading —
 * refreshes last-seen/connected only, leaves distance untouched. For
 * wifi_sniff.c's AP-to-client frames: the RSSI measured on those is the
 * AP's signal, not the client's, so applying it would corrupt the
 * client's distance estimate even though the frame does prove it's still
 * there.
 */
void devices_touch(wifeel_dev_source_t source, const uint8_t mac[6], bool connected);

/** Fills *out with the current summary (counts + up to
 *  WIFEEL_DEVICES_MAX_ENTRIES nearest entries) for the STATE-like
 *  broadcast and the `phones` console command. */
void devices_get_summary(wifeel_msg_devices_t *out);

/**
 * Starts a distance-calibration window: hold the reference device (e.g. a
 * phone) 1m from the hub for `duration_ms`, then whichever tracked entry
 * of `source` has the strongest (least negative) smoothed RSSI when the
 * window ends is taken as the 1m reference and saved to NVS.
 */
esp_err_t devices_calibrate_start(wifeel_dev_source_t source, uint32_t duration_ms);

/** Same as devices_calibrate_start(), but restricts the "strongest wins"
 *  search to entries classified as `vendor_filter` (pass WIFEEL_VENDOR_COUNT
 *  for no filter — devices_calibrate_start()'s behavior). For calibrating
 *  against one specific device on a desk with several BLE sources active
 *  at once, where an unfiltered calibration can otherwise lock onto
 *  whichever *other* device happens to be strongest during the window
 *  instead of the one actually being held at 1m. */
esp_err_t devices_calibrate_start_filtered(wifeel_dev_source_t source, uint32_t duration_ms,
                                            wifeel_vendor_t vendor_filter);
bool devices_calibrate_is_active(void);
uint32_t devices_calibrate_remaining_ms(void);

/**
 * Discards a previous calibration and restores the documented default
 * 1m reference for `source` (persisted to NVS, same as a normal
 * calibration would). For undoing a bad calibration — e.g. one taken
 * with the reference device essentially touching the antenna rather
 * than genuinely 1m away, which reads as a much-too-strong RSSI and
 * makes every real device at normal room distance compute as far
 * beyond its actual range (seen live: BLE calibrated to -29dBm made a
 * phone sitting <1m away read as "unknown").
 */
void devices_calibrate_reset(wifeel_dev_source_t source);

/** Current 1m reference RSSI per source (dBm) and the shared path-loss
 *  exponent used for distance estimates — for `phones`/status display. */
float devices_get_ref_1m(wifeel_dev_source_t source);
float devices_get_path_loss_exponent(void);

#ifdef __cplusplus
}
#endif
