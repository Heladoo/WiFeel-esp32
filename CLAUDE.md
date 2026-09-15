# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

See also the approved project plan at the path recorded in `docs/boards.md`,
and the `esp32-firmware-flashing` skill for generic ESP32 flashing help.

## What this project is

WiFeel: Wi-Fi CSI sensing (motion, presence, people count, breathing rate,
rough position) across two ESP32-C6 boards in one room. Full design rationale,
architecture, and milestone checklist live in the plan file this project was
built from — check `docs/boards.md` for its path if it's not obvious, and
otherwise ask the user rather than re-deriving the architecture from scratch.

## Architecture

- **Two independent firmware images**, not one shared build: `firmware/sense`
  (the hub — does all CSI sensing and fusion) and `firmware/display` (LVGL UI,
  plus a second CSI vantage point). They only share code via `components/` —
  never a shared `main`, and `idf.py` is invoked separately for each with `-C`.
- **`components/`** is the only place logic is shared between the two images:
  - `wifeel_proto`: the ESP-NOW wire format. One `wifeel_msg_type_t` enum, one
    tagged-union `wifeel_msg_payload_t`, `wifeel_proto_pack()`/`_unpack()` with
    CRC-16 framing. Adding a message type = add the enum value + payload
    struct + a union member; the union's size (and `WIFEEL_PROTO_MAX_LEN`)
    follows the largest member automatically.
  - `wifeel_csi`: the CSI ring buffer and feature extraction
    (`wifeel_csi_stream_t`) — identical code path on both firmwares for
    whichever stream(s) that board owns.
- **CSI streams**: the plan's original design has 4 (S1-S4); only two exist
  today. **S1** = router→hub, via the hub's own STA join (`wifi_mgr.c`).
  **S3** = display→hub, via the hub's SoftAP that the display joins as a
  station and pings continuously (see "Hub↔display link" below — active
  traffic, not just association, is what makes CSI work here).
- **Hub fusion pipeline** (`firmware/sense/main/`): `csi_mgr.c` feeds raw CSI
  into `wifeel_csi` per stream → `motion.c` (P1) and `presence.c` (P2) each
  read features from both streams and fuse independently → `devices.c` holds
  a separate table of nearby BLE/Wi-Fi devices, fed by `ble_scan.c` (passive
  NimBLE scan) and `wifi_sniff.c` (a second promiscuous Wi-Fi RX callback
  alongside CSI's own) → `link.c` packages all of it into
  `WIFEEL_MSG_STATE` (~3Hz) and `WIFEEL_MSG_DEVICES` (~1Hz) and broadcasts
  both over ESP-NOW.
- **Display side** (`firmware/display/main/`): `link.c` receives and caches
  the latest STATE/DEVICES message (mutex-protected, most-recent-wins);
  `app_main.c`'s `ui_update_task` polls both on a fixed interval and updates
  an `lv_tileview` with two tiles — Home (`build_home_screen()`) and Phones
  (`ui_phones.c`).
- **Reliability watchdogs**: `ping_gw.c` on both boards normally relies on
  `WIFI_EVENT_STA_DISCONNECTED` to detect a dead link, which has been
  observed live to not fire even when the link is truly dead. Both
  `firmware/sense/main/app_main.c` (S1) and `firmware/display/main/link.c`
  (S3) run a small watchdog task that force-reconnects after a stall — but
  only once a first successful reply has been seen, since a network that
  never answers ICMP at all must not be mistaken for "stalled" (see
  docs/boards.md's "Correction: the ~8s reconnect cycle was our own
  watchdog" section for why this distinction exists).

## Current status (see docs/boards.md for full detail)

- Both boards identified, flashed, and running custom firmware (HUB-1 =
  XIAO C6, DISP-1 = Waveshare AMOLED).
- P1 (motion, fusing S1+S3 in `motion.c`) is live-tested and working;
  P3-P5 (people count, breathing, rough position) are not built yet.
- Display's Home tile shows three top stats (nearby phones, presence,
  motion) plus a gridded 2-series trend chart of S1 vs S3 scores with
  live current-value legends.
- P2 (presence) code is written (`presence.c`/`.h`) and verified correct
  in a clean test, but its wander thresholds are still unvalidated
  placeholders.
- **Opening/closing the serial port resets HUB-1** unless DTR and RTS
  are held low before open. All tools now open ports through
  `tools/serial_util.py` `open_port()`, which does that — use it in any
  new tool. (An earlier "DTR/RTS refuted" conclusion was wrong; see
  docs/boards.md.) Also: `run_in_background` Bash calls on this machine
  launch duplicate Python processes — never use it for serial.
- The watchdogs described under Architecture are fixes for a real incident:
  a stuck S1 ping once starved the hub enough to fail the SoftAP's WPA2
  handshake with the display (802.11 reason code 15) — breaking S3 too
  without the hub ever crashing. Side effect on the current test network:
  it doesn't answer ICMP at all, so S1 CSI is only ~2-5 pkt/s (AP frames
  only, no ping replies).
