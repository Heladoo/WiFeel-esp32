# Board log

Plan this project was built from:
`C:\Users\Lenovo\.claude\plans\i-want-to-create-enumerated-thimble.md` (approved 2026-09-14).

## Toolchain

- ESP-IDF version: v5.5.5 (installed via `eim install -t esp32c6 -i v5.5.5`, base path `C:\Espressif`) — confirmed installed 2026-09-14.
- `IDF_PATH`: `C:\Espressif\v5.5.5\esp-idf`
- `IDF_TOOLS_PATH`: `C:\Espressif\tools`
- `IDF_PYTHON_ENV_PATH`: `C:\Espressif\tools\python\v5.5.5\venv`
- Activate with: `eim shell -i v5.5.5` (opens a pre-activated shell), or
  source the generated PowerShell activation script directly (EIM placed a
  desktop shortcut for it too).

## Physical units

| Label | Role | Board | Chip ID / MAC | Flash | COM port (as of last flash) | Firmware version running |
|---|---|---|---|---|---|---|
| HUB-1 | Sensor hub | Seeed XIAO ESP32-C6 (ext. antenna) | ESP32-C6FH4 (QFN32) rev v0.2, MAC `58:e6:c5:10:76:d0` | 4MB embedded | COM9 (native USB-Serial/JTAG, VID_303A&PID_1001) | _not yet flashed_ |
| DISP-1 | Display + 2nd sensor | Waveshare ESP32-C6-Touch-AMOLED-1.43 | ESP32-C6 (QFN40) rev v0.2, MAC `fc:01:2c:fe:0f:f8` | 16MB external | COM8 (native USB-Serial/JTAG, VID_303A&PID_1001) | _not yet flashed_ |

Identified 2026-09-14 via `python -m esptool --port COMx chip_id` /
`flash_id` — auto-reset via RTS works cleanly on both units, no manual
BOOT/RESET needed for flashing. Physically label each board with its row
name (HUB-1 / DISP-1) once identified — there's only one of each right now,
but this stops future mixups if a second XIAO or Waveshare unit is added.

## Room layout / calibration notes

_Not yet calibrated._ Fill in after milestone 7 (Presence):

- Router location relative to HUB-1 and DISP-1
- Date of last empty-room calibration
- Date of last people-count / position training session, and its confusion
  matrix (see `docs/` for saved `train_models.py` output)

## Firmware build status

- `firmware/sense` (hub, target esp32c6): builds cleanly against ESP-IDF
  v5.5.5 as of 2026-09-14 — `idf.py build` succeeds with 0 warnings/errors
  from WiFeel code, `wifeel_sense.bin` is ~880KB (71% of the 3MB factory
  partition free). Covers milestone 4's code (board bring-up, Wi-Fi join,
  CSI capture on S1, gateway ping, `join`/`status` console commands) — not
  yet flashed to real hardware.
- `vendor/waveshare_10_lvgl_v9_test/` (milestone 3, step 1): flashed to
  DISP-1 2026-09-14, builds clean, boots clean (SH8601 panel init success,
  LVGL init success, no crashes), **user-confirmed on real hardware: image
  displays and touch responds.** Panel/touch/toolchain all verified working
  before any WiFeel code runs on this board.
- `firmware/display` (milestone 3, step 2 — DONE): boots clean on DISP-1,
  own SH8601+LVGL bring-up (`bsp_amoled.c`/`touch.c`, adapted from the
  vendor reference above) — **user-confirmed: "WiFeel" title, firmware
  version, and free heap render correctly on real hardware.** No ESP-NOW
  link yet (milestone 5).

## CSI capture findings (milestone 4)

**Root cause found and fixed 2026-09-14**: JOIN mode initially showed ~0
CSI pkt/s from the router regardless of RSSI (-80 to -50 dBm tested) or
AMPDU aggregation (on/off tested) — this looked like a fundamental
"own-BSS traffic bypasses the promiscuous CSI hook" driver limitation and
very nearly caused a premature pivot away from JOIN mode. It wasn't that —
the real cause was `.acquire_csi_legacy = false` in `csi_mgr.c`'s
`wifi_csi_config_t` — copied verbatim from Espressif's own esp-csi
`get-started/csi_recv` reference example, which is wrong for a STA
actually associated to a real AP (that example never associates to
anything). Setting `.acquire_csi_legacy = true` alongside the existing
HE/HT flags raised the *overall* ambient capture rate roughly 10x and, for
the first time in any test, produced CSI repeatedly and reliably from our
own AP's exact BSSID.

