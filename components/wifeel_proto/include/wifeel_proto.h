/**
 * WiFeel shared ESP-NOW wire protocol.
 *
 * Used by both firmware/sense (hub) and firmware/display so the two
 * projects can never drift out of sync on message layout. Raw CSI data
 * NEVER goes over this link — only extracted features and fused results.
 *
 * Every message starts with wifeel_msg_header_t, followed by a
 * type-specific payload, followed by a trailing uint16_t CRC-16/CCITT-FALSE
 * computed over everything before it (header + payload). Use
 * wifeel_proto_pack() / wifeel_proto_unpack() rather than hand-rolling this.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFEEL_PROTO_MAGIC   0x4C454657u /* 'W' 'F' 'E' 'L' little-endian */
#define WIFEEL_PROTO_VERSION 1

#define WIFEEL_SSID_MAX_LEN     32   /* + NUL */
#define WIFEEL_PASSWORD_MAX_LEN 63   /* + NUL, matches WPA2 max */
#define WIFEEL_FW_VERSION_LEN   16   /* + NUL */

/** Number of CSI streams the system fuses over: S1 router->hub,
 *  S2 router->display, S3 display->hub (sounding), S4 hub->display (sounding). */
#define WIFEEL_NUM_STREAMS 4

/**
 * Fixed local Wi-Fi credentials for the hub<->display link: the hub runs
 * its own SoftAP under this SSID/password and the display joins it as a
 * plain station (see CLAUDE.md / docs/boards.md for why — this gives a
 * second, independent CSI-sensing vantage point, S3, using the same
 * proven data-frame CSI path JOIN mode already uses, unlike raw ESP-NOW
 * broadcasts which don't produce CSI at all).
 *
 * This is NOT the user's home Wi-Fi network — it's a private link between
 * WiFeel's own two boards, so hardcoding it here (one source of truth for
 * both firmwares, instead of two hand-typed copies that could drift out
 * of sync) is fine. Never reuse these for anything user-facing.
 */
#define WIFEEL_LINK_AP_SSID     "WiFeel-Link"
#define WIFEEL_LINK_AP_PASSWORD "wifeel-sense-link"

typedef enum {
    WIFEEL_STREAM_ROUTER_TO_HUB     = 0, /* S1 */
    WIFEEL_STREAM_ROUTER_TO_DISPLAY = 1, /* S2 */
    WIFEEL_STREAM_DISPLAY_TO_HUB    = 2, /* S3 */
    WIFEEL_STREAM_HUB_TO_DISPLAY    = 3, /* S4 */
} wifeel_stream_id_t;

typedef enum {
    WIFEEL_MSG_HELLO       = 1,  /* broadcast, discovery */
    WIFEEL_MSG_HELLO_ACK   = 2,
    WIFEEL_MSG_SET_SOURCE  = 3,  /* display -> hub: pick network / mode */
    WIFEEL_MSG_SOUND       = 4,  /* either direction: tiny CSI sounding frame */
    WIFEEL_MSG_FEATURES    = 5,  /* display -> hub: S2/S4 extracted features */
    WIFEEL_MSG_STATE       = 6,  /* hub -> display: fused result, 1 Hz */
    WIFEEL_MSG_WAVE        = 7,  /* hub -> display: breathing waveform sample, 5 Hz */
    WIFEEL_MSG_CALIB       = 8,  /* display -> hub: start/cancel empty-room calibration */
    WIFEEL_MSG_TRAIN       = 9,  /* display -> hub: start/stop a labelled recording */
    WIFEEL_MSG_PING        = 10,
    WIFEEL_MSG_PONG        = 11,
    WIFEEL_MSG_DEVICES     = 12, /* hub -> display: nearby-phone summary, ~1 Hz */
} wifeel_msg_type_t;

typedef enum {
    WIFEEL_SOURCE_JOIN    = 0, /* password given: STA-join + active ping */
    WIFEEL_SOURCE_PASSIVE = 1, /* no password: promiscuous listen only */
    WIFEEL_SOURCE_DIRECT  = 2, /* "Direct link (no router)": S3/S4 only */
} wifeel_source_mode_t;

typedef enum {
    WIFEEL_PRESENCE_EMPTY         = 0,
    WIFEEL_PRESENCE_PRESENT_STILL = 1,
    WIFEEL_PRESENCE_MOTION        = 2,
} wifeel_presence_state_t;

/** Why breathing_bpm in wifeel_msg_state_t should not be trusted right now. */
typedef enum {
    WIFEEL_BREATH_OK           = 0, /* bpm is a real, current reading */
    WIFEEL_BREATH_WARMING_UP   = 1, /* < 20s of stillness so far */
    WIFEEL_BREATH_MOVING       = 2,
    WIFEEL_BREATH_NO_ONE       = 3,
    WIFEEL_BREATH_MULTIPLE     = 4, /* people_count != 1 */
} wifeel_breath_reason_t;

