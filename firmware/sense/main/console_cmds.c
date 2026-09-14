#include "console_cmds.h"

#include <string.h>
#include <inttypes.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"

#include "wifi_mgr.h"
#include "csi_mgr.h"
#include "ping_gw.h"
#include "board.h"
#include "wifeel_csi.h"

static const char *TAG = "console";

/* ~100 Hz per the plan's JOIN mode design. */
#define JOIN_PING_INTERVAL_MS 10
#define JOIN_TIMEOUT_MS       15000

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

    uint8_t bssid[6];
    uint8_t channel = 0;
    esp_ip4_addr_t gw = {0};
    ESP_ERROR_CHECK(wifi_mgr_get_ap_bssid(bssid));
    ESP_ERROR_CHECK(wifi_mgr_get_ap_channel(&channel));
    ESP_ERROR_CHECK(wifi_mgr_get_gateway_ip(&gw));

    err = csi_mgr_set_source_mac(WIFEEL_STREAM_ROUTER_TO_HUB, bssid);
    if (err != ESP_OK) {
        printf("warning: csi_mgr_set_source_mac failed: %s\n", esp_err_to_name(err));
    }
    err = csi_mgr_set_enabled(true);
    if (err != ESP_OK) {
        printf("warning: csi_mgr_set_enabled failed: %s\n", esp_err_to_name(err));
    }
    err = ping_gw_start(&gw, JOIN_PING_INTERVAL_MS);
    if (err != ESP_OK) {
        printf("warning: ping_gw_start failed: %s\n", esp_err_to_name(err));
    }

    printf("joined. AP=" MACSTR " channel=%u gateway=" IPSTR "\n"
           "pinging gateway every %d ms — run 'status' in a few seconds to check S1 pkt/s\n",
           MAC2STR(bssid), channel, IP2STR(&gw), JOIN_PING_INTERVAL_MS);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("firmware:  %s (target " CONFIG_IDF_TARGET ")\n", WIFEEL_SENSE_FW_VERSION);
    printf("antenna:   %s\n", board_has_external_antenna() ? "external" : "onboard");
    printf("free heap: %" PRIu32 " bytes\n", (uint32_t)esp_get_free_heap_size());

    if (!wifi_mgr_is_connected()) {
        printf("wifi:      not connected — try 'join <ssid> [password]'\n");
        return 0;
    }

    uint8_t bssid[6];
    uint8_t channel = 0;
    esp_ip4_addr_t gw = {0};
    wifi_mgr_get_ap_bssid(bssid);
    wifi_mgr_get_ap_channel(&channel);
    wifi_mgr_get_gateway_ip(&gw);
    printf("wifi:      connected, AP=" MACSTR " channel=%u gateway=" IPSTR "\n",
           MAC2STR(bssid), channel, IP2STR(&gw));

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

    return esp_console_start_repl(repl);
}
