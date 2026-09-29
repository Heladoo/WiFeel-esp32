/* Tiny read-only web UI: "/" = self-contained HTML page, "/api/status" = JSON.
 * Local network only, no auth/TLS — fine for a LAN status page, not for exposure. */
#include "web_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "web";

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Device</title><body style='font-family:sans-serif;background:#101418;color:#e8eef2'>"
    "<h1>Device</h1><pre id=o>loading…</pre><script>"
    "async function t(){try{const r=await fetch('/api/status');"
    "o.textContent=JSON.stringify(await r.json(),null,2)}catch(e){o.textContent='offline'}}"
    "t();setInterval(t,1000)</script>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char buf[256];   /* small bodies on the stack; heap-allocate bigger ones */
    snprintf(buf, sizeof(buf), "{\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 "}",
             esp_timer_get_time() / 1000000, (uint32_t)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Default is tskIDLE_PRIORITY+5, higher than typical app tasks. On WiFeel this starved
     * networking and the UI task. Keep it low so the LVGL/UI tasks always preempt it. */
    cfg.task_priority = tskIDLE_PRIORITY + 1;
    cfg.stack_size = 6144;
    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t st = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &st);
    return ESP_OK;
}
