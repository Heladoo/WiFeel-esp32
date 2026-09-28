#include "zb_tuya.h"

#include <inttypes.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/secur.h"

static const char *TAG = "zb_tuya";

#define EP_MOTION   1
#define EP_PRESENCE 2
#define EP_SCORE    3

/* Same manufacturer/model strings the probe used and the Tuya hub accepted. */
#define ZB_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ZB_MODEL_IDENTIFIER  "\x07""esp32c6"

/* Channel sets exactly as the working probe used them (primary channel 13,
 * secondary = every 2.4 GHz Zigbee channel 11-26; the hub turned out to be
 * on 11, found via the secondary set). */
#define ZB_PRIMARY_CHANNEL_MASK   (1U << 13)
#define ZB_SECONDARY_CHANNEL_MASK 0x07FFF800U

/* Pairing window: how long an unpaired display keeps scanning for an open
 * network. An unbounded scan would keep the radio busy that S3's CSI ping
 * traffic depends on, for a display that may never be paired at all. */
#define PAIRING_WINDOW_US (10LL * 60 * 1000000)

/* Zigbee steering is an active scan of all 16 channels (~4 s per attempt).
 * The first version retried every 1 s — an ~80% scan duty cycle — and the
 * display's Wi-Fi association to the hub never succeeded while it ran
 * ("disconnected from hub, retrying" every ~2.4 s, S3 CSI at 0 pkt/s).
 * So: wait for Wi-Fi to get its first association in, then leave long gaps. */
#define STEERING_FIRST_DELAY_MS 8000
#define STEERING_RETRY_DELAY_MS 15000

/* A stored network the hub isn't answering for (hub off, out of range,
 * rebooting): retry the rejoin, backing off, but NEVER auto-forget the
 * pairing — the probe did, and that would force a re-pair after any
 * hub outage. Forgetting is an explicit long-press of BOOT only. */
#define REJOIN_FAST_TRIES     5
#define REJOIN_FAST_DELAY_MS  1000
#define REJOIN_SLOW_DELAY_MS  30000

#define BOOT_BUTTON_GPIO      GPIO_NUM_9
#define BOOT_LONG_PRESS_MS    6000

#define TICK_MS               250
#define REFRESH_INTERVAL_MS   60000 /* keep the app in sync even when nothing changed */
#define FLAG_MIN_INTERVAL_MS  1000
#define SCORE_MIN_INTERVAL_MS 3000
#define SCORE_MIN_DELTA       5     /* percent */

typedef struct {
    volatile bool     fresh;
    volatile uint8_t  motion_on;
    volatile uint8_t  presence_on;
    volatile uint8_t  score_pct; /* 0-100 */
} desired_t;

static desired_t s_desired;
static volatile bool s_joined;
static volatile bool s_resync_needed; /* the app/hub wrote one of our attributes */

static int64_t s_steering_until_us;
static esp_timer_handle_t s_comm_timer;
static volatile uint8_t s_comm_mode;
static int s_rejoin_failures;

static void comm_timer_cb(void *arg)
{
    (void)arg;
    if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(2000))) {
        (void)ezb_bdb_start_top_level_commissioning(s_comm_mode);
        esp_zigbee_lock_release();
    }
}

/* Single pending commissioning request at a time; a new one replaces it. */
static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    s_comm_mode = mode;
    esp_timer_stop(s_comm_timer); /* ESP_ERR_INVALID_STATE if not running — fine */
    esp_timer_start_once(s_comm_timer, (uint64_t)delay_ms * 1000);
}

