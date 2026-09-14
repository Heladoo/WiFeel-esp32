#include "ping_gw.h"

#include <inttypes.h>
#include "ping/ping_sock.h"
#include "esp_log.h"

static const char *TAG = "ping_gw";
static esp_ping_handle_t s_session;

esp_err_t ping_gw_start(const esp_ip4_addr_t *target, uint32_t interval_ms)
{
    if (!target) {
        return ESP_ERR_INVALID_ARG;
    }
    ping_gw_stop();

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr.type = IPADDR_TYPE_V4;
    /* esp_ip4_addr_t (our/esp_netif's type) and lwIP's own ip4_addr_t are
     * layout-identical (single uint32_t addr) but distinct C struct types,
     * so assign through the raw field rather than by struct copy. */
    config.target_addr.u_addr.ip4.addr = target->addr;
    config.count = 0; /* continuous, until ping_gw_stop() */
    config.interval_ms = interval_ms;
    config.timeout_ms = interval_ms > 20 ? interval_ms - 5 : interval_ms;
    config.data_size = 8; /* small payload: we only care about CSI on the reply, not the data */

    esp_ping_callbacks_t cbs = {0}; /* no callbacks needed — we read CSI, not ping RTT */

    esp_err_t err = esp_ping_new_session(&config, &cbs, &s_session);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ping_new_session failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ping_start(s_session);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ping_start failed: %s", esp_err_to_name(err));
        esp_ping_delete_session(s_session);
        s_session = NULL;
        return err;
    }

    ESP_LOGI(TAG, "pinging " IPSTR " every %" PRIu32 " ms", IP2STR(target), interval_ms);
    return ESP_OK;
}

void ping_gw_stop(void)
{
    if (s_session) {
        esp_ping_stop(s_session);
        esp_ping_delete_session(s_session);
        s_session = NULL;
    }
}
