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
| HUB-1 | Sensor hub | Seeed XIAO ESP32-C6 (ext. antenna) | _pending `chip-id`_ | _pending_ | _pending_ | _not yet flashed_ |
| DISP-1 | Display + 2nd sensor | Waveshare ESP32-C6-Touch-AMOLED-1.43 | _pending `chip-id`_ | _pending_ | _pending_ | _not yet flashed_ |

Fill in each row the first time a board is identified with
`python -m esptool --port COMx chip-id`, per the esp32-firmware-flashing
skill. Physically label each board with its row name (HUB-1 / DISP-1) once
identified — there's only one of each right now, but this stops future mixups
if a second XIAO or Waveshare unit is added.

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
- `firmware/display`: not started yet. First step is flashing
  `vendor/waveshare_10_lvgl_v9_test/` unmodified (milestone 3).

## Known per-unit quirks

_None yet — add here as they're discovered (e.g. "HUB-1 needs
`esp_wifi_set_max_tx_power` lowered, connection drops otherwise")._
