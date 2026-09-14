#include "csi_mgr.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "csi_mgr";

typedef struct {
    wifeel_csi_stream_t *stream;
    uint8_t mac[6];
    bool active; /* mac is non-zero and this slot is tracking a real source */
} tracked_stream_t;

/* Only the streams this board can produce CSI for locally. On the hub
 * that's S1 (router->hub); S3 (display->hub sounding) joins in milestone
 * 5. The display's csi_mgr instance will track S2/S4 instead — same code,
 * different ids, per wifeel_csi.h's design. */
static tracked_stream_t s_tracked[WIFEEL_NUM_STREAMS];
static bool s_csi_enabled;

static tracked_stream_t *find_slot(wifeel_stream_id_t id)
{
    if ((size_t)id >= WIFEEL_NUM_STREAMS) {
        return NULL;
    }
    return &s_tracked[id];
}

static void IRAM_ATTR on_csi(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;
    if (!data) {
        return;
    }
    for (size_t i = 0; i < WIFEEL_NUM_STREAMS; i++) {
        tracked_stream_t *t = &s_tracked[i];
        if (t->active && t->stream && memcmp(t->mac, data->mac, 6) == 0) {
            wifeel_csi_stream_feed(t->stream, data);
        }
    }
}

esp_err_t csi_mgr_init(void)
{
    memset(s_tracked, 0, sizeof(s_tracked));

    wifeel_csi_stream_config_t cfg = {
        .id = WIFEEL_STREAM_ROUTER_TO_HUB,
        .has_baseline = false,
    };
    tracked_stream_t *s1 = find_slot(WIFEEL_STREAM_ROUTER_TO_HUB);
    s1->stream = wifeel_csi_stream_init(&cfg);
    if (!s1->stream) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_wifi_set_csi_rx_cb(&on_csi, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi_rx_cb failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Only fields common to both wifi_csi_acquire_config_t layouts (see
     * wifeel_csi.c's comment on esp_wifi_types_native.h) are set here, so
     * this compiles the same regardless of which SOC_WIFI_MAC_VERSION_NUM
     * variant applies to the target. */
    wifi_csi_config_t csi_config = {
        .enable = 1,
        .acquire_csi_legacy = 0,
        .acquire_csi_ht20 = 1,
        .acquire_csi_ht40 = 1,
        .acquire_csi_su = 1,
        .acquire_csi_mu = 1,
        .acquire_csi_dcm = 1,
        .acquire_csi_beamformed = 1,
        .val_scale_cfg = 0, /* automatic scaling */
        .dump_ack_en = 0,
    };
    err = esp_wifi_set_csi_config(&csi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi_config failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t csi_mgr_set_source_mac(wifeel_stream_id_t id, const uint8_t mac[6])
{
    tracked_stream_t *t = find_slot(id);
    if (!t || !t->stream) {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint8_t zero_mac[6] = {0};
    memcpy(t->mac, mac, 6);
    t->active = memcmp(mac, zero_mac, 6) != 0;
    ESP_LOGI(TAG, "stream %d now tracking " MACSTR, (int)id, MAC2STR(t->mac));
    return ESP_OK;
}

esp_err_t csi_mgr_set_enabled(bool enabled)
{
    esp_err_t err = esp_wifi_set_csi(enabled);
    if (err == ESP_OK) {
        s_csi_enabled = enabled;
    }
    return err;
}

wifeel_csi_stream_t *csi_mgr_get_stream(wifeel_stream_id_t id)
{
    tracked_stream_t *t = find_slot(id);
    return t ? t->stream : NULL;
}

bool csi_mgr_is_enabled(void)
{
    return s_csi_enabled;
}
