#include "ble_scan.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "devices.h"

static const char *TAG = "ble_scan";

/* Fixed 30ms listen window; duty cycle is set by stretching the interval
 * around it. NimBLE units are 0.625ms. */
#define SCAN_WINDOW_UNITS 48
#define SCAN_ITVL_MAX_UNITS 0x4000

#define COMPANY_APPLE     0x004C
#define COMPANY_SAMSUNG   0x0075
#define COMPANY_GOOGLE    0x00E0
#define COMPANY_MICROSOFT 0x0006
/* Not a phone vendor — Broadcom/Cypress wireless chips are standard on
 * Raspberry Pi boards. Confirmed against a real Pi's advert on 2026-09-15
 * (see docs/boards.md); kept as WIFEEL_VENDOR_OTHER (matches the default
 * case) but named explicitly so it isn't confused for an unhandled id. */
#define COMPANY_BROADCOM 0x005D

/* 16-bit "member" service UUID registered to Google LLC (Bluetooth SIG
 * assigned numbers, uuids/member_uuids.yaml) — seen in AD type 0x16
 * (Service Data), not manufacturer data. This is Android/Google Play
 * Services' own cross-device beacon, confirmed live alongside a Samsung
 * phone's manufacturer-data advert on 2026-09-15. Not scored as
 * "phone-like" on its own: the same UUID could just as well come from a
 * Chromecast, Android TV, or Google smart speaker. */
#define GOOGLE_SERVICE_UUID 0xFCF1

/* Apple Continuity message types (first byte of each TLV after the company
 * ID). Sent by iPhones: Nearby Info, Handoff, Nearby Action. Not phones:
 * AirPods (Proximity Pairing), AirTags/offline devices (Find My), iBeacons. */
#define APPLE_IBEACON            0x02
#define APPLE_PROXIMITY_PAIRING  0x07
#define APPLE_HANDOFF            0x0C
#define APPLE_NEARBY_ACTION      0x0F
#define APPLE_NEARBY_INFO        0x10
#define APPLE_FIND_MY            0x12

#define RAW_QUEUE_LEN 32

static volatile uint8_t s_duty = BLE_SCAN_DEFAULT_DUTY_PCT;
static volatile bool s_synced;
static volatile uint32_t s_adv_count;

static QueueHandle_t s_raw_queue;
static volatile bool s_raw_active;

static bool apple_is_phone_like(const uint8_t *p, size_t len, uint8_t *first_type)
{
    bool phone = false;
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t type = p[i];
        uint8_t tlv_len = p[i + 1];
        if (first_type && *first_type == 0) {
            *first_type = type;
        }
        switch (type) {
            case APPLE_NEARBY_INFO:
            case APPLE_HANDOFF:
            case APPLE_NEARBY_ACTION:
                phone = true;
                break;
            case APPLE_PROXIMITY_PAIRING:
            case APPLE_FIND_MY:
            case APPLE_IBEACON:
                return false; /* an accessory or beacon, even if other TLVs follow */
            default:
                break;
        }
        i += 2u + tlv_len;
    }
    return phone;
}

/* Scans every AD structure rather than stopping at the first one: a single
 * advert can carry both a manufacturer-data field and a service-data field
 * (seen live — Android phones send their OEM's manufacturer-data alongside
 * Google Play Services' own service-data beacon), and an unrecognized
 * manufacturer id shouldn't shadow a recognized service-data match found
 * later in the same packet. Priority: a recognized manufacturer id (has a
 * real phone/not-phone answer) beats the Google service-data UUID (vendor
 * only, no phone answer) beats an unrecognized manufacturer id (-> Other)
 * beats nothing found (-> Unknown). */