- **Nearby phone detection built end to end**: BLE scanning + Wi-Fi
  client sniffing on the hub (`devices.c`, `ble_scan.c`, `wifi_sniff.c`),
  broadcast to the display (`WIFEEL_MSG_DEVICES`, ~1Hz), rendered on a
  swipeable Phones tile (`ui_phones.c`) — smartwatch-vitals style: a
  plain phones-nearby headline number (no gauge), a Wi-Fi-connected
  count, and a nearest-first device table (type icon, vendor, range
  zone). Entries are ranked by computed distance, not raw RSSI (BLE and
  Wi-Fi have different 1m references). Verified live end to end. Vendor
  classifier (Apple/Samsung/Google/Microsoft) checked against real
  nearby devices with sourced Bluetooth SIG data and live raw captures
  — see docs/boards.md. `phones calib ble|wifi <s> [vendor]` restricts
  calibration to one vendor (needed live — an unfiltered calibration on
  a desk with several BLE sources locked onto the wrong device once);
  `phones calib reset ble|wifi` undoes a bad calibration. Not yet
  visually confirmed on the physical round screen (no camera/simulator
  available).
- **Web status dashboard**: the hub serves a read-only status page
  (`web_status.c`, `esp_http_server`) at `http://<hub IP>/` — same live
  data as the display's tiles, reachable from any device on the hub's
  own Wi-Fi (local-network-only by design, no auth/TLS). Its httpd task
  runs at `tskIDLE_PRIORITY+1`, not the component's own default
  (`+5`) — the default was verified live to starve `ping_gw` and
  ESP-NOW badly enough to drop the display's link; keep it low if this
  file is touched again. Reachability from an actual phone/PC on the
  hub's network isn't confirmed yet — see docs/boards.md.

## Hub↔display link (the "second sensing node")

The hub runs `WIFI_MODE_APSTA`: a station connection to the user's real
router (for S1), *and* its own SoftAP simultaneously (SSID/password:
`WIFEEL_LINK_AP_SSID`/`WIFEEL_LINK_AP_PASSWORD` in `wifeel_proto.h` — a
fixed local credential, NOT the user's home network, safe to hardcode).
The display joins that SoftAP as a plain station and pings it at 100Hz
(`ping_gw.c`, copied from `firmware/sense`), exactly like S1's
gateway-ping design — this is what makes S3's CSI capture work (real
data-frame traffic), unlike the raw ESP-NOW broadcasts tried earlier,
which never produce CSI on this hardware (see docs/boards.md's
DIRECT-mode finding).

ESP-NOW itself (broadcast, unencrypted) rides on top of this same
association for the STATE message the display renders — that part works
reliably; it's the underlying Wi-Fi *association* that's been dropping.

## Toolchain

- **ESP-IDF v5.5.5**, installed via Espressif's EIM CLI at `C:\Espressif` (confirmed working — see `docs/boards.md`).
- This machine's `eim` alias isn't on PATH in a non-interactive shell yet
  (needs a fresh terminal after its winget install); use the full path or
  run one command at a time via `eim run "<command>" v5.5.5`:
  ```powershell
  $eim = "C:\Users\Lenovo\AppData\Local\Microsoft\WinGet\Packages\Espressif.EIM-CLI_Microsoft.Winget.Source_8wekyb3d8bbwe\eim.exe"
  & $eim run "idf.py -C firmware/sense build" v5.5.5
  ```
  In an interactive shell where `eim` is on PATH, `eim shell v5.5.5` opens
  an already-activated shell instead (more convenient for a multi-command
  session).
- **Note:** this machine also has an unrelated ESP-IDF v6.0.2 at
  `C:\esp\v6.0.2` (pre-existing, not installed by this project) — always
  pass `v5.5.5` explicitly to `eim run`/`eim shell` rather than relying on
  whatever `eim select` last left active, to avoid silently building
  against the wrong IDF version.
