/**
 * Read-only status dashboard, served by the hub's built-in HTTP server on
 * the home Wi-Fi network — so the system can be checked from a phone/PC
 * without walking up to the display's small screen.
 *
 * Deliberately local-network-only: no login, no HTTPS, no write endpoints.
 * That's a conscious scope choice (see docs/boards.md), not an oversight —
 * exposing this beyond the home network would need real authentication and
 * TLS first. Reachable at http://<hub's local IP>/ once the hub has joined
 * a network (see `status` on the console, or the boot log, for the IP).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts the HTTP server and registers the dashboard's routes. Safe to
 *  call before the hub has joined a network — the server just won't be
 *  reachable until an IP is assigned. Call once, after the subsystems it
 *  reads from (motion_init(), presence_init(), devices_init(), etc.) are
 *  already up. */
esp_err_t web_status_init(void);

/** Diagnostic: has the hub make an HTTP GET to its own dashboard, over
 *  its actual STA IP (not loopback — ESP-IDF's default lwIP config
 *  doesn't build in a loopback netif, so 127.0.0.1 isn't a meaningful
 *  test here). Isolates "is the server itself broken" from "something
 *  on the network/router is blocking other devices from reaching it" —
 *  see the `web selftest` console command. */
esp_err_t web_status_selftest(void);

#ifdef __cplusplus
}
#endif