static void open_pairing_window(void)
{
    s_steering_until_us = esp_timer_get_time() + PAIRING_WINDOW_US;
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_rejoin_failures = 0;
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "not paired — searching for a Tuya hub in pairing mode (10 min window)");
                open_pairing_window();
                schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, STEERING_FIRST_DELAY_MS);
            } else {
                ESP_LOGI(TAG, "rejoined stored network");
                s_joined = true;
                s_resync_needed = true;
            }
        } else if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
            s_rejoin_failures++;
            uint32_t delay = s_rejoin_failures <= REJOIN_FAST_TRIES ? REJOIN_FAST_DELAY_MS : REJOIN_SLOW_DELAY_MS;
            if (s_rejoin_failures == REJOIN_FAST_TRIES + 1) {
                ESP_LOGW(TAG, "hub not answering the stored network — backing off to every %d s "
                              "(pairing is kept; long-press BOOT to forget it)", REJOIN_SLOW_DELAY_MS / 1000);
            }
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, delay);
        } else {
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with hub: PAN 0x%04hx, channel %d, short addr 0x%04hx", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_joined = true;
            s_resync_needed = true;
        } else if (esp_timer_get_time() < s_steering_until_us) {
            /* status 0x03 = no open network found: hub not in pairing mode yet */
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, STEERING_RETRY_DELAY_MS);
        } else {
            ESP_LOGW(TAG, "pairing window closed without finding a hub in pairing mode — "
                          "long-press BOOT for ~%d s to try again", BOOT_LONG_PRESS_MS / 1000);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *leave = ezb_app_signal_get_params(app_signal);
        ESP_LOGW(TAG, "left the Tuya network (type 0x%02x) — searching again", leave->leave_type);
        s_joined = false;
        open_pairing_window();
        /* Neither the stock example nor a local reset restarts the search on
         * their own — found the hard way with the probe. */
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, STEERING_FIRST_DELAY_MS);
    } break;

    default:
        break;
    }
    return true;
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID && message) {
        /* Someone tapped a tile in the app. The tiles mirror sensed state, not
         * controls, so don't act on it — flag it and let the feeder task push the
         * true value back (can't take the stack lock from inside its callback). */
        s_resync_needed = true;
    }
}

static esp_err_t create_endpoints(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();

    const uint8_t on_off_eps[] = {EP_MOTION, EP_PRESENCE};
    for (size_t i = 0; i < sizeof(on_off_eps); i++) {
        ezb_zha_on_off_light_config_t cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
        ezb_af_ep_desc_t ep = ezb_zha_create_on_off_light(on_off_eps[i], &cfg);
        ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUFACTURER_NAME);
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_IDENTIFIER);
        ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, ep), TAG, "add on/off endpoint %d", on_off_eps[i]);
    }

    ezb_zha_dimmable_light_config_t dcfg = EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
    ezb_af_ep_desc_t score_ep = ezb_zha_create_dimmable_light(EP_SCORE, &dcfg);
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(score_ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_IDENTIFIER);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, score_ep), TAG, "add dimmer endpoint");

    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev), TAG, "register device");
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    return ESP_OK;
}

/* Must hold the Zigbee lock. Reports to the coordinator (0x0000) endpoint 1 —
 * the destination the probe used and the hub ACKed. */
static void report_attr(uint8_t src_ep, uint16_t cluster_id, uint16_t attr_id)
{
    ezb_zcl_report_attr_cmd_t report = {
        .cmd_ctrl =
            {
                .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr     = EZB_ADDRESS_SHORT(0x0000),
                .dst_ep       = 1,
                .src_ep       = src_ep,
                .cluster_id   = cluster_id,
            },
        .payload = {.attr_id = attr_id},
    };
    ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&report);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report ep%u cluster 0x%04x failed: 0x%04x", src_ep, cluster_id, ret);
    }
}

