#include "console_cmds.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_mgr.h"
#include "csi_mgr.h"
#include "ping_gw.h"
#include "espnow_rx_test.h"
#include "motion.h"
#include "presence.h"
#include "board.h"
#include "wifeel_csi.h"
#include "ble_scan.h"
#include "devices.h"
#include "esp_timer.h"

static const char *TAG = "console";

#define JOIN_TIMEOUT_MS 15000

static int cmd_join(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: join <ssid> [password]\n");
        return 1;
    }
    const char *ssid = argv[1];
    const char *password = (argc >= 3) ? argv[2] : NULL;

    printf("joining \"%s\"...\n", ssid);
    esp_err_t err = wifi_mgr_join(ssid, password, JOIN_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("join failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* CSI + gateway-ping setup happens automatically via wifi_mgr's
     * connected-callback (see app_main.c's on_wifi_connected) — it fires
     * for this join same as it would for an automatic NVS-based reconnect
     * on a future boot, so this command doesn't duplicate that logic. */
    uint8_t bssid[6];
    uint8_t channel = 0;
    esp_ip4_addr_t gw = {0};
    ESP_ERROR_CHECK(wifi_mgr_get_ap_bssid(bssid));
    ESP_ERROR_CHECK(wifi_mgr_get_ap_channel(&channel));
    ESP_ERROR_CHECK(wifi_mgr_get_gateway_ip(&gw));

    printf("joined. AP=" MACSTR " channel=%u gateway=" IPSTR "\n"
           "run 'status' in a few seconds to check S1 pkt/s\n",
           MAC2STR(bssid), channel, IP2STR(&gw));
    return 0;
}

/* TEMPORARY diagnostic (milestone 4 hardware bring-up, no password needed):
 * tune to a channel and track a target MAC's CSI WITHOUT associating —
 * to test whether CSI is only ever reported for traffic outside our own
 * active association (see csi_mgr.c's on_csi diagnostic logging). This is
 * mechanically the core of the plan's PASSIVE mode, tested early. Not
 * wired into the real PASSIVE console command yet — that's milestone 5. */
static int cmd_sniff(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: sniff <bssid AA:BB:CC:DD:EE:FF> <channel>\n");
        return 1;
    }
    uint8_t mac[6];
    int vals[6];
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        printf("bad bssid format\n");
        return 1;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)vals[i];
    }
    uint8_t channel = (uint8_t)atoi(argv[2]);

    /* Stop the gateway ping FIRST — otherwise once we disconnect below it
     * floods the log with "ping_sock: send error" at ~100Hz (learned the
     * hard way: it swamped an earlier sniff test's output before any real
     * CSI data could be seen). */
    ping_gw_stop();

    /* Stop the driver from immediately reconnecting us to the last-joined
     * AP — otherwise association happens within ~500ms and defeats the
     * point of this test (capturing CSI WITHOUT being associated). */
    wifi_mgr_set_auto_reconnect(false);
    wifi_mgr_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        printf("esp_wifi_set_channel failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    uint8_t actual_primary = 0;
    wifi_second_chan_t actual_second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&actual_primary, &actual_second);
    csi_mgr_set_source_mac(WIFEEL_STREAM_ROUTER_TO_HUB, mac);
    csi_mgr_set_enabled(true);

    printf("sniffing channel %u (readback: actually on %u) for " MACSTR
           " (not associating) — check 'status' or watch the raw-frame log\n",
           channel, actual_primary, MAC2STR(mac));
    return 0;
}

/* TEMPORARY diagnostic: retarget S1's tracked MAC without touching Wi-Fi
 * state at all (no disconnect, no channel change) — for testing CSI on
 * co-channel non-associated traffic (e.g. another board's ESP-NOW frames
 * on the same channel as our current AP) while staying connected. */
static int cmd_track(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: track <mac AA:BB:CC:DD:EE:FF>\n");
        return 1;
    }
    uint8_t mac[6];
    int vals[6];
    if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        printf("bad mac format\n");
        return 1;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)vals[i];
    }
    csi_mgr_set_source_mac(WIFEEL_STREAM_ROUTER_TO_HUB, mac);
    csi_mgr_set_enabled(true);
    printf("S1 now tracking " MACSTR " (Wi-Fi state untouched, still on whatever channel we were on)\n",
           MAC2STR(mac));
    return 0;
}

/* TEMPORARY diagnostic: plain ESP-NOW reception, no CSI — isolates
 * "not reaching us" from "reaching us but CSI doesn't tap it". */
