/**
 * Board support for the Waveshare ESP32-C6-Touch-AMOLED-1.43's display:
 * SH8601 panel over QSPI + LVGL 9 port glue.
 *
 * Pin assignments and the panel init command sequence are copied from
 * Waveshare's own confirmed-working example
 * (vendor/waveshare_10_lvgl_v9_test/main/{user_config.h,main.c}) — verified
 * booting and rendering on real DISP-1 hardware before this file existed
 * (see docs/boards.md). Don't "clean up" the init sequence without a real
 * board to test against; QSPI panel bring-up is exactly the kind of code
 * that looks equivalent but isn't.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFEEL_LCD_H_RES 466
#define WIFEEL_LCD_V_RES 466

/**
 * Bring up the SPI bus, SH8601 panel, and LVGL display (double-buffered,
 * partial render mode, DMA-capable buffers — no PSRAM on this board, see
 * CLAUDE.md). Starts the LVGL tick timer and port task; does NOT touch
 * touch/I2C (see touch.h) and does not draw anything — the caller creates
 * its own LVGL screen afterward.
 *
 * Must be called from a task, not from app_main() before the scheduler is
 * relevant — it creates a mutex, a task, and an esp_timer.
 */
esp_err_t bsp_amoled_init(void);

/**
 * LVGL is not thread-safe: any LVGL API call from outside the LVGL port
 * task (created by bsp_amoled_init) must be wrapped in
 * bsp_amoled_lvgl_lock()/bsp_amoled_lvgl_unlock(). Pass -1 for
 * timeout_ms to wait forever (the common case — screen setup at boot).
 * Returns false on timeout.
 */
bool bsp_amoled_lvgl_lock(int timeout_ms);
void bsp_amoled_lvgl_unlock(void);

/** 0-255. Backed by SH8601 command 0x51 (write_display_brightness). */
void bsp_amoled_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
