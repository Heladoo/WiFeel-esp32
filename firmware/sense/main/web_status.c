#include "web_status.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "wifi_mgr.h"
#include "csi_mgr.h"
#include "wifeel_csi.h"
#include "motion.h"
#include "presence.h"
#include "devices.h"
#include "ble_scan.h"

static const char *TAG = "web_status";
static httpd_handle_t s_server;

/* Generous fixed size for the JSON body — heap-allocated per request
 * (free heap runs ~200KB, see docs/boards.md) rather than a stack buffer,
 * so this doesn't depend on tuning the httpd worker task's stack size.
 * Worst case is WIFEEL_DEVICES_MAX_ENTRIES (8) device rows plus the fixed
 * fields above them — comfortably under this with room to spare. */
#define JSON_BUF_SIZE 6144

static const char *presence_state_name(wifeel_presence_state_t s)
{
    switch (s) {
        case WIFEEL_PRESENCE_EMPTY:         return "EMPTY";
        case WIFEEL_PRESENCE_PRESENT_STILL: return "PRESENT_STILL";
        case WIFEEL_PRESENCE_MOTION:        return "MOTION";
        default:                             return "UNKNOWN";
    }
}

/* Mirrors firmware/display/main/ui_phones.c's type_icon_symbol() logic —
 * same classification, just named instead of drawn as a glyph. Keep the
 * two in sync if the classifier's rules change. */
static const char *device_type_name(uint8_t vendor, uint8_t phone_like)
{
    if (phone_like) {
        return "phone";
    }
    if ((wifeel_vendor_t)vendor == WIFEEL_VENDOR_MICROSOFT) {
        return "computer";
    }
    return "other";
}

/* Mirrors firmware/display/main/ui_phones.c's range_text() — same
 * <1m/X.Xm/3-6m/6m+/Unknown zones, so the web view and the AMOLED never
 * disagree about what a given reading means. */
static void range_text(char *out, size_t out_len, uint8_t distance_dm)
{
    if (distance_dm == 255) {
        snprintf(out, out_len, "Unknown");
    } else if (distance_dm < 10) {
        snprintf(out, out_len, "<1m");
    } else if (distance_dm < 30) {
        snprintf(out, out_len, "%.1fm", (double)distance_dm / 10.0);
    } else if (distance_dm < 60) {
        snprintf(out, out_len, "3-6m");
    } else {
        snprintf(out, out_len, "6m+");
    }
}

