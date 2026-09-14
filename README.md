# WiFeel

Camera-free Wi-Fi sensing: motion, presence, people count, breathing rate, and
rough room position — using Wi-Fi Channel State Information (CSI) from an
existing Wi-Fi network. Two ESP32-C6 boards, fixed in one room.

> **Not a medical device.** Breathing-rate output is experimental and only
> meaningful while one person sits still. Treat every reading as informational,
> not diagnostic.

## Hardware

| Role | Board | Notes |
|---|---|---|
| Sensor hub | [Seeed XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) | External u.FL antenna required — see `docs/boards.md` |
| Display + 2nd sensor | [Waveshare ESP32-C6-Touch-AMOLED-1.43](https://www.waveshare.com/esp32-c6-touch-amoled-1.43.htm) | 466×466 round AMOLED, touch, fixed placement |

Both boards run native **ESP-IDF v5.5.x**.

## Sensing priorities

This project is built and tuned in this order (see the plan for the full
rationale and milestone checklist):

1. **Motion** — is anything moving in the room right now
2. **Presence** — is anyone in the room, moving or still
3. **People count** — 0 / 1 / 2 / 3+ (requires in-room training)
4. **Vitals** — breathing rate, only when exactly one person is present and still
5. **Rough position** — which area of the room (requires in-room training)

## Repo layout

```
components/
  wifeel_proto/   Shared ESP-NOW wire protocol (structs, CRC)
  wifeel_csi/     Shared CSI processing: ring buffer, subcarrier cleanup, features
firmware/
  sense/          Hub firmware (XIAO ESP32-C6) — CSI capture + sensor fusion
  display/        Display firmware (Waveshare AMOLED) — LVGL UI + 2nd CSI stream
tools/            Python: serial logging, live plotting, session recording, training
docs/             Per-board flash log (docs/boards.md), room layout, calibration notes
data/             Recorded training sessions (git-ignored — room/person specific)
```

## Building and flashing

See [`CLAUDE.md`](CLAUDE.md) for exact commands. Short version:

```bash
idf.py -C firmware/sense build
idf.py -C firmware/sense -p COMx -b 921600 flash monitor

idf.py -C firmware/display build
idf.py -C firmware/display -p COMy -b 921600 flash monitor
```

## Setup (first use)

1. Power on both boards. On the display, tap the Wi-Fi icon, pick your network,
   and enter its password (or "Listen only" for passive mode with reduced
   accuracy).
2. Leave the room for ~30 s when prompted, to calibrate the empty-room baseline
   (Status page → Calibrate).
3. For people count and rough position, use the Train page to record a few
   minutes of labelled sessions (see `docs/boards.md` for the workflow), then
   retrain with `tools/train_models.py`.

## License

Project code: MIT (see `LICENSE`). Uses Apache-2.0 components from Espressif
(`esp-radar`, `esp-dsp`) and MIT-licensed `lvgl`/`emlearn` — see each
component's own license in `managed_components/` after the first build.
