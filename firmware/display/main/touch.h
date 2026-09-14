/**
 * FT6146 capacitive touch controller on DISP-1's I2C bus.
 *
 * I2C pins and the touch device address are confirmed against Waveshare's
 * own working example (see bsp_amoled.h's header comment for the same
 * caveat — don't "clean up" register-level code here without hardware to
 * test against). Touch RST/INT are not wired on this board (polled only).
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up I2C and the touch controller, then register an LVGL pointer
 *  input device backed by it. Call after bsp_amoled_init() (needs LVGL
 *  initialized) — order relative to bsp_amoled_init() otherwise doesn't
 *  matter, this doesn't touch the display. */
esp_err_t touch_init(void);

#ifdef __cplusplus
}
#endif