typedef enum {
    WIFEEL_ZONE_UNKNOWN      = 0,
    WIFEEL_ZONE_NEAR_HUB     = 1,
    WIFEEL_ZONE_NEAR_DISPLAY = 2,
    WIFEEL_ZONE_NEAR_ROUTER  = 3,
    WIFEEL_ZONE_MIDDLE       = 4,
} wifeel_zone_t;

typedef enum {
    WIFEEL_TRAIN_LABEL_COUNT = 0,
    WIFEEL_TRAIN_LABEL_ZONE  = 1,
} wifeel_train_label_type_t;

typedef enum {
    WIFEEL_TRAIN_START = 0,
    WIFEEL_TRAIN_STOP  = 1,
} wifeel_train_action_t;

typedef enum {
    WIFEEL_CALIB_START  = 0,
    WIFEEL_CALIB_CANCEL = 1,
} wifeel_calib_action_t;

/** Vendor of a nearby device, from BLE advert manufacturer data. Vendor
 *  level only — model can't be told apart reliably. Also indexes
 *  wifeel_msg_devices_t.vendor_counts[]. */
typedef enum {
    WIFEEL_VENDOR_UNKNOWN   = 0, /* no manufacturer data (e.g. Wi-Fi-only sightings) */
    WIFEEL_VENDOR_APPLE     = 1,
    WIFEEL_VENDOR_SAMSUNG   = 2,
    WIFEEL_VENDOR_GOOGLE    = 3,
    WIFEEL_VENDOR_MICROSOFT = 4,
    WIFEEL_VENDOR_OTHER     = 5,
    WIFEEL_VENDOR_COUNT     = 6,
} wifeel_vendor_t;

typedef enum {
    WIFEEL_DEV_SRC_BLE  = 0,
    WIFEEL_DEV_SRC_WIFI = 1,
} wifeel_dev_source_t;

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;   /* WIFEEL_PROTO_MAGIC */
    uint8_t  version; /* WIFEEL_PROTO_VERSION */
    uint8_t  type;    /* wifeel_msg_type_t */
    uint16_t seq;
} wifeel_msg_header_t;

/** WIFEEL_MSG_HELLO / WIFEEL_MSG_HELLO_ACK payload: empty. Role is implied
 *  by which board sends it (hub vs display); the sender's MAC comes from
 *  the ESP-NOW receive callback, not this payload. */

typedef struct {
    uint8_t  mode;              /* wifeel_source_mode_t */
    char     ssid[WIFEEL_SSID_MAX_LEN + 1];
    char     password[WIFEEL_PASSWORD_MAX_LEN + 1]; /* empty for PASSIVE/DIRECT */
    uint8_t  bssid[6];
    uint8_t  channel;
} wifeel_msg_set_source_t;

typedef struct {
    uint32_t counter;
    uint32_t t_ms;
} wifeel_msg_sound_t;

/** Per-window features for one CSI stream, computed on a 20 Hz grid over a
 *  2s window / 1s hop. Sent by the display for its two streams (S2, S4);
 *  the hub computes S1/S3 locally and doesn't need to send them to itself. */
typedef struct {
    uint8_t  stream_id;              /* wifeel_stream_id_t */
    float    mean_amplitude;
    float    wander;                 /* deviation from calibrated baseline */
    float    jitter_energy;          /* short-term amplitude-difference energy */
    float    subcarrier_variance[8]; /* variance spread across 8 subcarrier groups */
    float    breathing_band_power;   /* 0.1-0.6 Hz band power */
    float    rssi_mean;
    float    rssi_std;
    float    pkt_rate;
} wifeel_msg_features_t;

typedef struct {
    uint8_t  motion_flag;      /* bool */
    uint8_t  motion_score;     /* 0-100 */
    uint8_t  presence_state;   /* wifeel_presence_state_t */
    uint8_t  people_count;     /* 0-3, 3 means "3+" */
    uint8_t  count_confidence; /* 0-100 */
    float    breathing_bpm;    /* valid only when breathing_reason == WIFEEL_BREATH_OK */
    uint8_t  breathing_confidence; /* 0-100 */
    uint8_t  breathing_reason; /* wifeel_breath_reason_t */
    uint8_t  zone;             /* wifeel_zone_t */
    uint8_t  zone_confidence;  /* 0-100 */
    float    pkt_rate[WIFEEL_NUM_STREAMS];
    float    rssi[WIFEEL_NUM_STREAMS];
    /* Per-stream motion score (0-100) BEFORE fusion — motion_score above
     * is the fused (max) value; this lets the display chart each stream's
     * contribution separately. 0 on an index with no data (S2/S4 aren't
     * implemented yet). */
    uint8_t  motion_score_streams[WIFEEL_NUM_STREAMS];
    char     fw_version[WIFEEL_FW_VERSION_LEN + 1];
    char     ssid[WIFEEL_SSID_MAX_LEN + 1]; /* the network the hub is sensing from; empty if not connected */
} wifeel_msg_state_t;

