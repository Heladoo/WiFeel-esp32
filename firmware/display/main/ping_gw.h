#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start pinging `target` continuously at ~1000/interval_ms Hz to generate
 * the traffic JOIN mode's CSI capture rides on (see the plan: "pings the
 * gateway at 100 Hz"). Only one session runs at a time — starting a new
 * one stops any previous target.
 *
 * This exists as a thin wrapper over lwIP's esp_ping component rather than
 * ICMP/raw sockets by hand, since esp_ping already handles the continuous
 * (count=0) session and per-reply callback bookkeeping we need.
 */
esp_err_t ping_gw_start(const esp_ip4_addr_t *target, uint32_t interval_ms);

void ping_gw_stop(void);

/** True between ping_gw_start() and ping_gw_stop() (or before the first
 *  start). Lets a caller-side watchdog tell "not supposed to be pinging
 *  right now" apart from "supposed to be pinging but isn't succeeding". */
bool ping_gw_is_running(void);

/**
 * Milliseconds since the last successful reply, or since ping_gw_start()
 * if none has arrived yet. Returns 0 if not currently running.
 *
 * Exists so a caller can detect a stalled link even when nothing ever
 * calls ping_gw_stop() for it: on this hardware, a dead SoftAP link has
 * been observed to leave esp_ping retrying (and failing) forever without
 * WIFI_EVENT_STA_DISCONNECTED ever firing — see firmware/display/main/
 * link.c's watchdog task, which polls this rather than trusting the
 * disconnect event alone.
 */
uint32_t ping_gw_ms_since_last_success(void);

#ifdef __cplusplus
}
#endif
