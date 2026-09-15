# WiFeel

Camera-free Wi-Fi sensing — motion, presence, and nearby-phone detection —
using Wi-Fi Channel State Information (CSI) from an existing Wi-Fi network,
plus BLE/Wi-Fi scanning. Two ESP32-C6 boards, fixed in one room.

> **Not a medical device.** Every reading here is experimental and
> informational, not diagnostic.

## Status

| Feature | State |
|---|---|
| **Motion** (P1) | Working, live-tested |
| **Presence** (P2) | Built, calibrates, but thresholds are unvalidated placeholders |
| **Nearby phone detection** | Working: BLE scan + Wi-Fi client sniff + a "Phones" page on the display |
| **People count / breathing / rough position** (P3–P5) | Not built |

See [`docs/boards.md`](docs/boards.md) for the detailed findings log (what's
been tested, what's still a placeholder, open issues) and
[`CLAUDE.md`](CLAUDE.md) for day-to-day dev notes.

## Hardware

| Role | Board | Notes |
|---|---|---|
| Sensor hub | [Seeed XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) | External u.FL antenna required — see `docs/boards.md` |
| Display + 2nd sensor | [Waveshare ESP32-C6-Touch-AMOLED-1.43](https://www.waveshare.com/esp32-c6-touch-amoled-1.43.htm) | 466×466 round AMOLED, fixed placement |

Both boards sit in the same room (works best close together — the display's
own link to the hub is itself a second CSI sensing stream) and run native
**ESP-IDF v5.5.5**.

## Repo layout

```
components/
  wifeel_proto/   Shared ESP-NOW wire protocol (structs, CRC)
  wifeel_csi/     Shared CSI processing: ring buffer, subcarrier cleanup, features
firmware/
  sense/          Hub firmware (XIAO ESP32-C6): CSI capture, motion/presence
                  fusion, BLE scanning, Wi-Fi client sniffing, console
  display/        Display firmware (Waveshare AMOLED): LVGL UI (Home +
                  Phones tiles), 2nd CSI stream, hub link
tools/            Python: non-resetting serial capture/command tools,
                  radio-load measurement, network-join helper
docs/             Per-board flash log, room layout, calibration notes,
                  findings (docs/boards.md)
```

## Setup

### 1. Toolchain

Install ESP-IDF v5.5.5 (see [`CLAUDE.md`](CLAUDE.md) for the exact
Windows/EIM CLI steps this project used). Two independent projects, not a
single multi-target build:

```bash
idf.py -C firmware/sense set-target esp32c6
idf.py -C firmware/display set-target esp32c6
```

### 2. Build and flash

```bash
idf.py -C firmware/sense build
idf.py -C firmware/sense -p COMx -b 921600 flash

idf.py -C firmware/display build
idf.py -C firmware/display -p COMy -b 921600 flash
```

Confirm which physical board is on which port first — see
[`docs/boards.md`](docs/boards.md). Before the first WiFeel flash on a board
(or after it ran different firmware), erase flash first:
`idf.py -C firmware/sense -p COMx erase-flash`.

`idf.py monitor` needs an interactive terminal. `tools/serial_log.py COMx
--seconds 20` captures boot output without one — see that file's docstring.
All the `tools/` scripts open the serial port in a way that avoids
unexpectedly resetting the board (see `tools/serial_util.py`); a plain
terminal program that doesn't do the same may reset it on connect.

### 3. Join your Wi-Fi network

The hub has an interactive console over its USB serial port, but it also
logs continuously, which makes typing a command (especially one with a
password) hard to see. Two ways to join:

```bash
# Types over the log noise for you — password is prompted locally,
# masked, and never printed or sent anywhere but the board:
python tools/join_network.py COM9

# Or type directly into a serial terminal, once connected:
join <ssid> <password>
```

Once joined, the hub starts sensing immediately — no further setup needed.
The display connects to the hub automatically (it's a fixed local link, not
your home network) and starts showing live data as soon as it boots.

### 4. Optional: calibrate presence / phone distance

On the hub's console:

```
calib [seconds]              # empty-room baseline for presence (P2); leave the room
phones calib ble <seconds>   # hold a phone ~1m from the hub for BLE distance estimates
phones calib wifi <seconds>  # same, for Wi-Fi-sourced distance estimates
```

## Using it

- **Display, Home tile**: live motion indicator and a trend chart of the two
  CSI streams (router→hub and display→hub).
- **Display, Phones tile**: swipe left from Home. Shows nearby BLE devices
  (ring gauge), devices confirmed on your Wi-Fi network, and a list of the
  nearest ones with vendor and estimated distance.
- **Hub console** (`status`, `phones`, `phones raw <seconds>`, `phones
  selftest`, `motion <seconds>`): live diagnostics — packet rates, RSSI,
  per-device detail, a self-test against known-good captured data.

## Known limitations

- Distance and presence thresholds are placeholders pending real-world
  calibration — treat numbers as rough, not precise.
- BLE device count can briefly double-count around a phone's ~15-minute
  private-address rotation, and miss idle phones that aren't advertising.
- "Connected to Wi-Fi" detection only sees 2.4GHz traffic (the C6 has no
  5GHz radio) on the hub's own channel.
- Some networks don't answer ICMP pings, which the hub uses to generate CSI
  traffic — see `docs/boards.md` for the impact and workarounds.

## License

Project code: MIT (see `LICENSE`). Built on ESP-IDF's own components
(Apache-2.0, including its NimBLE Bluetooth stack) and
[LVGL](https://github.com/lvgl/lvgl) (MIT) for the display UI — see each
component's own license under `managed_components/` after the first build.