static void push_on_off(uint8_t ep, uint8_t on)
{
    ezb_zcl_set_attr_value(ep, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                           EZB_ZCL_STD_MANUF_CODE, &on, false);
    report_attr(ep, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
}

static void push_level(uint8_t ep, uint8_t pct)
{
    uint8_t level = (uint8_t)(((unsigned)pct * 254u + 50u) / 100u); /* ZCL level 0-254 */
    ezb_zcl_set_attr_value(ep, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_SERVER, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID,
                           EZB_ZCL_STD_MANUF_CODE, &level, false);
    report_attr(ep, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID);
}

typedef struct {
    int      last_value; /* -1 = never pushed */
    int64_t  last_push_us;
} pushed_t;

static bool due(const pushed_t *p, int now_value, int64_t now_us, int min_interval_ms, int min_delta, bool force)
{
    if (force || p->last_value < 0 || (now_us - p->last_push_us) >= (int64_t)REFRESH_INTERVAL_MS * 1000) {
        return true;
    }
    if (abs(now_value - p->last_value) < min_delta) {
        return false;
    }
    return (now_us - p->last_push_us) >= (int64_t)min_interval_ms * 1000;
}

static void feeder_task(void *arg)
{
    (void)arg;
    pushed_t motion = {-1, 0}, presence = {-1, 0}, score = {-1, 0};
    int held_ms = 0;
    bool was_joined = false;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    int64_t last_heap_log_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        /* No PSRAM and ~78 KB free after Wi-Fi+LVGL+Zigbee: keep an eye on it. */
        int64_t t = esp_timer_get_time();
        if (t - last_heap_log_us >= 60LL * 1000000) {
            last_heap_log_us = t;
            ESP_LOGI(TAG, "joined=%d heap=%" PRIu32 " min_heap=%" PRIu32, (int)s_joined, (uint32_t)esp_get_free_heap_size(),
                     (uint32_t)esp_get_minimum_free_heap_size());
        }

        /* Long-press BOOT: forget the pairing and start a fresh pairing window. */
        held_ms = (gpio_get_level(BOOT_BUTTON_GPIO) == 0) ? held_ms + TICK_MS : 0;
        if (held_ms >= BOOT_LONG_PRESS_MS) {
            held_ms = 0;
            ESP_LOGW(TAG, "BOOT long-press: forgetting the Tuya pairing");
            s_joined = false;
            open_pairing_window();
            if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(2000))) {
                ezb_bdb_reset_via_local_action();
                esp_zigbee_lock_release();
            }
            /* A local reset with no live network emits no LEAVE signal, so kick the
             * search ourselves (same trap the probe hit). */
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 1500);
            motion = presence = score = (pushed_t){-1, 0};
            continue;
        }

        if (!s_joined || !s_desired.fresh) {
            was_joined = s_joined;
            continue;
        }

        bool force = s_resync_needed || !was_joined; /* just (re)joined, or the app wrote a tile */
        was_joined = true;
        s_resync_needed = false;

        int64_t now_us = esp_timer_get_time();
        int m = s_desired.motion_on, p = s_desired.presence_on, sc = s_desired.score_pct;

        bool do_m = due(&motion, m, now_us, FLAG_MIN_INTERVAL_MS, 1, force);
        bool do_p = due(&presence, p, now_us, FLAG_MIN_INTERVAL_MS, 1, force);
        bool do_s = due(&score, sc, now_us, SCORE_MIN_INTERVAL_MS, SCORE_MIN_DELTA, force);
        if (!do_m && !do_p && !do_s) {
            continue;
        }

        if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(2000))) {
            ESP_LOGW(TAG, "Zigbee lock busy, skipping this tick");
            continue;
        }
        if (do_m) {
            push_on_off(EP_MOTION, (uint8_t)m);
            motion = (pushed_t){m, now_us};
        }
        if (do_p) {
            push_on_off(EP_PRESENCE, (uint8_t)p);
            presence = (pushed_t){p, now_us};
        }
        if (do_s) {
            push_level(EP_SCORE, (uint8_t)sc);
            score = (pushed_t){sc, now_us};
        }
        esp_zigbee_lock_release();
    }
}

static void stack_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = {
        .device_config =
            {
                .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
                .install_code_policy = false,
                .zed_config = {.ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN, .keep_alive = 4000},
            },
        .platform_config =
            {
                .storage_partition_name = "nvs",
                .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
            },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    /* Without this the join reached the security handshake and failed with
     * EZB_BDB_STATUS_TCLK_EX_FAILURE (0x0a), then the device left the network. */
    ESP_ERROR_CHECK(ezb_secur_set_tclk_exchange_required(false));
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZB_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ZB_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));

    ESP_ERROR_CHECK(create_endpoints());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_LOGI(TAG, "Zigbee stack started; free heap %" PRIu32, (uint32_t)esp_get_free_heap_size());

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void zb_tuya_update(bool fresh, const wifeel_msg_state_t *state)
{
    if (!fresh || !state) {
        s_desired.fresh = false;
        return;
    }
    s_desired.motion_on = state->motion_flag ? 1 : 0;
    s_desired.presence_on = state->presence_state != WIFEEL_PRESENCE_EMPTY ? 1 : 0;
    s_desired.score_pct = state->motion_score > 100 ? 100 : state->motion_score;
    s_desired.fresh = true;
}

esp_err_t zb_tuya_init(void)
{
    const esp_timer_create_args_t timer_args = {.callback = comm_timer_cb, .name = "zb_comm"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_comm_timer), TAG, "esp_timer_create");

    /* Below LVGL's task (5) and the CSI-critical link tasks' Wi-Fi/lwIP
     * dependencies, well above the UI updater (2): a same-priority-as-LVGL
     * Zigbee task is exactly the kind of thing that starved the hub's
     * networking when the HTTP server did it. */
    BaseType_t ok = xTaskCreate(stack_main_task, "zb_stack", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "stack task");
    ok = xTaskCreate(feeder_task, "zb_feed", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "feeder task");
    return ESP_OK;
}
