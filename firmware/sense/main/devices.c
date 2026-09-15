#include "devices.h"

#include <math.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "ble_scan.h" /* ble_scan_get_duty(), for the summary's scan_duty_pct */

static const char *TAG = "devices";

#define MAX_DEVICES 32
#define OBS_QUEUE_LEN 32
#define TASK_PERIOD_MS 500

/* BLE addresses rotate and idle phones advertise rarely — but 30s without
 * any advert at all is a real "gone". Wi-Fi clients only show up when they
 * actually send a frame, which idle phones do far less often than they
 * advertise over BLE, so it gets a much longer leash. */
#define BLE_EXPIRE_MS  30000
#define WIFI_EXPIRE_MS 180000

#define RSSI_HISTORY_LEN 5
#define RSSI_EMA_ALPHA 0.4f

/* Placeholders — reasoned from typical BLE/Wi-Fi TX power, not yet
 * validated. `phones calib ble|wifi <s>` overwrites and persists these. */
#define BLE_REF_1M_DEFAULT  -59.0f
#define WIFI_REF_1M_DEFAULT -40.0f
#define PATH_LOSS_EXPONENT_DEFAULT 2.5f

#define NVS_NAMESPACE "wifeel_dev"
#define NVS_KEY_BLE_REF  "ble_ref1m_x100"
#define NVS_KEY_WIFI_REF "wifi_ref1m_x100"

typedef struct {
    wifeel_dev_source_t source;
    uint8_t mac[6];
    wifeel_vendor_t vendor;
    bool phone_like;
    int8_t rssi;
    bool connected;
    bool touch_only; /* true: only refresh last-seen/connected, skip RSSI (see devices_touch()) */
} observation_t;

typedef struct {
    bool in_use;
    uint8_t mac[6];
    uint16_t id;
    wifeel_dev_source_t source;
    wifeel_vendor_t vendor;
    bool phone_like;
    int8_t rssi_history[RSSI_HISTORY_LEN];
    uint8_t rssi_history_count;
    uint8_t rssi_history_next;
    float rssi_ema;
    bool rssi_ema_valid;
    int64_t last_seen_us;
    bool connected;
} device_slot_t;

static uint32_t s_salt;
static device_slot_t s_devices[MAX_DEVICES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_obs_queue;

static float s_ble_ref_1m = BLE_REF_1M_DEFAULT;
static float s_wifi_ref_1m = WIFI_REF_1M_DEFAULT;
static const float s_path_loss_n = PATH_LOSS_EXPONENT_DEFAULT;

static bool s_calibrating;
static wifeel_dev_source_t s_calib_source;
static int64_t s_calib_deadline_us;

uint16_t devices_id_hash(const uint8_t mac[6])
{
    /* FNV-1a over salt + MAC, folded to 16 bits. Not cryptographic — it only
     * needs to keep raw MACs off the screen and out of logs. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h = (h ^ ((s_salt >> (8 * i)) & 0xFF)) * 16777619u;
    }
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}

static void load_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return; /* nothing saved yet — defaults stand */
    }
    int32_t v;
    if (nvs_get_i32(h, NVS_KEY_BLE_REF, &v) == ESP_OK) {
        s_ble_ref_1m = (float)v / 100.0f;
    }
    if (nvs_get_i32(h, NVS_KEY_WIFI_REF, &v) == ESP_OK) {
        s_wifi_ref_1m = (float)v / 100.0f;
    }
    nvs_close(h);
}

static void save_ref_1m(wifeel_dev_source_t source, float value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, calibration not persisted");
        return;
    }
    const char *key = (source == WIFEEL_DEV_SRC_BLE) ? NVS_KEY_BLE_REF : NVS_KEY_WIFI_REF;
    nvs_set_i32(h, key, (int32_t)lroundf(value * 100.0f));
    nvs_commit(h);
    nvs_close(h);
}

static device_slot_t *find_slot(const uint8_t mac[6])
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].in_use && memcmp(s_devices[i].mac, mac, 6) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static device_slot_t *alloc_slot(void)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            return &s_devices[i];
        }
    }
    device_slot_t *oldest = &s_devices[0];
    for (int i = 1; i < MAX_DEVICES; i++) {
        if (s_devices[i].last_seen_us < oldest->last_seen_us) {
            oldest = &s_devices[i];
        }
    }
    return oldest;
}

