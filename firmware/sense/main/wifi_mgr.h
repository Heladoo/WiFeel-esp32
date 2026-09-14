#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One-time setup: netif, default event loop, Wi-Fi driver in STA mode,
 *  power-save disabled (per the plan: PS off, so CSI/traffic isn't gated
 *  by modem sleep). Call once from app_main() before wifi_mgr_join(). */
esp_err_t wifi_mgr_init(void);

/**
 * Join `ssid` (with `password`, or NULL/empty for an open network) and
 * block until an IP address is obtained or `timeout_ms` elapses. This is
 * the JOIN source mode (see wifeel_proto.h) — PASSIVE and DIRECT modes
 * don't call this at all (source.c, milestone 5, will route between them).
 *
 * Safe to call again to switch networks; disconnects first if already
 * connected.
 */
esp_err_t wifi_mgr_join(const char *ssid, const char *password, uint32_t timeout_ms);

void wifi_mgr_disconnect(void);

/** True once an IP has been obtained and no disconnect has happened since. */
bool wifi_mgr_is_connected(void);

/** Fails with ESP_ERR_WIFI_NOT_CONNECT if wifi_mgr_is_connected() is false. */
esp_err_t wifi_mgr_get_ap_bssid(uint8_t bssid_out[6]);
esp_err_t wifi_mgr_get_ap_channel(uint8_t *channel_out);
esp_err_t wifi_mgr_get_gateway_ip(esp_ip4_addr_t *gw_out);

#ifdef __cplusplus
}
#endif
