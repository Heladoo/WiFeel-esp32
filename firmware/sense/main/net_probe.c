#include "net_probe.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <fcntl.h>

#include "lwip/sockets.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"

static const char *TAG = "net_probe";

#define REPLY_RATE_WINDOW_US 1000000
#define RECV_DRAIN_BUDGET    8   /* replies to drain per send, keeps the loop bounded */

static TaskHandle_t        s_task;
static volatile bool       s_stop;
static net_probe_method_t  s_method;
static esp_netif_t        *s_netif;
static esp_ip4_addr_t      s_gateway;
static esp_ip4_addr_t      s_dns;
static uint32_t            s_interval_ms;

static volatile uint32_t s_sends;
static volatile uint32_t s_replies;
static volatile float    s_reply_rate;
static volatile bool     s_has_succeeded;
static volatile int64_t  s_last_success_us;

const char *net_probe_method_name(net_probe_method_t m)
{
    switch (m) {
        case NET_PROBE_METHOD_DNS: return "dns";
        case NET_PROBE_METHOD_ARP: return "arp";
        default:                   return "none";
    }
}

bool net_probe_method_from_string(const char *s, net_probe_method_t *out)
{
    if (!s || !out) {
        return false;
    }
    if (strcmp(s, "dns") == 0)  { *out = NET_PROBE_METHOD_DNS;  return true; }
    if (strcmp(s, "arp") == 0)  { *out = NET_PROBE_METHOD_ARP;  return true; }
    if (strcmp(s, "none") == 0) { *out = NET_PROBE_METHOD_NONE; return true; }
    return false;
}

/*
 * A minimal DNS A-query for "wifeel-probe.invalid".
 *
 * .invalid is reserved by RFC 2606 and can never resolve, so this asks the
 * resolver for something guaranteed not to exist and guaranteed not to touch
 * any real domain. The expected answer is NXDOMAIN — which is exactly as
 * useful as a real answer here, because what we need is the AP transmitting a
 * frame to us, not the record. Query the same name every time so a caching
 * forwarder answers locally instead of going upstream.
 */
static size_t build_dns_query(uint8_t *out, size_t cap, uint16_t id)
{
    static const char *labels[] = { "wifeel-probe", "invalid" };
    size_t need = 12;
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        need += 1 + strlen(labels[i]);
    }
    need += 1 + 4; /* root label + QTYPE + QCLASS */
    if (cap < need) {
        return 0;
    }

    size_t n = 0;
    out[n++] = (uint8_t)(id >> 8);
    out[n++] = (uint8_t)(id & 0xFF);
    out[n++] = 0x01; out[n++] = 0x00;  /* standard query, recursion desired */
    out[n++] = 0x00; out[n++] = 0x01;  /* QDCOUNT = 1 */
    out[n++] = 0x00; out[n++] = 0x00;  /* ANCOUNT */
    out[n++] = 0x00; out[n++] = 0x00;  /* NSCOUNT */
    out[n++] = 0x00; out[n++] = 0x00;  /* ARCOUNT */
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        size_t len = strlen(labels[i]);
        out[n++] = (uint8_t)len;
        memcpy(out + n, labels[i], len);
        n += len;
    }
    out[n++] = 0x00;                   /* root label */
    out[n++] = 0x00; out[n++] = 0x01;  /* QTYPE  = A  */
    out[n++] = 0x00; out[n++] = 0x01;  /* QCLASS = IN */
    return n;
}

static void note_reply(void)
{
    s_replies++;
    s_last_success_us = esp_timer_get_time();
    s_has_succeeded = true;
}

static void update_reply_rate(int64_t *window_start_us, uint32_t *window_start_replies)
{
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - *window_start_us;
    if (elapsed < REPLY_RATE_WINDOW_US) {
        return;
    }
    uint32_t delta = s_replies - *window_start_replies;
    s_reply_rate = (float)delta * (1000000.0f / (float)elapsed);
    *window_start_us = now;
    *window_start_replies = s_replies;
}