static int8_t rssi_median(const int8_t *history, uint8_t count)
{
    int8_t tmp[RSSI_HISTORY_LEN];
    memcpy(tmp, history, count);
    for (uint8_t i = 1; i < count; i++) {
        int8_t key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    return tmp[count / 2];
}

/* Caller must hold s_mux. */
static void apply_observation_locked(const observation_t *obs)
{
    device_slot_t *slot = find_slot(obs->mac);
    if (!slot) {
        slot = alloc_slot();
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        memcpy(slot->mac, obs->mac, 6);
        slot->id = devices_id_hash(obs->mac);
        slot->source = obs->source;
    }
    slot->connected = obs->connected;
    slot->last_seen_us = esp_timer_get_time();

    if (obs->touch_only) {
        return; /* presence/connected refreshed above; RSSI intentionally left alone */
    }

    if (obs->vendor != WIFEEL_VENDOR_UNKNOWN) {
        slot->vendor = obs->vendor; /* keep the most recent non-unknown classification */
        slot->phone_like = obs->phone_like;
    }

    slot->rssi_history[slot->rssi_history_next] = obs->rssi;
    slot->rssi_history_next = (uint8_t)((slot->rssi_history_next + 1) % RSSI_HISTORY_LEN);
    if (slot->rssi_history_count < RSSI_HISTORY_LEN) {
        slot->rssi_history_count++;
    }
    int8_t median = rssi_median(slot->rssi_history, slot->rssi_history_count);
    if (!slot->rssi_ema_valid) {
        slot->rssi_ema = (float)median;
        slot->rssi_ema_valid = true;
    } else {
        slot->rssi_ema = RSSI_EMA_ALPHA * (float)median + (1.0f - RSSI_EMA_ALPHA) * slot->rssi_ema;
    }
}

static void expire_stale(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            continue;
        }
        uint32_t timeout_ms = (s_devices[i].source == WIFEEL_DEV_SRC_BLE) ? BLE_EXPIRE_MS : WIFI_EXPIRE_MS;
        if ((uint32_t)((now - s_devices[i].last_seen_us) / 1000) > timeout_ms) {
            s_devices[i].in_use = false;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static void check_calibration(void)
{
    if (!s_calibrating || esp_timer_get_time() < s_calib_deadline_us) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    device_slot_t *best = NULL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_slot_t *s = &s_devices[i];
        if (!s->in_use || s->source != s_calib_source || !s->rssi_ema_valid) {
            continue;
        }
        if (!best || s->rssi_ema > best->rssi_ema) {
            best = s;
        }
    }
    float new_ref = best ? best->rssi_ema : 0.0f;
    portEXIT_CRITICAL(&s_mux);

    s_calibrating = false;
    const char *name = (s_calib_source == WIFEEL_DEV_SRC_BLE) ? "BLE" : "Wi-Fi";
    if (!best) {
        ESP_LOGW(TAG, "%s calibration finished but no matching device was seen — not saved", name);
        return;
    }
    if (s_calib_source == WIFEEL_DEV_SRC_BLE) {
        s_ble_ref_1m = new_ref;
    } else {
        s_wifi_ref_1m = new_ref;
    }
    save_ref_1m(s_calib_source, new_ref);
    ESP_LOGI(TAG, "%s calibration finished: 1m reference = %.1f dBm", name, (double)new_ref);
}

static void devices_task(void *arg)
{
    (void)arg;
    for (;;) {
        observation_t obs;
        if (xQueueReceive(s_obs_queue, &obs, pdMS_TO_TICKS(TASK_PERIOD_MS)) == pdTRUE) {
            portENTER_CRITICAL(&s_mux);
            apply_observation_locked(&obs);
            portEXIT_CRITICAL(&s_mux);
        }
        expire_stale();
        check_calibration();
    }
}

esp_err_t devices_init(void)
{
    s_salt = esp_random();
    load_calibration();

    s_obs_queue = xQueueCreate(OBS_QUEUE_LEN, sizeof(observation_t));
    if (!s_obs_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(&devices_task, "devices", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void devices_observe(wifeel_dev_source_t source, const uint8_t mac[6],
                      wifeel_vendor_t vendor, bool phone_like, int8_t rssi, bool connected)
{
    if (!s_obs_queue) {
        return;
    }
    observation_t obs = {
        .source = source,
        .vendor = vendor,
        .phone_like = phone_like,
        .rssi = rssi,
        .connected = connected,
    };
    memcpy(obs.mac, mac, 6);
    xQueueSend(s_obs_queue, &obs, 0); /* never block a radio callback; drop if full */
}

void devices_touch(wifeel_dev_source_t source, const uint8_t mac[6], bool connected)
{
    if (!s_obs_queue) {
        return;
    }
    observation_t obs = {
        .source = source,
        .connected = connected,
        .touch_only = true,
    };
    memcpy(obs.mac, mac, 6);
    xQueueSend(s_obs_queue, &obs, 0);
}

static float distance_meters(wifeel_dev_source_t source, float rssi_dbm)
{
    float ref = (source == WIFEEL_DEV_SRC_BLE) ? s_ble_ref_1m : s_wifi_ref_1m;
    return powf(10.0f, (ref - rssi_dbm) / (10.0f * s_path_loss_n));
}

static uint8_t distance_to_dm(float meters)
{
    if (isnan(meters) || meters < 0.0f || meters > 25.4f) {
        return 255;
    }
    return (uint8_t)lroundf(meters * 10.0f);
}

typedef struct {
    device_slot_t *slot;
    float dist_m; /* cached so the sort below doesn't recompute powf() per comparison */
} ranked_t;

void devices_get_summary(wifeel_msg_devices_t *out)
{
    memset(out, 0, sizeof(*out));
    out->scan_duty_pct = ble_scan_get_duty();

    ranked_t ranked[MAX_DEVICES];
    int n = 0;

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_slot_t *s = &s_devices[i];
        if (!s->in_use || !s->rssi_ema_valid) {
            continue;
        }
        ranked[n].slot = s;
        ranked[n].dist_m = distance_meters(s->source, s->rssi_ema);
        n++;
        if (s->source == WIFEEL_DEV_SRC_BLE && s->phone_like) {
            out->ble_phone_count++;
        } else if (s->source == WIFEEL_DEV_SRC_WIFI) {
            out->wifi_client_count++;
        }
        if (s->vendor < WIFEEL_VENDOR_COUNT) {
            out->vendor_counts[s->vendor]++;
        }
    }

    /* Partial selection sort by nearest (smallest) estimated distance — n is
     * at most MAX_DEVICES (32), so an O(n * WIFEEL_DEVICES_MAX_ENTRIES) pass
     * here is cheap and avoids pulling in a full sort for 8 items. Ranking
     * by distance rather than raw RSSI matters because BLE and Wi-Fi use
     * different 1m references (ref_1m) — a strong Wi-Fi RSSI and a strong
     * BLE RSSI aren't the same physical distance, so comparing raw RSSI
     * across sources produced a "closest first" order that wasn't actually
     * closest-first whenever both sources were present. */
    int top = (n < WIFEEL_DEVICES_MAX_ENTRIES) ? n : WIFEEL_DEVICES_MAX_ENTRIES;
    for (int k = 0; k < top; k++) {
        int best_idx = k;
        for (int j = k + 1; j < n; j++) {
            if (ranked[j].dist_m < ranked[best_idx].dist_m) {
                best_idx = j;
            }
        }
        ranked_t tmp = ranked[k];
        ranked[k] = ranked[best_idx];
        ranked[best_idx] = tmp;
    }

    int64_t now = esp_timer_get_time();
    for (int k = 0; k < top; k++) {
        device_slot_t *s = ranked[k].slot;
        wifeel_msg_device_entry_t *e = &out->entries[k];
        e->id = s->id;
        e->source = (uint8_t)s->source;
        e->vendor = (uint8_t)s->vendor;
        e->rssi = (int8_t)lroundf(s->rssi_ema);
        e->distance_dm = distance_to_dm(ranked[k].dist_m);
        e->connected = s->connected ? 1 : 0;
        e->phone_like = (s->source == WIFEEL_DEV_SRC_BLE && s->phone_like) ? 1 : 0;
        int64_t age_s = (now - s->last_seen_us) / 1000000;
        e->age_s = (age_s > 255) ? 255 : (uint8_t)age_s;
    }
    out->n_entries = (uint8_t)top;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t devices_calibrate_start(wifeel_dev_source_t source, uint32_t duration_ms)
{
    s_calib_source = source;
    s_calib_deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    s_calibrating = true;
    return ESP_OK;
}

bool devices_calibrate_is_active(void)
{
    return s_calibrating;
}

uint32_t devices_calibrate_remaining_ms(void)
{
    if (!s_calibrating) {
        return 0;
    }
    int64_t remain_us = s_calib_deadline_us - esp_timer_get_time();
    if (remain_us < 0) {
        remain_us = 0;
    }
    return (uint32_t)(remain_us / 1000);
}

float devices_get_ref_1m(wifeel_dev_source_t source)
{
    return (source == WIFEEL_DEV_SRC_BLE) ? s_ble_ref_1m : s_wifi_ref_1m;
}

float devices_get_path_loss_exponent(void)
{
    return s_path_loss_n;
}
