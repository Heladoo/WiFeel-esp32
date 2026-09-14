#include "csi_mgr.h"

#include <string.h>
#include <inttypes.h>
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

/* A per-frame debug log ("raw CSI frame #N from <mac>") lived here during
 * milestone 4 hardware bring-up, to tell apart "the callback never fires"
 * from "it fires but nothing matches our filter" — real hardware
 * confirmed the pipeline works end-to-end (S1 ~4 pkt/s, S3 ~100 pkt/s;
 * see docs/boards.md), so it's removed now. It was also getting in the
 * way of longer unattended captures at S3's rate. Re-add temporarily
 * (rate-limited!) if diagnosing CSI capture again — don't log every frame
 * unconditionally at 100pkt/s. */
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

    /* S3 (display->hub): the hub also runs its own SoftAP that the
     * display joins as a station (see wifi_mgr.h's AP support) — a
     * second, independent sensing vantage point, per the plan's original
     * multi-stream design. Target MAC is set once the display actually
     * associates (see wifi_mgr_set_ap_peer_connected_cb in app_main.c). */
    wifeel_csi_stream_config_t s3_cfg = {
        .id = WIFEEL_STREAM_DISPLAY_TO_HUB,
        .has_baseline = false,
    };
    tracked_stream_t *s3 = find_slot(WIFEEL_STREAM_DISPLAY_TO_HUB);
    s3->stream = wifeel_csi_stream_init(&s3_cfg);
    if (!s3->stream) {
        return ESP_ERR_NO_MEM;
    }

    /* Required even though the hub is (or will be) associated to an AP as
     * a STA: the CSI extraction hardware only taps frames seen via the
     * promiscuous RX path, not the normal STA data path. Confirmed against
     * Espressif's own esp-csi get-started/csi_recv example, which calls
     * this first, before esp_wifi_set_csi_config() — without it, CSI never
     * fires at all (esp_wifi_set_csi(true) "succeeds" but no callback ever
     * runs), which is silent and easy to miss without a real pkt/s check.
     * Safe to enable alongside an active STA connection: it just adds a
     * monitoring tap and doesn't disrupt normal association/IP traffic. */
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_promiscuous failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_csi_rx_cb(&on_csi, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_csi_rx_cb failed: %s", esp_err_to_name(err));
        return err;
    }

    /* wifi_csi_config_t is a completely different struct depending on
     * whether the target has an HE (Wi-Fi 6) radio (see
     * esp_wifi_types_native.h, and wifeel_csi.c's comment on the same) —
     * not just different field names within one shape, as an earlier
     * version of this code assumed. That earlier "portable" version
     * compiled fine here (esp32c6 is HE) but would NOT have compiled at
     * all on a non-HE target like esp32c3, and — the actual bug that sent
     * pkt/s to zero on real hardware — silently omitted
     * .acquire_csi_he_stbc, which Espressif's own esp-csi reference
     * example sets explicitly for esp32c6. Values below are copied from
     * that reference (esp-csi/examples/get-started/csi_recv), not
     * guessed. */
#if CONFIG_SOC_WIFI_HE_SUPPORT
    wifi_csi_config_t csi_config = {
        .enable = true,
        /* TESTING: legacy=true (unlike the reference config, which leaves
         * this off) — ESP-NOW broadcast frames may go out at a legacy/
         * basic PHY rate for reliability, which acquire_csi_legacy=false
         * would silently exclude from CSI entirely. See docs/boards.md's
         * DIRECT-mode CSI test notes. */
        .acquire_csi_legacy = true,
        .acquire_csi_ht20 = true,
        .acquire_csi_ht40 = true,
        .acquire_csi_su = true,
        .acquire_csi_mu = true,
        .acquire_csi_dcm = true,
        .acquire_csi_beamformed = true,
        .acquire_csi_he_stbc = 2,
        .val_scale_cfg = 0,
        .dump_ack_en = false,
    };
#else
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .dump_ack_en = false,
    };
#endif
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
