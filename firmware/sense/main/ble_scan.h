/**
 * Passive BLE scanning for nearby phones (NimBLE, observer role only).
 *
 * The hub only listens: it never sends scan requests, never advertises and
 * never connects. BLE shares the C6's single 2.4 GHz radio with Wi-Fi and
 * CSI capture, so scanning is duty-cycled (ble_scan_set_duty) and its cost
 * is measured as the drop in S1/S3 packet rate (see docs/boards.md).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Measured on HUB-1 (3 min each, docs/boards.md): S3 median pkt/s was
 * 100.0 at 0%, 100.0 at 10%, 99.0 at 25%, 96.7 at 50%. 25% is effectively
 * free for CSI and catches ~3.5 adverts/s. */
#define BLE_SCAN_DEFAULT_DUTY_PCT 25

/** Starts the NimBLE host and begins scanning at BLE_SCAN_DEFAULT_DUTY_PCT
 *  once the host syncs. Requires NVS to be initialized. */
esp_err_t ble_scan_init(void);

/** Percent of time the radio listens for adverts (0 pauses scanning). */
esp_err_t ble_scan_set_duty(uint8_t pct);
uint8_t ble_scan_get_duty(void);

/** Adverts received since boot (diagnostic). */
uint32_t ble_scan_adv_count(void);

/**
 * Classifies one advert's payload (AD structures, up to 31 bytes).
 * Returns true if it looks like a phone. *vendor is always set;
 * *apple_type (may be NULL) gets the first Apple Continuity message type,
 * or 0 if none.
 */
bool ble_scan_classify(const uint8_t *data, size_t len, wifeel_vendor_t *vendor, uint8_t *apple_type);

/** One captured advert, for `phones raw`. */
typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    uint8_t event_type;
    uint8_t len;
    uint8_t data[31];
} ble_scan_raw_adv_t;

/** Raw capture for fingerprinting real phones: start, pull adverts, stop.
 *  Only one capture at a time. */
esp_err_t ble_scan_raw_start(void);
bool ble_scan_raw_next(ble_scan_raw_adv_t *out, uint32_t timeout_ms);
void ble_scan_raw_stop(void);

#ifdef __cplusplus
}
#endif