- Two independent ESP-IDF projects, not a single multi-target build:
  `firmware/sense` (target `esp32c6`, the XIAO hub) and `firmware/display`
  (target `esp32c6`, the Waveshare AMOLED).

## Build / flash / monitor

```bash
idf.py -C firmware/sense set-target esp32c6      # once, after a clean checkout
idf.py -C firmware/sense build
idf.py -C firmware/sense -p COMx -b 921600 flash

idf.py -C firmware/display set-target esp32c6
idf.py -C firmware/display build
idf.py -C firmware/display -p COMy -b 921600 flash
```

`idf.py monitor` needs an interactive TTY this environment doesn't have — use
`tools/serial_log.py COMx --seconds 20` instead to capture boot output
non-interactively, per the esp32-firmware-flashing skill's verification step.

Before the **first** WiFeel flash on any board (or after it ran different
firmware), erase flash first: `idf.py -C firmware/sense -p COMx erase-flash`.

## Verification

There's no build-time test suite — verification is on-device, over the
hub's interactive `esp_console` REPL (`firmware/sense/main/console_cmds.c`):

```
status                  # link, CSI packet rates/RSSI, motion+presence state, free heap
phones                  # nearby-device summary table (BLE + Wi-Fi)
phones selftest         # runs the BLE/Wi-Fi classifiers against known-good captured
                         # byte samples on-device — closest thing to a regression test
motion [seconds]        # streams live motion score/jitter, for walk-by tuning
```

Drive the console non-interactively from `tools/` (only `pyserial` is
actually required; `requirements.txt` also lists packages for training
tooling from the original plan that was never built):

```bash
python tools/send_cmd.py COMx status          # one command, capture the reply
python tools/serial_log.py COMx --seconds 20  # boot log / free-running capture
python tools/status_poll.py COMx --minutes 5  # S1/S3 pkt-rate + heap over time, one connection
```

Never open the hub's serial port with a plain terminal or a fresh
`serial.Serial(...)` call — see "Opening/closing the serial port..." below.
Always go through `tools/serial_util.py`'s `open_port()` (all scripts above
already do).

## Board identification

Always confirm which physical board is on which COM port before flashing —
see `docs/boards.md` for the running log (chip-id, MAC, flash size, which
firmware belongs on it). Both boards use native USB-CDC (no bridge chip), so:

- They enumerate as `COMx` but auto-reset can be unreliable — if `idf.py
  flash` times out waiting to connect, do a manual bootloader entry: hold
  BOOT, tap RST, release BOOT, then retry immediately.
- Ports can renumber when boards are unplugged/replugged or a hub is used —
  re-check `Win32_PnPEntity` (or Device Manager) rather than assuming the
  last-used COM port is still correct.

## Hardware pin reference

**Waveshare ESP32-C6-Touch-AMOLED-1.43** (from Waveshare's own
`Example/ESP-IDF/10_LVGL_V9_Test/main/user_config.h` — do not re-derive these
from the datasheet, they're confirmed working):

| Signal | GPIO |
|---|---|
| LCD CS | 10 |
| LCD PCLK | 11 |
| LCD D0–D3 | 4, 5, 6, 7 |
| LCD RST | 3 |
| I2C SCL | 8 |
| I2C SDA | 18 |
| Touch (FT6146) I2C addr | 0x38 |
| Touch RST / INT | not wired (-1) |

Panel driver component: `esp_lcd_sh8601` (the CO5300 is SH8601-compatible).
466×466, RGB565, QSPI @ 40 MHz. **No PSRAM** — keep LVGL draw buffers small
(see the plan's UI memory section) and watch free heap in every boot log.

**Seeed XIAO ESP32-C6** external antenna switch — must be set before Wi-Fi
init or CSI quality suffers:
```c
// GPIO3 LOW = enable RF switch control, GPIO14 HIGH = external antenna
gpio_set_level(GPIO_NUM_3, 0);
gpio_set_level(GPIO_NUM_14, 1);
```

## Conventions

- Raw CSI data never crosses the ESP-NOW link — only extracted features and
  results. Keep it that way; it's both a bandwidth and a privacy boundary.
- Every firmware prints a boot banner with a version string, target, and (hub
  only) antenna mode — required for the flashing skill's post-flash
  verification checklist. Bump the version string on every firmware change
  that changes behavior.
- `docs/boards.md` is the source of truth for which physical unit is which —
  keep it updated whenever a board is re-flashed, relabeled, or replaced.
