/**
 * Passive Wi-Fi sniffing: which nearby clients are connected to the home
 * network, for the "connected" flag in nearby-phone detection.
 *
 * Rides on the promiscuous mode csi_mgr.c already enables for CSI capture
 * (esp_wifi_set_promiscuous(true)) — this file only adds its own general
 * RX callback (esp_wifi_set_promiscuous_rx_cb), a separate registration
 * point from CSI's own esp_wifi_set_csi_rx_cb, so both run side by side
 * without disturbing CSI. It never changes the promiscuous filter.
 *
 * Only ever reports the home network's own traffic: a data frame is
 * tracked only when its BSSID is one already known to belong to the home
 * network (seeded from the current association, grown from matching
 * beacons so sibling mesh nodes count too) — traffic on a neighboring
 * network sharing the same channel is ignored, never tracked.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers the promiscuous RX callback. Call after csi_mgr_init() (so
 *  promiscuous mode is already on) and after devices_init(). */
esp_err_t wifi_sniff_init(void);

/** Tells the sniffer which network is "home": call whenever the hub
 *  (re)joins its router (wifi_mgr's connected callback), even on repeat
 *  calls — `bssid` is added to a small set (not replaced), so a mesh
 *  system's other nodes accumulate as the hub roams between them. */
void wifi_sniff_set_home(const uint8_t bssid[6], const char *ssid);

/** Excludes one MAC from tracking — the display's own WiFeel-Link
 *  association, so it's never counted as a "nearby phone". Pass all-zero
 *  to clear. */
void wifi_sniff_set_display_mac(const uint8_t mac[6]);

/**
 * Pure, stateless 802.11 parsers — exposed for `phones selftest` to run
 * against known-good byte samples without needing a real radio. The
 * real-time path (promiscuous_rx_cb in wifi_sniff.c) uses these same
 * functions, so a selftest failure means the live path is also broken.
 */

/** Parses a data frame's client<->AP addressing. Returns false if `data`
 *  isn't a data frame with infrastructure addressing (ToDS xor FromDS) —
 *  IBSS/WDS frames and anything shorter than a full header don't count.
 *  On success, *bssid_out and *client_out are set and *to_ds_out says who
 *  sent it: true means the client's own RSSI is usable for distance
 *  (see devices_observe()); false means the RSSI belongs to the AP, not
 *  the client (see devices_touch()). */
bool wifi_sniff_parse_data_frame(const uint8_t *data, uint16_t len,
                                  uint8_t bssid_out[6], uint8_t client_out[6], bool *to_ds_out);

/** Parses a beacon's BSSID and SSID. Returns false if `data` isn't a
 *  beacon or its SSID element is missing/malformed. `ssid_out` must be at
 *  least 32 bytes; *ssid_len_out is the actual SSID length (0-32). */
bool wifi_sniff_parse_beacon(const uint8_t *data, uint16_t len,
                              uint8_t bssid_out[6], uint8_t *ssid_out, uint8_t *ssid_len_out);

/** Diagnostic counters, all since boot — for `phones` and bring-up
 *  debugging (e.g. "zero Wi-Fi clients": which stage is filtering
 *  everything out?). */
typedef struct {
    uint32_t data_frames_seen;    /* type==DATA, any BSSID */
    uint32_t ibss_or_wds_skipped; /* ToDS == FromDS (not an infra client<->AP frame) */
    uint32_t not_home_bssid;      /* BSSID isn't in the home set yet */
    uint32_t excluded_client;     /* multicast, our own MACs, or the display */
    uint32_t tracked;             /* passed every filter, handed to devices_observe/touch */
    uint8_t  home_bssid_count;
} wifi_sniff_stats_t;
void wifi_sniff_get_stats(wifi_sniff_stats_t *out);

#ifdef __cplusplus
}
#endif
