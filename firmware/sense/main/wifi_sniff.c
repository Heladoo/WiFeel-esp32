#include "wifi_sniff.h"

#include <stdbool.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "devices.h"

static const char *TAG = "wifi_sniff";

#define MAX_HOME_BSSIDS 4

/* 802.11 frame control field, byte 0 (version/type/subtype) and byte 1
 * (ToDS/FromDS/...) — see IEEE 802.11-2020 9.2.4.1. Two separate uint8_t
 * reads rather than a uint16_t cast: this is raw wire order, not a
 * multi-byte integer, so there's no endianness question to get wrong. */
#define FC_TYPE(fc0)    (((fc0) >> 2) & 0x3)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0xF)
#define FC_TODS(fc1)    ((fc1) & 0x1)
#define FC_FROMDS(fc1)  (((fc1) >> 1) & 0x1)

#define DOT11_TYPE_MGMT 0
#define DOT11_TYPE_DATA 2
#define DOT11_SUBTYPE_BEACON 8

/* Fixed 24-byte header (FC 2 + Duration 2 + Addr1 6 + Addr2 6 + Addr3 6 +
 * SeqCtl 2) — same for every frame shape we handle here. QoS data frames
 * add a 2-byte QoS Control field, but only *after* this, so it doesn't
 * affect Addr1-3 offsets and we never need to read into the body of a
 * data frame anyway. */
#define DOT11_HDR_LEN 24
#define DOT11_ADDR1_OFFSET 4
#define DOT11_ADDR2_OFFSET 10
#define DOT11_ADDR3_OFFSET 16
#define BEACON_FIXED_FIELDS_LEN 12 /* Timestamp 8 + Beacon Interval 2 + Capability Info 2 */

static uint8_t s_home_bssids[MAX_HOME_BSSIDS][6];
static uint8_t s_home_bssid_count;
static uint8_t s_home_ssid[33];
static uint8_t s_home_ssid_len;

static uint8_t s_own_sta_mac[6];
static uint8_t s_own_ap_mac[6];
static uint8_t s_display_mac[6];

static wifi_sniff_stats_t s_stats;

static bool mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};
    return memcmp(mac, zero, 6) == 0;
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool is_home_bssid(const uint8_t mac[6])
{
    for (int i = 0; i < s_home_bssid_count; i++) {
        if (mac_eq(s_home_bssids[i], mac)) {
            return true;
        }
    }
    return false;
}

static void add_home_bssid(const uint8_t mac[6])
{
    if (is_home_bssid(mac) || s_home_bssid_count >= MAX_HOME_BSSIDS) {
        return;
    }
    memcpy(s_home_bssids[s_home_bssid_count++], mac, 6);
    ESP_LOGI(TAG, "home BSSID added: " MACSTR, MAC2STR(mac));
}

static bool is_excluded_client(const uint8_t mac[6])
{
    if (mac[0] & 0x01) { /* group/multicast address */
        return true;
    }
    if (mac_eq(mac, s_own_sta_mac) || mac_eq(mac, s_own_ap_mac)) {
        return true;
    }
    if (!mac_is_zero(s_display_mac) && mac_eq(mac, s_display_mac)) {
        return true;
    }
    return false;
}

bool wifi_sniff_parse_data_frame(const uint8_t *data, uint16_t len,
                                  uint8_t bssid_out[6], uint8_t client_out[6], bool *to_ds_out)
{
    if (len < DOT11_HDR_LEN || FC_TYPE(data[0]) != DOT11_TYPE_DATA) {
        return false;
    }
    bool to_ds = FC_TODS(data[1]);
    bool from_ds = FC_FROMDS(data[1]);
    if (to_ds == from_ds) {
        return false; /* IBSS (0,0) or WDS (1,1) — not the infrastructure client<->AP case we track */
    }
    const uint8_t *addr1 = data + DOT11_ADDR1_OFFSET;
    const uint8_t *addr2 = data + DOT11_ADDR2_OFFSET;
    memcpy(bssid_out, to_ds ? addr1 : addr2, 6);
    memcpy(client_out, to_ds ? addr2 : addr1, 6);
    if (to_ds_out) {
        *to_ds_out = to_ds;
    }
    return true;
}

