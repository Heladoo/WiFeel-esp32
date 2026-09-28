/*
 * PROBE (not WiFeel firmware): Zigbee End Device that presents itself as an
 * On/Off light on endpoint 1, to find out what a Tuya Zigbee hub will accept
 * and surface in its app. Derived from Espressif's stock HA_temperature_sensor
 * and HA_on_off_light examples (SPDX: LicenseRef-Included, Public Domain/CC0).
 *
 * Changes from the stock temperature_sensor example, each evidence-driven:
 *  - endpoint 1 (Tuya devices expose their function on endpoint 1)
 *  - On/Off light endpoint instead of temperature sensor (an ESP32-C6 with
 *    custom manufacturer/model strings was adopted by a Tuya gateway as an
 *    on/off device — espressif/esp-zigbee-sdk issue #752)
 *  - TC link-key exchange NOT required: the previous run failed with
 *    EZB_BDB_STATUS_TCLK_EX_FAILURE (0x0a) against the hub, then left the network
 *  - flips the On/Off state every 10 s and reports it, so we can see whether the
 *    app displays state that originates from the ESP32
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alarm_timer.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/secur.h"

#include "temperature_sensor.h"

static const char *TAG = "ZB_PROBE";

#define PROBE_EP_ID ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID

static volatile uint32_t s_flips_done;

/* Independent of the Zigbee lock on purpose: if the flip task stops logging
 * but this keeps going, the stack is holding the lock (blocked/hung); if this
 * stops too, the whole system is stalled. Diagnoses the "silent after ~95 s"
 * behaviour seen in the previous run. */
static void heartbeat_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "heartbeat: uptime=%llus heap=%u min_heap=%u flips=%u", (unsigned long long)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)s_flips_done);
    }
}

static void flip_task(void *arg)
{
    uint8_t on = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        on = !on;

        if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(3000))) {
            ESP_LOGE(TAG, "flip: Zigbee lock NOT acquired within 3s — stack is holding it (blocked/hung)");
            continue;
        }
        ezb_zcl_set_attr_value(PROBE_EP_ID, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                               EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &on, false);

        /* Report to the coordinator (short address 0x0000), endpoint 1 — the
         * gateway. Explicit destination rather than relying on a binding the
         * hub may never have created for an unrecognised device. */
        ezb_zcl_report_attr_cmd_t report = {
            .cmd_ctrl =
                {
                    .fc.direction       = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                    .dst_addr           = EZB_ADDRESS_SHORT(0x0000),
                    .dst_ep             = 1,
                    .src_ep             = PROBE_EP_ID,
                    .cluster_id         = EZB_ZCL_CLUSTER_ID_ON_OFF,
                },
            .payload =
                {
                    .attr_id = EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                },
        };
        ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&report);
        esp_zigbee_lock_release();

        s_flips_done++;
        ESP_LOGI(TAG, "flipped On/Off -> %u, report %s (0x%04x)", on, ret == EZB_ERR_NONE ? "sent" : "FAILED", ret);
    }
}

static void start_flip_task_once(void)
{
    static bool started = false;
    if (!started) {
        started = true;
        xTaskCreate(flip_task, "probe_flip", 3072, NULL, 4, NULL);
    }
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device reboot (already on a network)");
                start_flip_task_once();
            }
        } else {
            /* Rebooted holding a stored network the hub no longer answers for
             * (seen live: NO_NETWORK forever on DEVICE_REBOOT). Instead of
             * needing an external flash erase each probe, give up after a few
             * tries and go factory-new so steering can start over. */
            static int s_reboot_failures;
            if (signal_type == EZB_BDB_SIGNAL_DEVICE_REBOOT && ++s_reboot_failures >= 6) {
                ESP_LOGW(TAG, "stored network unreachable after %d tries — local factory reset, re-steering", s_reboot_failures);
                s_reboot_failures = 0;
                ezb_bdb_reset_via_local_action();
                /* Observed live: with no live network to leave, the reset emits
                 * no LEAVE signal, so nothing else would restart the search.
                 * Kick steering ourselves shortly after the reset settles. */
                alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1500);
                break;
            }
            ESP_LOGW(TAG, "%s failed with status(0x%02x), please retry", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "JOINED network: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            start_flip_task_once();
        } else {
            ESP_LOGW(TAG, "Failed to join network with status(0x%02x)", status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *dev_annce_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device announce (short: 0x%04hx)", dev_annce_params->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network with type(0x%02x) — will re-search in 1s", leave_params->leave_type);
        /* Stock example just logs here and idles forever afterwards (that is
         * what the probe did after a hub-initiated leave / local reset). */
        alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Permit-join on our network: %d s", duration);
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        /* The hub/app writing our On/Off — proves the control path works. */
        ezb_zcl_set_attr_value_message_t *msg = (ezb_zcl_set_attr_value_message_t *)message;
        if (msg && msg->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
            ESP_LOGI(TAG, "HUB WROTE On/Off = %d (ep %d)", *(uint8_t *)msg->in.attribute.data.value, msg->info.dst_ep);
        }
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGI(TAG, "Received ZCL Default Response: status(0x%02x)", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGW(TAG, "ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

static esp_err_t esp_zigbee_create_on_off_device(void)
{
    ezb_af_device_desc_t          dev_desc   = ezb_af_create_device_desc();
    ezb_zha_on_off_light_config_t light_cfg  = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t              ep_desc    = ezb_zha_create_on_off_light(PROBE_EP_ID, &light_cfg);
    ezb_zcl_cluster_desc_t        basic_desc = {0};

    basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ESP_MODEL_IDENTIFIER);

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);
    return ESP_OK;
}

static esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    /* Do not demand a Trust Center link-key exchange: the hub failed it last
     * run (status 0x0a), and per secur.h a joiner without this "will not
     * request key from the coordinator". */
    ESP_ERROR_CHECK(ezb_secur_set_tclk_exchange_required(false));
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    return ESP_OK;
}

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());
    ESP_ERROR_CHECK(esp_zigbee_create_on_off_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "Start ESP Zigbee Stack (probe: On/Off light, EP %d, TCLK exchange off)", PROBE_EP_ID);
    xTaskCreate(heartbeat_task, "probe_hb", 3072, NULL, 3, NULL);
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