static void probe_dns_loop(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return;
    }
    /* Non-blocking recv: this loop must keep to its send cadence regardless of
     * whether anything ever answers, so it drains whatever has arrived and
     * moves on rather than waiting on a reply. (Note SO_RCVTIMEO of 0 would
     * mean "block forever" in lwIP, not "don't wait" — O_NONBLOCK is what
     * actually gives a non-blocking recv here.) */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "fcntl(O_NONBLOCK) failed: errno %d", errno);
        close(sock);
        return;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    /* Prefer the DHCP-provided resolver; on most home/guest networks that IS
     * the gateway, and where it isn't, the reply still arrives through the AP
     * either way, which is all S1 needs. */
    dest.sin_addr.s_addr = s_dns.addr ? s_dns.addr : s_gateway.addr;

    ESP_LOGI(TAG, "dns probe -> " IPSTR ":53 every %" PRIu32 " ms",
             IP2STR(s_dns.addr ? &s_dns : &s_gateway), s_interval_ms);

    uint8_t query[64];
    uint8_t reply[128];
    uint16_t id = 1;
    int64_t win_us = esp_timer_get_time();
    uint32_t win_replies = s_replies;

    while (!s_stop) {
        size_t qlen = build_dns_query(query, sizeof(query), id++);
        if (qlen > 0) {
            if (sendto(sock, query, qlen, 0, (struct sockaddr *)&dest, sizeof(dest)) > 0) {
                s_sends++;
            }
        }
        for (int i = 0; i < RECV_DRAIN_BUDGET; i++) {
            int got = recvfrom(sock, reply, sizeof(reply), 0, NULL, NULL);
            if (got <= 0) {
                break;
            }
            note_reply();
        }
        update_reply_rate(&win_us, &win_replies);
        vTaskDelay(pdMS_TO_TICKS(s_interval_ms));
    }
    close(sock);
}

static void probe_arp_loop(void)
{
    struct netif *lwip_netif = esp_netif_get_netif_impl(s_netif);
    if (!lwip_netif) {
        ESP_LOGE(TAG, "no lwIP netif for this interface");
        return;
    }
    ip4_addr_t target;
    target.addr = s_gateway.addr;

    ESP_LOGI(TAG, "arp probe -> " IPSTR " every %" PRIu32 " ms",
             IP2STR(&s_gateway), s_interval_ms);

    int64_t win_us = esp_timer_get_time();
    uint32_t win_replies = s_replies;
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;

    while (!s_stop) {
        /* Drop any cached entry first, otherwise lwIP answers from its own
         * table and never puts a request on the air. */
        etharp_cleanup_netif(lwip_netif);
        if (etharp_request(lwip_netif, &target) == ERR_OK) {
            s_sends++;
        }
        vTaskDelay(pdMS_TO_TICKS(s_interval_ms));
        /* An entry reappearing in the ARP table means the gateway answered.
         * There is no per-reply callback for ARP, so this is the available
         * signal — it undercounts (several replies can collapse into one
         * observation) but it is a truthful "the gateway is answering". */
        if (etharp_find_addr(lwip_netif, &target, &eth_ret, &ip_ret) >= 0) {
            note_reply();
        }
        update_reply_rate(&win_us, &win_replies);
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    switch (s_method) {
        case NET_PROBE_METHOD_DNS: probe_dns_loop(); break;
        case NET_PROBE_METHOD_ARP: probe_arp_loop(); break;
        default: break;
    }
    ESP_LOGI(TAG, "probe task exiting (method=%s)", net_probe_method_name(s_method));
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t net_probe_start(net_probe_method_t method, esp_netif_t *netif,
                          const esp_ip4_addr_t *gateway, uint32_t interval_ms)
{
    if (!netif || !gateway || interval_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    net_probe_stop();

    if (method == NET_PROBE_METHOD_NONE) {
        s_method = method;
        return ESP_OK;
    }

    s_method = method;
    s_netif = netif;
    s_gateway = *gateway;
    s_interval_ms = interval_ms;

    esp_netif_dns_info_t dns = {0};
    s_dns.addr = 0;
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        s_dns.addr = dns.ip.u_addr.ip4.addr;
    }

    s_sends = 0;
    s_replies = 0;
    s_reply_rate = 0.0f;
    s_has_succeeded = false;
    /* Seed with "now" so ms_since_last_success() doesn't read as a huge stall
     * in the window before the first reply. */
    s_last_success_us = esp_timer_get_time();
    s_stop = false;

    BaseType_t ok = xTaskCreate(&probe_task, "net_probe", 3584, NULL,
                                 tskIDLE_PRIORITY + 1, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        s_method = NET_PROBE_METHOD_NONE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_probe_stop(void)
{
    if (!s_task) {
        s_method = NET_PROBE_METHOD_NONE;
        return;
    }
    s_stop = true;
    /* The loops check s_stop once per interval; wait out a couple of those
     * plus slack so the socket is closed before a restart reopens one. */
    for (int i = 0; i < 50 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_method = NET_PROBE_METHOD_NONE;
}

bool net_probe_is_running(void)              { return s_task != NULL; }
net_probe_method_t net_probe_get_method(void) { return s_method; }
uint32_t net_probe_reply_count(void)          { return s_replies; }
float    net_probe_reply_rate(void)           { return s_reply_rate; }
uint32_t net_probe_send_count(void)           { return s_sends; }
bool     net_probe_has_succeeded(void)        { return s_task && s_has_succeeded; }

uint32_t net_probe_ms_since_last_success(void)
{
    if (!s_task) {
        return 0;
    }
    int64_t elapsed = esp_timer_get_time() - s_last_success_us;
    if (elapsed < 0) {
        elapsed = 0;
    }
    return (uint32_t)(elapsed / 1000);
}
