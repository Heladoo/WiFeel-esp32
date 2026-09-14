# vendor/

Unmodified example code from the Waveshare ESP32-C6-Touch-AMOLED-1.43's own
repository, kept here for two reasons:

1. **`waveshare_10_lvgl_v9_test/`** — flashed as-is for the *first* step of
   milestone 3 (display bring-up), to confirm the panel, touch, and
   toolchain all work before any WiFeel code runs on the board. See
   `CLAUDE.md` / the plan for the milestone checklist.
2. **`waveshare_reference/`** — just `touch_bsp.c`/`.h`, kept as a fallback
   reference in case `esp_lcd_touch_ft5x06` (the managed component WiFeel's
   own display firmware uses) doesn't handle the FT6146 touch controller
   correctly and needs porting.

Source: <https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.43>,
`Example/ESP-IDF/10_LVGL_V9_Test` (fetched 2026-09-14). No `LICENSE` file
was present in that repository at fetch time — this is vendor-provided
example code for use with their own hardware, not WiFeel's code, and isn't
covered by this project's MIT license (see the root `LICENSE`).

Neither folder is referenced by WiFeel's own `firmware/` build — they're
standalone ESP-IDF projects you build and flash independently
(`idf.py -C vendor/waveshare_10_lvgl_v9_test build flash`), used only to
sanity-check hardware.