/* Minimal JSON string escaping — only SSID is untrusted-ish (a Wi-Fi SSID
 * is technically an arbitrary byte string up to 32 bytes, typed by
 * whoever ran `join`), everything else here comes from fixed C strings or
 * numbers. Escapes just enough to keep the JSON well-formed; drops
 * control characters rather than \u-encoding them, fine for a status
 * display. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } /* else: drop control/non-ASCII bytes */
    }
    out[o] = '\0';
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int n = 0;

    n += snprintf(buf + n, JSON_BUF_SIZE - n,
                  "{\"fw\":\"%s\",\"target\":\"" CONFIG_IDF_TARGET "\",\"antenna\":\"%s\","
                  "\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 ",",
                  WIFEEL_SENSE_FW_VERSION, board_has_external_antenna() ? "external" : "onboard",
                  esp_timer_get_time() / 1000000, (uint32_t)esp_get_free_heap_size());

    if (wifi_mgr_is_connected()) {
        char ssid[WIFEEL_SSID_MAX_LEN + 1] = {0};
        char ssid_esc[WIFEEL_SSID_MAX_LEN * 2 + 1];
        uint8_t channel = 0;
        esp_ip4_addr_t gw = {0};
        wifi_mgr_get_ap_ssid(ssid, sizeof(ssid));
        wifi_mgr_get_ap_channel(&channel);
        wifi_mgr_get_gateway_ip(&gw);
        json_escape(ssid, ssid_esc, sizeof(ssid_esc));
        n += snprintf(buf + n, JSON_BUF_SIZE - n,
                      "\"wifi\":{\"connected\":true,\"ssid\":\"%s\",\"channel\":%u,\"gateway\":\"" IPSTR "\"},",
                      ssid_esc, channel, IP2STR(&gw));
    } else {
        n += snprintf(buf + n, JSON_BUF_SIZE - n, "\"wifi\":{\"connected\":false},");
    }

    wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
    wifeel_csi_stream_t *s3 = csi_mgr_get_stream(WIFEEL_STREAM_DISPLAY_TO_HUB);
    n += snprintf(buf + n, JSON_BUF_SIZE - n,
                  "\"csi\":{\"s1_pkt_rate\":%.1f,\"s3_pkt_rate\":%.1f},",
                  (double)wifeel_csi_stream_get_pkt_rate(s1), (double)wifeel_csi_stream_get_pkt_rate(s3));

    n += snprintf(buf + n, JSON_BUF_SIZE - n,
                  "\"motion\":{\"score\":%u,\"flag\":%s,\"threshold\":%u,\"s1_score\":%u,\"s3_score\":%u},",
                  motion_get_score(), motion_get_flag() ? "true" : "false",
                  motion_get_enter_threshold(), motion_get_s1_score(), motion_get_s3_score());

    n += snprintf(buf + n, JSON_BUF_SIZE - n,
                  "\"presence\":{\"state\":\"%s\",\"calibrated\":%s},",
                  presence_state_name(presence_get_state()), presence_is_calibrated() ? "true" : "false");

    wifeel_msg_devices_t d;
    devices_get_summary(&d);
    n += snprintf(buf + n, JSON_BUF_SIZE - n,
                  "\"phones\":{\"ble_count\":%u,\"wifi_count\":%u,\"scan_duty_pct\":%u,\"devices\":[",
                  d.ble_phone_count, d.wifi_client_count, d.scan_duty_pct);
    for (uint8_t i = 0; i < d.n_entries && n < JSON_BUF_SIZE - 200; i++) {
        const wifeel_msg_device_entry_t *e = &d.entries[i];
        char range[12];
        range_text(range, sizeof(range), e->distance_dm);
        n += snprintf(buf + n, JSON_BUF_SIZE - n,
                      "%s{\"id\":\"%04x\",\"source\":\"%s\",\"type\":\"%s\",\"vendor\":\"%s\","
                      "\"range\":\"%s\",\"rssi\":%d,\"connected\":%s,\"age_s\":%u}",
                      i == 0 ? "" : ",", e->id, e->source == WIFEEL_DEV_SRC_WIFI ? "WiFi" : "BLE",
                      device_type_name(e->vendor, e->phone_like), wifeel_vendor_name(e->vendor),
                      range, e->rssi, e->connected ? "true" : "false", e->age_s);
    }
    n += snprintf(buf + n, JSON_BUF_SIZE - n, "]}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

/* Single self-contained page: no external fonts/scripts/stylesheets, so it
 * works with no internet access — this only ever needs to be reachable on
 * the home Wi-Fi (see web_status.h). Polls /api/status every second and
 * updates the DOM in place rather than reloading. */
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>WiFeel status</title><style>"
"body{background:#101418;color:#E8EEF2;font:15px/1.4 -apple-system,Segoe UI,Helvetica,Arial,sans-serif;"
"max-width:640px;margin:0 auto;padding:16px}"
"h1{font-size:20px;font-weight:600;margin:0 0 4px}"
".muted{color:#8FA3AD;font-size:13px}"
".card{background:#181E24;border-radius:10px;padding:14px 16px;margin:14px 0}"
".row{display:flex;justify-content:space-between;align-items:center;padding:4px 0}"
".label{color:#8FA3AD}"
".big{font-size:34px;font-weight:700}"
".pill{display:inline-block;padding:2px 10px;border-radius:999px;font-size:13px;font-weight:600}"
".still{background:#3DAA6E33;color:#3DAA6E}"
".motion{background:#E8A33D33;color:#E8A33D}"
".grey{background:#3A475033;color:#8FA3AD}"
"table{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}"
"th{text-align:left;color:#8FA3AD;font-weight:600;padding:4px 6px;border-bottom:1px solid #2A3238}"
"td{padding:5px 6px;border-bottom:1px solid #1c232a}"
".bar{height:8px;border-radius:4px;background:#2A3238;overflow:hidden;margin-top:6px}"
".bar>div{height:100%;background:#4FA8E8}"
"#err{color:#E85D5D;display:none;margin:8px 0}"
"</style></head><body>"
"<h1>WiFeel</h1><div class=muted id=sub>connecting...</div>"
"<div id=err>Lost contact with the hub — retrying...</div>"

"<div class=card><div class=row><div><div class=label>Motion</div>"
"<div class=big id=motion-score>--</div></div><span class=pill id=motion-flag>--</span></div>"
"<div class=bar><div id=motion-bar style=width:0%></div></div>"
"<div class=row style=margin-top:8px><span class=muted>S1 router: <b id=s1>--</b></span>"
"<span class=muted>S3 display: <b id=s3>--</b></span>"
"<span class=muted>threshold: <b id=thresh>--</b></span></div></div>"

"<div class=card><div class=row><div class=label>Presence</div>"
"<span class=pill id=presence-pill>--</span></div>"
"<div class=muted id=presence-cal></div></div>"

"<div class=card><div class=row><div><div class=label>Nearby phones</div>"
"<div class=big id=phone-count>--</div></div>"
"<div style=text-align:right><div class=label>On home Wi-Fi</div>"
"<div class=big id=wifi-count>--</div></div></div>"
"<table><thead><tr><th>Type</th><th>Vendor</th><th>Range</th><th>RSSI</th><th>Age</th></tr></thead>"
"<tbody id=dev-rows></tbody></table>"
"<div class=muted id=dev-empty style=display:none;padding-top:6px>No devices tracked</div></div>"

"<div class=card><div class=row><span class=label>Network</span><span id=net>--</span></div>"
"<div class=row><span class=label>S1 / S3 pkt rate</span><span id=pkt>--</span></div>"
"<div class=row><span class=label>Firmware</span><span id=fw>--</span></div>"
"<div class=row><span class=label>Free heap</span><span id=heap>--</span></div>"
"<div class=row><span class=label>Uptime</span><span id=uptime>--</span></div></div>"

"<script>"
"function fmtDur(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;"
"return (h?h+'h ':'')+(h||m?m+'m ':'')+sec+'s'}"
"function typeIcon(t){return t=='phone'?'\\uD83D\\uDCF1':t=='computer'?'\\uD83D\\uDCBB':'\\uD83D\\uDD0C'}"
"async function tick(){"
"try{"
"const r=await fetch('/api/status',{cache:'no-store'});"
"if(!r.ok)throw new Error('bad status');"
"const d=await r.json();"
"document.getElementById('err').style.display='none';"
"document.getElementById('sub').textContent=d.fw+' on '+d.target;"
"document.getElementById('motion-score').textContent=d.motion.score;"
"document.getElementById('motion-bar').style.width=Math.min(100,d.motion.score)+'%';"
"const mp=document.getElementById('motion-flag');"
"mp.textContent=d.motion.flag?'MOTION':'still';mp.className='pill '+(d.motion.flag?'motion':'still');"
"document.getElementById('s1').textContent=d.motion.s1_score;"
"document.getElementById('s3').textContent=d.motion.s3_score;"
"document.getElementById('thresh').textContent=d.motion.threshold;"
"const pp=document.getElementById('presence-pill');"
"pp.textContent=d.presence.state;"
"pp.className='pill '+(d.presence.state=='EMPTY'?'grey':d.presence.state=='MOTION'?'motion':'still');"
"document.getElementById('presence-cal').textContent=d.presence.calibrated?'':'not calibrated yet — see \"calib\" on the console';"
"document.getElementById('phone-count').textContent=d.phones.ble_count;"
"document.getElementById('wifi-count').textContent=d.phones.wifi_count;"
"const rows=d.phones.devices.map(x=>"
"'<tr><td>'+typeIcon(x.type)+' '+x.type+'</td><td>'+x.vendor+'</td><td>'+x.range+'</td>"
"<td>'+x.rssi+'dBm</td><td>'+x.age_s+'s</td></tr>').join('');"
"document.getElementById('dev-rows').innerHTML=rows;"
"document.getElementById('dev-empty').style.display=d.phones.devices.length?'none':'block';"
"document.getElementById('net').textContent=d.wifi.connected?(d.wifi.ssid+' (ch '+d.wifi.channel+')'):'not connected';"
"document.getElementById('pkt').textContent=d.csi.s1_pkt_rate+' / '+d.csi.s3_pkt_rate+' pkt/s';"
"document.getElementById('fw').textContent=d.fw+' ('+d.antenna+' antenna)';"
"document.getElementById('heap').textContent=(d.free_heap/1024).toFixed(0)+' KB';"
"document.getElementById('uptime').textContent=fmtDur(d.uptime_s);"
"}catch(e){document.getElementById('err').style.display='block'}"
"}"
"tick();setInterval(tick,1000);"
"</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t web_status_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Default task_priority is tskIDLE_PRIORITY+5 — above every one of this
     * project's own tasks (devices/motion/presence/link run at +2, the
     * watchdogs at +1). On this single-core chip, already juggling STA+
     * SoftAP+BLE scan+CSI capture+Wi-Fi sniffing+ESP-NOW, that was enough
     * to starve real-time networking: ping_gw's raw socket sends started
     * failing continuously and esp_now_send began returning
     * ESP_ERR_ESPNOW_NO_MEM, badly enough that the display's SoftAP
     * association actually dropped and reconnected — confirmed live after
     * adding this server, not a guess. Dropped to +1 (same tier as the
     * watchdogs, always preemptable by the sensing-critical tasks) since a
     * status page has no latency requirement that justifies outranking
     * them. */
    config.task_priority = tskIDLE_PRIORITY + 1;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = &index_get_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = &status_get_handler};
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &status_uri);

    ESP_LOGI(TAG, "status dashboard started (reachable once the hub has an IP on your home Wi-Fi)");
    return ESP_OK;
}
