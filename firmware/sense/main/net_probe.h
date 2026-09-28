/**
 * S1 traffic generator: makes the AP send us frames, so S1 has CSI to sample.
 *
 * WHY THIS EXISTS. CSI is extracted from RECEIVED frames. S1 (router->hub)
 * therefore only produces data when the AP actually transmits to us, and
 * ping_gw.c's outgoing ICMP is useless on its own — on the test network the
 * gateway never answers ICMP at all, leaving S1 at ~2-5 pkt/s scraped from
 * beacons and broadcasts (docs/boards.md). That is below
 * WIFEEL_CSI_GRID_HZ (20), which means every frame closes its own grid
 * bucket and no frame averaging happens, so single-frame measurement noise
 * passes straight into the motion metric. A host-side simulation of the real
 * pipeline put the crossover sharply at that 20 pkt/s line: below it, 4-38
 * false MOTION events per 10 minutes; at 50 pkt/s, 1; at 100, none.
 *
 * WHY SEVERAL METHODS. Which mechanism gets a reply out of a given router is
 * not knowable in advance — the one network we have refuses ICMP, others
 * refuse ARP from unexpected sources or run no DNS forwarder. So rather than
 * bet on one, this module implements a few and counts replies for each, and
 * the `probe` console command switches between them at runtime. Pick the
 * winner by measurement on the network you actually have.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef enum {
    NET_PROBE_METHOD_NONE = 0,
    /** UDP DNS query to the DHCP-provided resolver (falls back to the
     *  gateway). Any answer counts, NXDOMAIN included — we want the reply
     *  frame, not the record. Usually the best bet: a router that hands out
     *  itself as the resolver has to answer. */
    NET_PROBE_METHOD_DNS,
    /** ARP request for the gateway. A gateway that answers no ARP cannot
     *  carry IP traffic at all, so this is the most universally-answered
     *  probe available — at the cost of being unusually chatty for ARP. */
    NET_PROBE_METHOD_ARP,
} net_probe_method_t;

const char *net_probe_method_name(net_probe_method_t m);

/** Parse "dns" / "arp" / "none"; returns false on anything else. */
bool net_probe_method_from_string(const char *s, net_probe_method_t *out);

/**
 * Start probing. `netif` and `gateway` come from wifi_mgr; interval_ms is the
 * send period — to lift S1 above the 20 pkt/s averaging threshold this needs
 * to be well under 50 ms, hence the aggressive default below.
 *
 * Safe to call repeatedly: restarts with the new settings.
 */
esp_err_t net_probe_start(net_probe_method_t method, esp_netif_t *netif,
                          const esp_ip4_addr_t *gateway, uint32_t interval_ms);
void net_probe_stop(void);

bool net_probe_is_running(void);
net_probe_method_t net_probe_get_method(void);

/** Replies seen since start, and a smoothed replies-per-second estimate.
 *  reply_rate is the number to watch: it is what S1's pkt/s is bounded by. */
uint32_t net_probe_reply_count(void);
float    net_probe_reply_rate(void);
uint32_t net_probe_send_count(void);

/** Liveness, same contract as ping_gw's: only meaningful once at least one
 *  reply has arrived, so a network that answers nothing is never mistaken for
 *  a stalled link (the bug that made the S1 watchdog reconnect forever —
 *  docs/boards.md's "Correction: the ~8s reconnect cycle was our own
 *  watchdog"). */
bool     net_probe_has_succeeded(void);
uint32_t net_probe_ms_since_last_success(void);

/** Default send period. 20 ms = 50/s, chosen to clear the 20 pkt/s grid
 *  threshold with margin even if a fraction of probes go unanswered. */
#define NET_PROBE_DEFAULT_INTERVAL_MS 20