bool ble_scan_classify(const uint8_t *data, size_t len, wifeel_vendor_t *vendor, uint8_t *apple_type)
{
    *vendor = WIFEEL_VENDOR_UNKNOWN;
    if (apple_type) {
        *apple_type = 0;
    }

    bool have_result = false;
    wifeel_vendor_t best_vendor = WIFEEL_VENDOR_UNKNOWN;
    bool best_phone = false;
    uint8_t best_apple_type = 0;
    bool saw_unrecognized_mfg = false;

    size_t i = 0;
    while (i < len) {
        uint8_t field_len = data[i];
        if (field_len == 0 || i + 1u + field_len > len) {
            break; /* padding or a truncated/malformed field */
        }
        uint8_t ad_type = data[i + 1];
        const uint8_t *val = &data[i + 2];
        size_t val_len = field_len - 1u;

        if (ad_type == 0xFF && val_len >= 2) { /* manufacturer specific data */
            uint16_t company = (uint16_t)(val[0] | (val[1] << 8));
            switch (company) {
                case COMPANY_APPLE:
                    *vendor = WIFEEL_VENDOR_APPLE;
                    return apple_is_phone_like(val + 2, val_len - 2, apple_type);
                case COMPANY_SAMSUNG:
                    *vendor = WIFEEL_VENDOR_SAMSUNG;
                    return true; /* provisional — confirm against real captures */
                case COMPANY_GOOGLE:
                    *vendor = WIFEEL_VENDOR_GOOGLE;
                    return true; /* provisional — confirm against real captures */
                case COMPANY_MICROSOFT:
                    *vendor = WIFEEL_VENDOR_MICROSOFT; /* Windows PCs */
                    return false;
                case COMPANY_BROADCOM:
                    saw_unrecognized_mfg = true; /* e.g. Raspberry Pi's onboard wireless chip */
                    break;
                default:
                    saw_unrecognized_mfg = true;
                    break;
            }
        } else if (ad_type == 0x16 && val_len >= 2 && !have_result) { /* service data, 16-bit UUID */
            uint16_t uuid = (uint16_t)(val[0] | (val[1] << 8));
            if (uuid == GOOGLE_SERVICE_UUID) {
                have_result = true;
                best_vendor = WIFEEL_VENDOR_GOOGLE;
                best_phone = false; /* see GOOGLE_SERVICE_UUID's comment — not phone-specific */
            }
        }
        i += 1u + field_len;
    }

    if (have_result) {
        *vendor = best_vendor;
        if (apple_type) {
            *apple_type = best_apple_type;
        }
        return best_phone;
    }
    *vendor = saw_unrecognized_mfg ? WIFEEL_VENDOR_OTHER : WIFEEL_VENDOR_UNKNOWN;
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            s_adv_count++;
            wifeel_vendor_t vendor;
            bool phone_like = ble_scan_classify(event->disc.data, event->disc.length_data, &vendor, NULL);
            devices_observe(WIFEEL_DEV_SRC_BLE, event->disc.addr.val, vendor, phone_like,
                             (int8_t)event->disc.rssi, false);
            if (s_raw_active) {
                ble_scan_raw_adv_t adv = {
                    .rssi = event->disc.rssi,
                    .event_type = event->disc.event_type,
                };
                memcpy(adv.addr, event->disc.addr.val, 6);
                adv.len = event->disc.length_data > sizeof(adv.data) ? sizeof(adv.data) : event->disc.length_data;
                memcpy(adv.data, event->disc.data, adv.len);
                xQueueSend(s_raw_queue, &adv, 0); /* drop if the console isn't keeping up */
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGW(TAG, "discovery ended (reason %d)", event->disc_complete.reason);
            return 0;
        default:
            return 0;
    }
}

static int start_scan(uint8_t pct)
{
    if (!s_synced || pct == 0) {
        return 0;
    }
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return rc;
    }

    uint32_t itvl = (uint32_t)SCAN_WINDOW_UNITS * 100u / pct;
    if (itvl < SCAN_WINDOW_UNITS) {
        itvl = SCAN_WINDOW_UNITS;
    }
    if (itvl > SCAN_ITVL_MAX_UNITS) {
        itvl = SCAN_ITVL_MAX_UNITS;
    }
    struct ble_gap_disc_params params = {
        .itvl = (uint16_t)itvl,
        .window = SCAN_WINDOW_UNITS,
        .passive = 1,
        .filter_duplicates = 0, /* every advert is a fresh RSSI sample */
    };
    return ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address (rc=%d)", rc);
        return;
    }
    s_synced = true;
    rc = start_scan(s_duty);
    if (rc != 0) {
        ESP_LOGE(TAG, "starting scan failed (rc=%d)", rc);
        return;
    }
    ESP_LOGI(TAG, "passive scan running at %u%% duty", s_duty);
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns only if nimble_port_stop() is called */
    nimble_port_freertos_deinit();
}

esp_err_t ble_scan_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ble_scan_set_duty(uint8_t pct)
{
    if (pct > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_duty = pct;
    if (!s_synced) {
        return ESP_OK; /* applied once the host syncs */
    }
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    int rc = start_scan(pct);
    if (rc != 0) {
        ESP_LOGE(TAG, "restarting scan failed (rc=%d)", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

uint8_t ble_scan_get_duty(void)
{
    return s_duty;
}

uint32_t ble_scan_adv_count(void)
{
    return s_adv_count;
}

esp_err_t ble_scan_raw_start(void)
{
    if (!s_raw_queue) {
        s_raw_queue = xQueueCreate(RAW_QUEUE_LEN, sizeof(ble_scan_raw_adv_t));
        if (!s_raw_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    xQueueReset(s_raw_queue);
    s_raw_active = true;
    return ESP_OK;
}

bool ble_scan_raw_next(ble_scan_raw_adv_t *out, uint32_t timeout_ms)
{
    return s_raw_queue && xQueueReceive(s_raw_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ble_scan_raw_stop(void)
{
    s_raw_active = false;
}
