#include "ping_gw.h"

#include <inttypes.h>
#include "ping/ping_sock.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ping_gw";
static esp_ping_handle_t s_session;
static bool s_running;
static int64_t s_last_success_us;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    s_last_success_us = esp_timer_get_time();
}

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

    /* on_ping_success is the only callback we need: it's the liveness
     * signal ping_gw_ms_since_last_success() reports on. We don't hook
     * on_ping_timeout — a dead link has been observed to fail at the
     * socket *send* itself (logged directly by the ping_sock component,
     * outside any of esp_ping's callbacks), which doesn't necessarily
     * ever reach a timeout callback either. Measuring "time since last
     * success" catches both failure shapes without needing to know
     * which one is happening. */
    esp_ping_callbacks_t cbs = {
        .on_ping_success = &on_ping_success,
    };

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

    /* Seed with "now" rather than 0 so the watchdog doesn't see a huge
     * apparent stall in the brief window before the first real reply. */
    s_last_success_us = esp_timer_get_time();
    s_running = true;

    ESP_LOGI(TAG, "pinging " IPSTR " every %" PRIu32 " ms", IP2STR(target), interval_ms);
    return ESP_OK;
}

void ping_gw_stop(void)
{
    s_running = false;
    if (s_session) {
        esp_ping_stop(s_session);
        esp_ping_delete_session(s_session);
        s_session = NULL;
    }
}

bool ping_gw_is_running(void)
{
    return s_running;
}

uint32_t ping_gw_ms_since_last_success(void)
{
    if (!s_running) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_last_success_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}