bool wifi_sniff_parse_beacon(const uint8_t *data, uint16_t len,
                              uint8_t bssid_out[6], uint8_t *ssid_out, uint8_t *ssid_len_out)
{
    if (len < DOT11_HDR_LEN || FC_TYPE(data[0]) != DOT11_TYPE_MGMT || FC_SUBTYPE(data[0]) != DOT11_SUBTYPE_BEACON) {
        return false;
    }
    memcpy(bssid_out, data + DOT11_ADDR3_OFFSET, 6); /* addr2 == addr3 == BSSID for a beacon */

    size_t i = DOT11_HDR_LEN + BEACON_FIXED_FIELDS_LEN;
    while (i + 2 <= len) {
        uint8_t element_id = data[i];
        uint8_t element_len = data[i + 1];
        if (i + 2u + element_len > len) {
            break; /* truncated/malformed IE */
        }
        if (element_id == 0) { /* SSID element */
            memcpy(ssid_out, data + i + 2, element_len);
            *ssid_len_out = element_len;
            return true;
        }
        i += 2u + element_len;
    }
    return false; /* well-formed beacon, but no SSID element found */
}

static void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (type == WIFI_PKT_MGMT) {
        uint8_t bssid[6], ssid[32], ssid_len;
        if (wifi_sniff_parse_beacon(p, len, bssid, ssid, &ssid_len) &&
            ssid_len == s_home_ssid_len && s_home_ssid_len > 0 &&
            memcmp(ssid, s_home_ssid, ssid_len) == 0) {
            add_home_bssid(bssid);
        }
        return;
    }

    /* ESP-IDF already classified this as WIFI_PKT_DATA above, so it's a
     * data frame by definition here — anything wifi_sniff_parse_data_frame()
     * rejects at this point is the IBSS/WDS addressing case (or, rarely, a
     * truncated capture). */
    s_stats.data_frames_seen++;
    uint8_t bssid[6], client[6];
    bool to_ds;
    if (!wifi_sniff_parse_data_frame(p, len, bssid, client, &to_ds)) {
        s_stats.ibss_or_wds_skipped++;
        return;
    }

    if (!is_home_bssid(bssid)) {
        s_stats.not_home_bssid++;
        return;
    }
    if (is_excluded_client(client)) {
        s_stats.excluded_client++;
        return;
    }
    s_stats.tracked++;

    if (to_ds) {
        /* The client transmitted this frame — its RSSI is a real reading
         * of the client's own signal, usable for distance. Includes null
         * data (subtype 0x4) — the keepalive frames idle phones still
         * send periodically even with no real traffic. */
        devices_observe(WIFEEL_DEV_SRC_WIFI, client, WIFEEL_VENDOR_UNKNOWN, false,
                         (int8_t)pkt->rx_ctrl.rssi, true);
    } else {
        /* The AP transmitted this frame; the RSSI we measured is the AP's,
         * not the client's, so it must not feed the client's distance
         * estimate — only presence/connected state updates. */
        devices_touch(WIFEEL_DEV_SRC_WIFI, client, true);
    }
}

esp_err_t wifi_sniff_init(void)
{
    esp_wifi_get_mac(WIFI_IF_STA, s_own_sta_mac);
    esp_wifi_get_mac(WIFI_IF_AP, s_own_ap_mac);
    return esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_cb);
}

void wifi_sniff_set_home(const uint8_t bssid[6], const char *ssid)
{
    add_home_bssid(bssid);
    if (ssid) {
        size_t n = strnlen(ssid, sizeof(s_home_ssid));
        memcpy(s_home_ssid, ssid, n);
        s_home_ssid_len = (uint8_t)n;
    }
}

void wifi_sniff_set_display_mac(const uint8_t mac[6])
{
    memcpy(s_display_mac, mac, 6);
}

void wifi_sniff_get_stats(wifi_sniff_stats_t *out)
{
    *out = s_stats;
    out->home_bssid_count = s_home_bssid_count;
}
