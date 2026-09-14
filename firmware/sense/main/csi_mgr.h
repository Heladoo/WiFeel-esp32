#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "wifeel_csi.h"
#include "wifeel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers the esp_wifi CSI callback and creates a wifeel_csi_stream_t
 *  for every stream this board tracks locally. On the hub that's S1
 *  (router->hub) today; S3 (display->hub sounding) is added in milestone
 *  5 once the ESP-NOW link exists. Call after wifi_mgr_init(). */
esp_err_t csi_mgr_init(void);

/** Point `id` at a new source MAC (e.g. S1 at the router's BSSID right
 *  after wifi_mgr_join() succeeds). Frames from any other MAC are ignored
 *  by that stream. Pass an all-zero MAC to stop tracking a source. */
esp_err_t csi_mgr_set_source_mac(wifeel_stream_id_t id, const uint8_t mac[6]);

/** Enable/disable CSI capture on the radio. Safe to call before any
 *  streams are registered (frames just won't match anything yet). */
esp_err_t csi_mgr_set_enabled(bool enabled);
bool      csi_mgr_is_enabled(void);

/** Borrow the underlying stream for feature/pkt-rate reads (console.c,
 *  and later motion.c/presence.c). Returns NULL for an id this board
 *  doesn't track locally (e.g. S2/S4 live only on the display). */
wifeel_csi_stream_t *csi_mgr_get_stream(wifeel_stream_id_t id);

#ifdef __cplusplus
}
#endif