**Confirmed steady-state rate for S1 (router→hub), JOIN mode, 100Hz
gateway ping**: **~3.4 pkt/s** — far below the plan's original "≥80 pkt/s"
target (which assumed close to 1:1 capture of the ping traffic), but a
completely different regime from "never." At ~3.4 Hz:
- Motion (P1) and presence (P2) — no concern, plenty of resolution.
- People count (P3) — likely fine, may want slightly longer feature
  windows than originally planned.
- Breathing (P4) — 3.4 Hz still clears Nyquist for the 0.1-0.6 Hz band in
  principle, but `wifeel_csi`'s current 20 Hz grid-bucketing design
  (`WIFEEL_CSI_GRID_HZ`) assumes far denser sampling than this actually
  delivers — will need rearchitecting around irregular ~3-4 Hz sampling
  when P4 is built, not a blocker today.
- Rough position (P5) — untested, likely needs re-evaluation once
  multiple streams (S1-S4) are combined in milestone 5+.

Also fixed along the way: `esp_wifi_set_promiscuous(true)` was missing
entirely (CSI never fires at all without it, even before the legacy-flag
issue); a console log-spam bug (a 2s periodic status ticker made the REPL
unusable — removed, `status` is on-demand only now); and a
`wifi_mgr`-level gap where CSI/ping setup only ever ran from the `join`
console command, never on ESP-IDF's own automatic NVS-based reconnect on
boot (fixed via a `wifi_mgr_set_connected_cb` hook).

A temporary `sniff <bssid> <channel>`, `track <mac>`, and `espnow_rx`
console command exist in `console_cmds.c` for hardware diagnostics (track
CSI for a MAC without associating / without touching Wi-Fi state / plain
ESP-NOW reception with no CSI). Remove once no longer needed for tuning.

## DIRECT-mode finding: ESP-NOW broadcasts don't produce CSI (use a SoftAP link instead)

Tested 2026-09-14 with the display (DISP-1) broadcasting ESP-NOW frames at
50Hz and the hub tracking DISP-1's exact MAC via `sniff`/`track`:

- On a channel with plenty of other ambient traffic (channel 9, ~37 CSI
  frames/sec total from neighboring devices), **the hub's CSI capture for
  DISP-1's broadcasts specifically was 0.0 pkt/s** over multiple runs
  (hundreds of broadcast opportunities each), while other devices on the
  same channel kept producing hits throughout.
- Ruled out as a reception/range problem directly: a plain ESP-NOW
  receiver (`espnow_rx` console command, no CSI involved) on the *same*
  channel/config received DISP-1's broadcasts perfectly — full 50Hz rate,
  ~20ms spacing, clean RSSI (-58 to -60 dBm). The frames unambiguously
  reach the hub.
- **Conclusion**: ESP-NOW frames are 802.11 management-type Action frames,
  not data frames — CSI extraction on this hardware appears tied to the
  data-frame RX path and doesn't tap Action frames at all, regardless of
  signal quality or acquire-config flags.

**Implication for the plan's DIRECT mode ("Direct link, no router")**: raw
ESP-NOW sounding frames between the hub and display won't produce CSI.
Build the direct link as a **SoftAP** on one board with the other
associating as a normal STA instead (reusing the exact same proven data-
frame CSI pipeline JOIN mode already uses, ~3.4 pkt/s). ESP-NOW remains a
good fit for the *control* channel (pairing/discovery, SET_SOURCE,
FEATURES/STATE messages per wifeel_proto.h) — just not for CSI sounding
itself. This changes milestone 5's design; update the plan when that
milestone starts.

## P1 Motion — confirmed working live (2026-09-14)

`motion.c` (fast per-sample jitter EMA from `wifeel_csi_stream_get_fast_jitter`,
not the slower ring-buffer window — see the CSI findings above on why: real
CSI arrives too sparsely for the ring window to react quickly) + simple
hysteresis (enter score ≥40, exit <20, `MOTION_SCORE_MAX_JITTER`=5.0,
all first-pass guesses, not tuned beyond this one test).

**Live-tested by walking past HUB-1 twice** over a ~25s `motion` console
capture: baseline score ~13 (jitter ~0.7) at rest, first walk-by →
score climbed to 54 → `MOTION started` → peaked at 76 → decayed → score 18
→ `MOTION cleared`; second walk-by repeated the same clean pattern
(→42 → `MOTION started` → 48). No chattering, sensible hysteresis
behavior, both events cleanly detected on the very first threshold guess.

Not yet tuned against: no-motion false-positive rate over a long idle
period, sensitivity to a *still* person (vs. actively moving) which is
presence's (P2) job not motion's, or multiple simultaneous nearby Wi-Fi
devices' own traffic adding jitter noise.

## Known per-unit quirks

_None yet — add here as they're discovered (e.g. "HUB-1 needs
`esp_wifi_set_max_tx_power` lowered, connection drops otherwise")._