typedef struct {
    float    sample;  /* one decimated breathing-signal sample */
    uint32_t t_ms;
} wifeel_msg_wave_t;

typedef struct {
    uint8_t  action;     /* wifeel_calib_action_t */
    uint16_t duration_s;
} wifeel_msg_calib_t;

typedef struct {
    uint8_t  label_type;  /* wifeel_train_label_type_t */
    uint8_t  label_value; /* people_count (0-3) or wifeel_zone_t */
    uint8_t  action;      /* wifeel_train_action_t */
} wifeel_msg_train_t;

/** One nearby device, sorted by distance on the sender side. `id` is a
 *  per-boot hash (devices_id_hash()) — never a real MAC address; see
 *  devices.h's privacy boundary comment. */
typedef struct {
    uint16_t id;
    uint8_t  source;       /* wifeel_dev_source_t */
    uint8_t  vendor;       /* wifeel_vendor_t */
    int8_t   rssi;
    uint8_t  distance_dm;  /* decimeters; 255 = unknown/out of range */
    uint8_t  connected;    /* bool: on the home Wi-Fi network (WIFEEL_DEV_SRC_WIFI only) */
    uint8_t  age_s;        /* seconds since last seen, capped at 255 */
    uint8_t  phone_like;   /* bool: BLE classifier's phone/not-phone verdict (ble_scan.c).
                             * Always 0 for WIFEEL_DEV_SRC_WIFI — there's no Wi-Fi vendor/type
                             * classifier yet (see wifi_sniff.c), so a Wi-Fi entry can't claim
                             * to be a phone vs a PC vs anything else. */
} wifeel_msg_device_entry_t;

#define WIFEEL_DEVICES_MAX_ENTRIES 8

typedef struct {
    uint8_t  ble_phone_count;
    uint8_t  wifi_client_count;
    uint8_t  vendor_counts[WIFEEL_VENDOR_COUNT];
    uint8_t  scan_duty_pct;
    uint8_t  n_entries;    /* how many of entries[] below are valid */
    wifeel_msg_device_entry_t entries[WIFEEL_DEVICES_MAX_ENTRIES];
} wifeel_msg_devices_t;

typedef struct {
    uint32_t t_ms;
} wifeel_msg_ping_t;

typedef struct {
    uint32_t t_ms; /* echoed from the ping */
} wifeel_msg_pong_t;

/** Union of every possible payload, sized for the largest member
 *  (wifeel_msg_set_source_t, ~100 bytes) — comfortably under the
 *  ESP-NOW 250-byte payload limit alongside the header and CRC. */
typedef union {
    wifeel_msg_set_source_t set_source;
    wifeel_msg_sound_t      sound;
    wifeel_msg_features_t   features;
    wifeel_msg_state_t      state;
    wifeel_msg_wave_t       wave;
    wifeel_msg_calib_t      calib;
    wifeel_msg_train_t      train;
    wifeel_msg_ping_t       ping;
    wifeel_msg_pong_t       pong;
    wifeel_msg_devices_t    devices;
} wifeel_msg_payload_t;

#pragma pack(pop)

/** Maximum bytes wifeel_proto_pack() can produce (header + largest payload + crc). */
#define WIFEEL_PROTO_MAX_LEN (sizeof(wifeel_msg_header_t) + sizeof(wifeel_msg_payload_t) + sizeof(uint16_t))

/**
 * Serialize a message into `out` (must be at least WIFEEL_PROTO_MAX_LEN
 * bytes). `payload_len` is sizeof(the specific payload struct being sent),
 * e.g. sizeof(wifeel_msg_state_t) — only that many bytes of `payload` are
 * read and written. Fills in magic/version/type/seq and appends the CRC.
 *
 * Returns the total number of bytes written to `out`, or 0 on error
 * (out == NULL, payload_len too large, etc).
 */
size_t wifeel_proto_pack(uint8_t *out, wifeel_msg_type_t type, uint16_t seq,
                          const void *payload, size_t payload_len);

/**
 * Validate and parse a received buffer. On success, returns true and sets
 * *type, *seq, and *payload_out to point at the payload bytes *inside buf*
 * (no copy) with *payload_len_out set to their length. Returns false if the
 * buffer is too short, the magic/version don't match, or the CRC fails —
 * callers must treat any false return as an untrusted/corrupt frame and
 * discard it without acting on the fields.
 *
 * This does NOT validate that *payload_len_out matches the struct size
 * expected for *type (a stale peer on an older protocol version could send
 * a shorter payload). Callers must check *payload_len_out >= sizeof(the
 * expected payload struct) before reading fields from *payload_out.
 */
bool wifeel_proto_unpack(const uint8_t *buf, size_t len,
                          wifeel_msg_type_t *type, uint16_t *seq,
                          const void **payload_out, size_t *payload_len_out);

/** Short display name for a wifeel_vendor_t value ("Apple", ...). */
const char *wifeel_vendor_name(uint8_t vendor);

#ifdef __cplusplus
}
#endif
