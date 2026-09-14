#pragma once

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

#ifdef __cplusplus
}
#endif
