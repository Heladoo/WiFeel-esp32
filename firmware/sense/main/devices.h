/**
 * Nearby-device tracking (phones seen over BLE and Wi-Fi).
 *
 * Privacy boundary: raw MAC addresses stay in this board's RAM only. They
 * are never written to NVS, never sent over ESP-NOW, and never printed —
 * everything outside the hub identifies a device by devices_id_hash(),
 * which uses a salt that changes on every boot.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Seeds the per-boot hash salt. Call after Wi-Fi is started (esp_random()
 *  is only truly random once the RF subsystem is running). */
esp_err_t devices_init(void);

/** 16-bit id for a MAC address, stable only until the next reboot. */
uint16_t devices_id_hash(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
