#include "link.h"

#include <string.h>
#include "esp_now.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifeel_proto.h"
#include "csi_mgr.h"
#include "motion.h"
#include "presence.h"
#include "wifi_mgr.h"
#include "board.h"
#include "devices.h"

static const char *TAG = "link";
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Was 1Hz; raised to lower the hub-computes-it -> display-shows-it delay
 * (see docs/boards.md's delay breakdown). motion_task itself only
 * recomputes every 300ms (MOTION_UPDATE_INTERVAL_MS in motion.c), so
 * broadcasting faster than that would just resend duplicate values. */
#define LINK_RATE_HZ 3

static void link_task(void *arg)
{
    (void)arg;
    uint16_t seq = 0;
    uint32_t iteration = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000 / LINK_RATE_HZ));
        iteration++;

        wifeel_msg_state_t state = {0};

        /* --- Real (P1) --- */
        state.motion_flag = motion_get_flag() ? 1 : 0;
        state.motion_score = motion_get_score();
        state.motion_score_streams[WIFEEL_STREAM_ROUTER_TO_HUB] = motion_get_s1_score();
        state.motion_score_streams[WIFEEL_STREAM_DISPLAY_TO_HUB] = motion_get_s3_score();

        /* --- Real (P2) --- */
        state.presence_state = presence_get_state();

        /* --- Placeholders (documented, not fabricated data): P3-P4 aren't
         * built yet. --- */
        state.people_count = 0;
        state.count_confidence = 0;
        state.breathing_bpm = 0.0f;
        state.breathing_confidence = 0;
        state.breathing_reason = WIFEEL_BREATH_NO_ONE;
        state.zone = WIFEEL_ZONE_UNKNOWN;
        state.zone_confidence = 0;

        wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
        state.pkt_rate[WIFEEL_STREAM_ROUTER_TO_HUB] = wifeel_csi_stream_get_pkt_rate(s1);
        wifeel_msg_features_t feat;
        if (wifeel_csi_stream_get_features(s1, &feat)) {
            state.rssi[WIFEEL_STREAM_ROUTER_TO_HUB] = feat.rssi_mean;
        }

        strlcpy(state.fw_version, WIFEEL_SENSE_FW_VERSION, sizeof(state.fw_version));
        wifi_mgr_get_ap_ssid(state.ssid, sizeof(state.ssid)); /* leaves "" if not connected */

        uint8_t buf[WIFEEL_PROTO_MAX_LEN];
        size_t len = wifeel_proto_pack(buf, WIFEEL_MSG_STATE, seq++, &state, sizeof(state));
        if (len == 0) {
            ESP_LOGW(TAG, "wifeel_proto_pack (state) failed");
        } else {
            esp_err_t err = esp_now_send(s_broadcast_mac, buf, len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send (state) failed: %s", esp_err_to_name(err));
            }
        }

        /* Nearby-phone summary at ~1Hz (every LINK_RATE_HZ-th pass) — it
         * doesn't need STATE's update rate; devices.c's own expiry is
         * already on a multi-second timescale. */
        if (iteration % LINK_RATE_HZ == 0) {
            wifeel_msg_devices_t devices;
            devices_get_summary(&devices);
            len = wifeel_proto_pack(buf, WIFEEL_MSG_DEVICES, seq++, &devices, sizeof(devices));
            if (len == 0) {
                ESP_LOGW(TAG, "wifeel_proto_pack (devices) failed");
            } else {
                esp_err_t err = esp_now_send(s_broadcast_mac, buf, len);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "esp_now_send (devices) failed: %s", esp_err_to_name(err));
                }
            }
        }
    }
}

esp_err_t link_init(void)
{
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = 0; /* use whatever channel the STA is currently on */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(&link_task, "link", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "broadcasting STATE at %d Hz", LINK_RATE_HZ);
    return ESP_OK;
}
