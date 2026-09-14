#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registered callback fires on EVERY successful connection — an explicit
 * wifi_mgr_join() call, but also ESP-IDF's own automatic reconnect using
 * credentials persisted in NVS from a previous join (which happens on
 * every boot once a network has been joined once, without any app code
 * asking for it). Register before wifi_mgr_init() so a same-boot
 * auto-reconnect can't fire before the callback is set. Call with NULL to
 * unregister. Only one callback is supported (no fan-out) — that's all
 * this firmware needs.
 */
typedef void (*wifi_mgr_connected_cb_t)(void);
void wifi_mgr_set_connected_cb(wifi_mgr_connected_cb_t cb);

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

/**
 * Controls whether the driver's WIFI_EVENT_STA_DISCONNECTED handler
 * automatically retries the connection (the default, true — needed for
 * ordinary JOIN-mode robustness). Set false before a diagnostic that needs
 * the radio to sit disconnected/unassociated on purpose (e.g. `sniff` in
 * console_cmds.c) — otherwise the handler fights the diagnostic by
 * reconnecting within milliseconds. Remember to set it back to true
 * afterward.
 */
void wifi_mgr_set_auto_reconnect(bool enabled);

/** True once an IP has been obtained and no disconnect has happened since. */
bool wifi_mgr_is_connected(void);

/** Fails with ESP_ERR_WIFI_NOT_CONNECT if wifi_mgr_is_connected() is false. */
esp_err_t wifi_mgr_get_ap_bssid(uint8_t bssid_out[6]);
esp_err_t wifi_mgr_get_ap_channel(uint8_t *channel_out);
esp_err_t wifi_mgr_get_gateway_ip(esp_ip4_addr_t *gw_out);

#ifdef __cplusplus
}
#endif
