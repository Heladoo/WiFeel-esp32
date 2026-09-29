---
name: esp32c6-amoled-ui
description: Use when starting or working on an ESP32-C6 project on the Waveshare ESP32-C6-Touch-AMOLED-1.43 (466x466 round AMOLED, SH8601 QSPI, FT6146 touch, LVGL 9, no PSRAM) — display/touch bring-up, LVGL UI layout for a round screen, optional Wi-Fi STA + tiny web page, ESP-IDF v5.5 build/flash, non-interactive serial testing. Distilled from the WiFeel project (which used the same board for a Wi-Fi CSI display).
---

# ESP32-C6 + AMOLED 1.43" — UI (and optional web) project kit

Everything here was verified on real hardware in the WiFeel project, except where marked
**untested**. `templates/` is a ready project skeleton; copy it, don't re-derive it.

## Quick start (new project)

1. `cp -r <skill>/templates <new-project>` (contains `main/`, `tools/`, `partitions.csv`,
   `sdkconfig.defaults`, top-level `CMakeLists.txt`; rename `project(amoled_app)`).
2. `idf.py set-target esp32c6 && idf.py build` (ESP-IDF **v5.5.5**; managed components
   `esp_lcd_sh8601` and `lvgl/lvgl ^9` download on first build, so the machine needs internet).
3. Erase on first flash of a board that ran other firmware: `idf.py -p COMx erase-flash`, then
   `idf.py -p COMx -b 921600 flash`.
4. Verify with `python tools/serial_log.py COMx --seconds 20` — expect the boot banner
   (version, target, free heap) and no panics. Then have a human look at the screen.

> The skeleton was assembled from working WiFeel code but **has not been compiled as a
> standalone project**. The `bsp_amoled.*`/`touch.*` files are verbatim from hardware-verified code
> (only renamed `WIFEEL_LCD_*` → `AMOLED_LCD_*`). `app_main.c`, `wifi_sta.c`, `web_server.c` are new:
> expect to fix a typo on first build.
> The pristine Waveshare reference is in the WiFeel repo at `vendor/waveshare_10_lvgl_v9_test/`.

## Hardware facts

