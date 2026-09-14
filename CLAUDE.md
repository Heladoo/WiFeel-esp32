# CLAUDE.md

Guidance for Claude Code sessions working in this repo. See also the approved
project plan at the path recorded in `docs/boards.md`, and the
`esp32-firmware-flashing` skill for generic ESP32 flashing help.

## What this project is

WiFeel: Wi-Fi CSI sensing (motion, presence, people count, breathing rate,
rough position) across two ESP32-C6 boards in one room. Full design rationale,
architecture, and milestone checklist live in the plan file this project was
built from — check `docs/boards.md` for its path if it's not obvious, and
otherwise ask the user rather than re-deriving the architecture from scratch.

## Current status (see docs/boards.md for full detail)

- Both boards identified, flashed, and running custom firmware (HUB-1 =
  XIAO C6, DISP-1 = Waveshare AMOLED).
- Hub senses two independent CSI streams: **S1** (router→hub, via
  `wifi_mgr_join()`) and **S3** (display→hub, via the hub's own SoftAP —
  see "Hub↔display link" below). `motion.c` fuses both into one score.
  P1 (motion) is live-tested and working; P2-P5 (presence, count,
  breathing, position) are not built yet.
- Display renders a live home screen (LVGL) fed by the hub's ESP-NOW
  STATE broadcasts: a motion indicator, fused score, and a 2-series trend
  chart of S1 vs S3 scores.
- P2 (presence) code is written (`presence.c`/`.h`) and verified correct
  in a clean test, but its wander thresholds are still unvalidated
  placeholders.
- **Opening/closing the serial port resets HUB-1** unless DTR and RTS
  are held low before open. All tools now open ports through
  `tools/serial_util.py` `open_port()`, which does that — use it in any
  new tool. (An earlier "DTR/RTS refuted" conclusion was wrong; see
  docs/boards.md.) Also: `run_in_background` Bash calls on this machine
  launch duplicate Python processes — never use it for serial.
- **Fixed**: both boards' `ping_gw.c` could get stuck retrying a dead
  ping forever without `WIFI_EVENT_STA_DISCONNECTED` ever firing. Both
  now run a watchdog task that force-reconnects after a 5s stall. One
  occurrence traced precisely: the hub's own S1 ping got stuck, starving
  it enough to fail the SoftAP's WPA2 handshake with the display
  (reason code 15) — breaking S3 too without the hub ever crashing.
- The ~8s router reconnect cycle seen after adding the watchdog was the
  watchdog itself: "Amira_Guest"'s gateway never answers ICMP, so every
  connection looked stalled. Fixed — the hub watchdog now arms only after
  a first reply. Side effect of no replies: S1 CSI is only ~2-5 pkt/s.
- **In progress**: nearby phone detection (BLE + Wi-Fi sniffing on the
  hub, Phones page on the display) — see the "CURRENT CHANGE" section of
  the plan file named in docs/boards.md. Deferred items are listed in
  docs/boards.md's "Backlog" section.

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

- Shared code between the two firmware projects goes in `components/`
  (`wifeel_proto` for the ESP-NOW wire format, `wifeel_csi` for CSI capture
  and feature extraction) — never duplicate this logic between
  `firmware/sense` and `firmware/display`.
- Raw CSI data never crosses the ESP-NOW link — only extracted features and
  results. Keep it that way; it's both a bandwidth and a privacy boundary.
- Every firmware prints a boot banner with a version string, target, and (hub
  only) antenna mode — required for the flashing skill's post-flash
  verification checklist. Bump the version string on every firmware change
  that changes behavior.
- `docs/boards.md` is the source of truth for which physical unit is which —
  keep it updated whenever a board is re-flashed, relabeled, or replaced.