static int cmd_espnow_rx(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = espnow_rx_test_start();
    if (err != ESP_OK) {
        printf("espnow_rx_test_start failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("ESP-NOW RX test running — watch the log for incoming frames\n");
    return 0;
}

/* Streams live motion readings for tuning against a real "walk past the
 * hub" test — watch raw_jitter/score change as you move. */
static int cmd_motion(int argc, char **argv)
{
    int seconds = (argc >= 2) ? atoi(argv[1]) : 20;
    if (seconds <= 0) {
        seconds = 20;
    }
    printf("streaming motion readings for %d s (Ctrl+C not supported here — just wait it out)\n", seconds);
    for (int i = 0; i < seconds * 1000 / 300; i++) {
        printf("fused=%3u flag=%-6s | S1: score=%3u jitter=%.2f floor=%.2f | S3: score=%3u jitter=%.2f floor=%.2f\n",
               motion_get_score(), motion_get_flag() ? "MOTION" : "still",
               motion_get_s1_score(), (double)motion_get_raw_jitter(), (double)motion_get_floor(),
               motion_get_s3_score(), (double)motion_get_s3_raw_jitter(), (double)motion_get_s3_floor());
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return 0;
}

/* Starts empty-room calibration (P2 presence needs this before wander
 * means anything — see presence.h). Room must stay empty for the whole
 * duration. */
static int cmd_calib(int argc, char **argv)
{
    uint32_t duration_s = (argc >= 2) ? (uint32_t)atoi(argv[1]) : 30;
    if (duration_s == 0) {
        duration_s = 30;
    }
    esp_err_t err = presence_calibrate_start(duration_s * 1000);
    if (err != ESP_OK) {
        printf("warning: presence_calibrate_start reported %s (a stream may not be tracking "
               "anything yet — still armed for whichever stream(s) are)\n", esp_err_to_name(err));
    }
    printf("calibrating for %" PRIu32 " s — leave the room empty until this finishes\n", duration_s);
    return 0;
}

/* `phones raw <seconds> [min_rssi]`: print adverts for fingerprinting real
 * phones. Hashed ids only — never the raw address. */
static int phones_raw(int seconds, int min_rssi)
{
    if (ble_scan_get_duty() == 0) {
        printf("BLE scanning is paused (phones duty 0) — nothing to capture\n");
        return 1;
    }
    esp_err_t err = ble_scan_raw_start();
    if (err != ESP_OK) {
        printf("raw capture failed to start: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("capturing adverts for %d s (rssi >= %d)...\n", seconds, min_rssi);
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    unsigned shown = 0, dropped_weak = 0;
    ble_scan_raw_adv_t adv;
    while (esp_timer_get_time() < end) {
        if (!ble_scan_raw_next(&adv, 200)) {
            continue;
        }
        if (adv.rssi < min_rssi) {
            dropped_weak++;
            continue;
        }
        wifeel_vendor_t vendor;
        uint8_t apple_type;
        bool phone = ble_scan_classify(adv.data, adv.len, &vendor, &apple_type);
        printf("id=%04x rssi=%4d evt=%u vendor=%-9s phone=%-3s apple=%02x data=",
               devices_id_hash(adv.addr), adv.rssi, adv.event_type,
               wifeel_vendor_name(vendor), phone ? "yes" : "no", apple_type);
        for (uint8_t i = 0; i < adv.len; i++) {
            printf("%02x", adv.data[i]);
        }
        printf("\n");
        shown++;
    }
    ble_scan_raw_stop();
    printf("done: %u shown, %u below rssi filter\n", shown, dropped_weak);
    return 0;
}

static int cmd_phones(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "raw") == 0) {
        int seconds = (argc >= 3) ? atoi(argv[2]) : 10;
        int min_rssi = (argc >= 4) ? atoi(argv[3]) : -127;
        return phones_raw(seconds > 0 ? seconds : 10, min_rssi);
    }
    if (argc >= 2 && strcmp(argv[1], "duty") == 0) {
        if (argc < 3) {
            printf("BLE scan duty: %u%%\n", ble_scan_get_duty());
            return 0;
        }
        int pct = atoi(argv[2]);
        if (pct < 0 || pct > 100 || ble_scan_set_duty((uint8_t)pct) != ESP_OK) {
            printf("usage: phones duty <0-100>\n");
            return 1;
        }
        printf("BLE scan duty set to %d%%%s\n", pct, pct == 0 ? " (paused)" : "");
        return 0;
    }
    printf("BLE scan duty: %u%%  adverts since boot: %" PRIu32 "\n",
           ble_scan_get_duty(), ble_scan_adv_count());
    return 0;
}

static const char *presence_state_name(wifeel_presence_state_t s)
{
    switch (s) {
        case WIFEEL_PRESENCE_EMPTY: return "EMPTY";
        case WIFEEL_PRESENCE_PRESENT_STILL: return "PRESENT_STILL";
        case WIFEEL_PRESENCE_MOTION: return "MOTION";
        default: return "?";
    }
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("firmware:  %s (target " CONFIG_IDF_TARGET ")\n", WIFEEL_SENSE_FW_VERSION);
    printf("antenna:   %s\n", board_has_external_antenna() ? "external" : "onboard");
    printf("free heap: %" PRIu32 " bytes\n", (uint32_t)esp_get_free_heap_size());

    if (!wifi_mgr_is_connected()) {
        printf("wifi:      not connected (try 'join <ssid> [password]', or this may be "
               "intentional — 'sniff'/'track' don't associate)\n");
    } else {
        uint8_t bssid[6];
        uint8_t channel = 0;
        esp_ip4_addr_t gw = {0};
        wifi_mgr_get_ap_bssid(bssid);
        wifi_mgr_get_ap_channel(&channel);
        wifi_mgr_get_gateway_ip(&gw);
        printf("wifi:      connected, AP=" MACSTR " channel=%u gateway=" IPSTR "\n",
               MAC2STR(bssid), channel, IP2STR(&gw));
    }

    /* CSI reporting below doesn't require an active Wi-Fi connection —
     * 'sniff'/'track' both run CSI while disconnected (see their docs). */
    printf("csi:       %s\n", csi_mgr_is_enabled() ? "enabled" : "disabled");

    wifeel_csi_stream_t *s1 = csi_mgr_get_stream(WIFEEL_STREAM_ROUTER_TO_HUB);
    float pkt_rate = wifeel_csi_stream_get_pkt_rate(s1);
    printf("S1 (router->hub): %.1f pkt/s", (double)pkt_rate);

    wifeel_msg_features_t feat;
    if (wifeel_csi_stream_get_features(s1, &feat)) {
        printf(", mean_amp=%.2f wander=%.2f jitter=%.2f rssi=%.1f+-%.1f",
               (double)feat.mean_amplitude, (double)feat.wander, (double)feat.jitter_energy,
               (double)feat.rssi_mean, (double)feat.rssi_std);
    } else {
        printf(" (warming up, not enough history yet)");
    }
    printf("\n");
    if (ping_gw_is_running()) {
        printf("S1 gateway ping: %s\n", ping_gw_has_succeeded()
               ? "answering"
               : "no replies (network doesn't answer ICMP; S1 watchdog inactive)");
    }

    printf("motion:    fused=%u flag=%s  (S1 score=%u jitter=%.2f floor=%.2f, S3 score=%u jitter=%.2f floor=%.2f)\n",
           motion_get_score(), motion_get_flag() ? "MOTION" : "still",
           motion_get_s1_score(), (double)motion_get_raw_jitter(), (double)motion_get_floor(),
           motion_get_s3_score(), (double)motion_get_s3_raw_jitter(), (double)motion_get_s3_floor());

    wifeel_csi_stream_t *s3 = csi_mgr_get_stream(WIFEEL_STREAM_DISPLAY_TO_HUB);
    printf("S3 (display->hub): %.1f pkt/s\n", (double)wifeel_csi_stream_get_pkt_rate(s3));

    if (presence_is_calibrating()) {
        printf("presence:  calibrating (%" PRIu32 " s remaining)\n",
               presence_calibrate_remaining_ms() / 1000);
    } else {
        printf("presence:  %s  calibrated=%s  (S1 wander=%.2f, S3 wander=%.2f)\n",
               presence_state_name(presence_get_state()), presence_is_calibrated() ? "yes" : "no",
               (double)presence_get_s1_wander(), (double)presence_get_s3_wander());
    }
    return 0;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "wifeel>";
    repl_config.max_cmdline_length = 256;

    esp_err_t err;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t jtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&jtag_config, &repl_config, &repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create console REPL: %s", esp_err_to_name(err));
        return err;
    }

    esp_console_register_help_command();

    const esp_console_cmd_t join_cmd = {
        .command = "join",
        .help = "Join a Wi-Fi network and start tracking CSI from its router (S1)",
        .hint = "<ssid> [password]",
        .func = &cmd_join,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&join_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show link, CSI and heap status",
        .hint = NULL,
        .func = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t sniff_cmd = {
        .command = "sniff",
        .help = "[diagnostic] Track CSI for a MAC on a channel WITHOUT associating",
        .hint = "<bssid> <channel>",
        .func = &cmd_sniff,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sniff_cmd));

    const esp_console_cmd_t track_cmd = {
        .command = "track",
        .help = "[diagnostic] Retarget S1's tracked MAC without touching Wi-Fi state",
        .hint = "<mac>",
        .func = &cmd_track,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&track_cmd));

    const esp_console_cmd_t espnow_rx_cmd = {
        .command = "espnow_rx",
        .help = "[diagnostic] Start a plain ESP-NOW receiver (no CSI) to check raw reception",
        .hint = NULL,
        .func = &cmd_espnow_rx,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&espnow_rx_cmd));

    const esp_console_cmd_t motion_cmd = {
        .command = "motion",
        .help = "Stream live motion score/jitter readings (for walk-by tuning)",
        .hint = "[seconds]",
        .func = &cmd_motion,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&motion_cmd));

    const esp_console_cmd_t calib_cmd = {
        .command = "calib",
        .help = "Start empty-room calibration for presence (P2) — room must stay empty",
        .hint = "[seconds, default 30]",
        .func = &cmd_calib,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&calib_cmd));

    const esp_console_cmd_t phones_cmd = {
        .command = "phones",
        .help = "Nearby phones: summary | raw <s> [min_rssi] | duty <0-100>",
        .hint = "[raw <s> [min_rssi] | duty <pct>]",
        .func = &cmd_phones,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&phones_cmd));

    return esp_console_start_repl(repl);
}