| Item | Value |
|---|---|
| Board | Waveshare ESP32-C6-Touch-AMOLED-1.43, ESP32-C6 (QFN40), 16 MB flash, **no PSRAM** |
| Panel | 466×466 round AMOLED, SH8601-compatible (CO5300), QSPI @ 40 MHz, RGB565, driver `esp_lcd_sh8601` |
| LCD pins | CS 10, PCLK 11, D0–D3 = 4,5,6,7, RST 3 (SPI2_HOST) |
| Touch | FT6146 on I2C0, SCL 8, SDA 18, addr 0x38, 300 kHz; RST/INT **not wired** → polled |
| USB | Native USB-Serial/JTAG (no UART bridge chip) → console on `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |
| Source of truth | Waveshare `Example/ESP-IDF/10_LVGL_V9_Test` (github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.43) |

Do **not** "clean up" the panel init command table (`0x11,0xC4,0x53,0x63,0x51,0x29,0x51`), the
`set_gap(0x06,0)`, or the touch register code without a board to test — they are opaque vendor writes.

## Display/LVGL rules learned the hard way

- **LVGL is not thread-safe.** Every LVGL call outside the LVGL port task must sit inside
  `bsp_amoled_lvgl_lock(timeout)`/`unlock()`. In periodic UI tasks use a finite timeout (100 ms)
  and `continue` on failure rather than blocking forever. Use `-1` only for one-time boot setup.
- **Memory (no PSRAM, ~200 KB free heap):** draw buffers are 2 × (466×50×2 B) partial-render DMA
  buffers — the size Waveshare confirmed. Don't enable large fonts; enable only the
  `CONFIG_LV_FONT_MONTSERRAT_*` sizes used (14/20/28/48 in the template). Print free heap in the
  boot banner and watch it. Full-frame buffers won't fit.
- **SH8601 needs even-aligned flush areas** — handled by the `LV_EVENT_INVALIDATE_AREA` rounder in
  `bsp_amoled.c`; keep it. The flush callback also byte-swaps RGB565 (`lv_draw_sw_rgb565_swap`).
- **Brightness:** `bsp_amoled_set_brightness(0-255)` (SH8601 cmd 0x51). Power/burn-in: AMOLED shows
  true black as off, so dark backgrounds save power; dim or blank after idle for static UIs.
- **Set dark background on screen, tileview AND each tile**, otherwise LVGL's light default theme
  flashes through during swipes ("background changed to white" was a real bug).
- `lv_tileview` with `LV_DIR_HOR` tiles gives swipe navigation for free (touch already wired to LVGL).
- Touch: only single-point reads are implemented; coordinates clamp to 466.
- Call `lv_display_add_event_cb` rounder + `lv_init()` inside `bsp_amoled_init()`; create widgets
  afterwards, from a task, under the lock.

## Designing for a round 466 px screen

- Visible chord width at vertical offset `dy` from center = `2*sqrt(233² − dy²)`. Anything near the
  top/bottom must be narrow. Content at |dy|≈185 fits roughly ±140 px; a 340 px-wide chart fits at
  center+55.
- Position with `lv_obj_align(CENTER, x, y)` for the anchor widget, then cascade siblings with
  `lv_obj_align_to(..., LV_ALIGN_OUT_BOTTOM_MID, ...)`. If an anchor fits the bezel, its children
  won't clip. Put chart axis labels **inside** the chart's rectangle (corner-anchored), not outside.
- Long text: `lv_label_set_long_mode(LV_LABEL_LONG_DOT)` + fixed width (SSIDs are up to 32 chars).
- Palette that tested well on the dark bg (0x101418): text 0xE8EEF2, muted 0x8FA3AD (~7:1; 0x5A6B73
  ≈3.3:1 and 0x3A4750 ≈1.9:1 were too dim for text), blue 0x4FA8E8, amber 0xE8A33D, green 0x3DAA6E,
  teal 0x3ABEBE. **One color = one meaning** across the UI (a UX audit caught amber meaning three things).
- Only LVGL's built-in `LV_SYMBOL_*` glyphs exist with Montserrat (no phone/PC/thermometer icons).
  `LV_SYMBOL_REFRESH` reads as "reload", not motion. For richer icons, generate a custom icon font.
- Show honest states: `--` + dimmed (`lv_obj_set_style_opa(LV_OPA_40)`) when data is missing/stale,
  spelled-out "Unknown" instead of "?", zones ("<1m") instead of falsely precise numbers.
- `lv_chart` (line): `lv_chart_set_update_mode(SHIFT)`, `lv_chart_set_next_value()` per tick,
  `lv_chart_set_div_line_count(3,3)` for a grid, `lv_obj_set_style_size(chart,0,0,LV_PART_INDICATOR)`
  hides point dots; a threshold line = `lv_line` sized to the chart, dashed via
  `line_dash_width/gap`, y = `H − H*value/100`.
- No simulator/camera in the WiFeel workflow: layouts were computed by hand and only verified by a
  human looking at the device. Ask the user to eyeball new screens; don't claim visual confirmation.

## Optional Wi-Fi + web access (no CSI)

`templates/main/wifi_sta.[ch]` + `web_server.[ch]` (**untested**; based on WiFeel's hub code). Enable by
adding the two files to `SRCS` and `esp_wifi esp_netif esp_event esp_http_server` to `REQUIRES`, then in
`app_main` after NVS init: `wifi_sta_start(); web_server_start();`.

- Credentials live in NVS (`wifi_sta_save_credentials`), never in source. Provision them by a
  console command typed by the person, not via automated tooling (`tools/send_cmd.py` refuses
  `join`/`passive` commands for that reason; adapt its deny list to your command names).
- **httpd task priority:** the default is `tskIDLE_PRIORITY+5`; on WiFeel that starved networking and
  dropped links. Set `cfg.task_priority = tskIDLE_PRIORITY + 1` and keep UI/LVGL task priority above it.
- Prove the server independently of the network with an on-device self-test: `esp_http_client` GET of
  your own STA IP (not 127.0.0.1 — default lwIP has no loopback netif). WiFeel's dashboard returned
  HTTP 200 on-device yet was unreachable from every other device: the cause was the **router**
  (AP/client isolation, guest networks). Check that before debugging firmware.
- Serve one self-contained HTML page (no external resources) + `/api/status` JSON polled every 1 s.
  Heap-allocate large JSON bodies (`malloc`, ~6 KB) rather than stack buffers; escape SSIDs.
- Wi-Fi is easy to make flaky: don't rely on `WIFI_EVENT_STA_DISCONNECTED` alone to detect a dead
  link (observed not firing). If reliability matters, add a watchdog that force-reconnects after a
  stall — but only after a first success, and never on a network that doesn't answer ICMP.
- Local-network only, no auth/TLS: fine for a LAN status page; add auth before exposing anything writable.
- mDNS (`esp_mdns`) would give `http://name.local/` instead of an IP — not tried yet.

## Toolchain & flashing (Windows dev machine used by the WiFeel user)

- ESP-IDF v5.5.5 via Espressif EIM CLI at `C:\Espressif`. Non-interactive:
  `& $eim run "idf.py -C <dir> build" v5.5.5` — always pass `v5.5.5` (a v6.0.2 also exists on that
  machine); interactive: `eim shell v5.5.5`.
- Confirm which board is on which COM port before flashing (ports renumber). Auto-reset via RTS worked
  for both boards; if flashing times out: hold BOOT, tap RST, release BOOT, retry immediately.
- Also see the `esp32-firmware-flashing` skill for generic ESP32 flashing steps.
- `idf.py monitor` needs a TTY — use `tools/serial_log.py COMx --seconds 20` instead.

## Serial pitfalls (important)

- On native USB-Serial/JTAG, DTR/RTS map to reset/boot. A plain `serial.Serial(port)` open/close
  **resets the board**. Always open through `tools/serial_util.py: open_port()` (sets DTR/RTS low
  *before* `open()`).
- `run_in_background` Bash calls on that machine spawned duplicate Python processes — never use it
  for serial access.
- `tools/send_cmd.py COMx <cmd>` sends one line to an `esp_console` REPL and prints the reply; if the
  new project needs an interactive console, add `esp_console` (USB-Serial/JTAG REPL) and verify
  over it, since there's no test suite — verification is on-device.

## Conventions worth keeping

- Boot banner with version string, target, free heap; bump the version on every behavior change.
- Keep a `docs/boards.md`: which physical unit has which MAC/COM port/firmware.
- Keep hardware-verified vendor code annotated as such, and log live findings (including refuted
  theories) — several WiFeel "root causes" were later corrected by the log itself.

## What NOT to carry over

Anything CSI/ESP-NOW/BLE-scan/SoftAP related (`wifeel_csi`, `wifeel_proto`, hub link, phones tile).
Those need Wi-Fi in promiscuous/AP+STA modes and BLE coexistence, and the display firmware's
`sdkconfig.defaults` for them differ. This kit deliberately omits them.
