# Zigbee → Tuya hub probe (experiment, not WiFeel firmware)

A throwaway ESP32-C6 firmware that proved an ESP32-C6 can join a Tuya Zigbee
hub, appear in the Tuya/Smart Life app, and exchange state **both ways**.
Kept here so the recipe isn't lost; it is **not** built by the WiFeel projects.

## What it proved (2026-09-28, on the display board, DISP-1)

- The C6 joins the hub's Zigbee network as an **End Device** (channel 11 on this hub).
- The Tuya app shows it as **"1 gang touch switch"** with an On/Off button.
- ESP32 → app: the chip flips On/Off every 10 s and the app shows it changing.
- App → ESP32: toggling in the app arrives at the chip (`HUB WROTE On/Off = …`).
- The hub answers every attribute report with a ZCL Default Response `0x00`.
- Stable for 200+ s of continuous running.

## What it took (three things changed together, so which subset is *strictly*
## necessary is not known — all three are cheap and evidence-backed)

1. **`ezb_secur_set_tclk_exchange_required(false)`** — before this, the join
   reached the security handshake and failed with
   `EZB_BDB_STATUS_TCLK_EX_FAILURE` (0x0a), then the device left the network.
2. **On/Off light endpoint via `ezb_zha_create_on_off_light()`** — not a bare
   temperature sensor. A temperature-sensor probe joined the network at the
   Zigbee layer but never appeared in the app.
3. **Endpoint 1** (`ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID` = 1, was 10).
   Tuya devices expose their function on endpoint 1; also matches the
   only public precedent found (espressif/esp-zigbee-sdk issue #752, where
   1–3-gang ESP32-C6 on/off devices with made-up manufacturer/model strings
   were adopted by a Tuya gateway; a 4-endpoint device was mis-detected).

Manufacturer/model strings were left at the SDK defaults
(`ESPRESSIF` / `esp32c6`) — Tuya's "allowlist" did **not** block it.

## Pitfalls found the hard way (both are in this code)

- **The stock example idles forever after leaving the network.** After a
  hub-initiated leave, *and* after `ezb_bdb_reset_via_local_action()` on a
  device with no live network, no signal restarts the search. This probe
  restarts `EZB_BDB_MODE_NETWORK_STEERING` itself.
- **A stored network the hub no longer answers for** shows up as endless
  `BDB Device Reboot failed with status(0x03)`. The probe gives up after 6
  tries, resets locally, and re-steers (saves erasing flash between tries).
- Pairing must be open on the hub at the moment the device searches
  (`NO_NETWORK` = status 0x03 = "no open network found", not a rejection).
  The app's "Add device" scan was enough.

## Rebuilding it

Sources here are drop-in replacements for Espressif's
`esp-zigbee-sdk/examples/home_automation_devices/temperature_sensor/`
(`main/temperature_sensor.{c,h}`, `sdkconfig.defaults`, `partitions.csv`).
Clone the SDK, copy these over, then:

```
idf.py set-target esp32c6
idf.py build
```

Needs `espressif/esp-zigbee-lib` ≥ 2.0 (resolved automatically) and
ESP-IDF ≥ 5.2 (built here on 5.5.5). **Use a short path**: the build nests
deeply and hit Windows' 250-char object-path limit from a deep scratch dir.

## Not yet done

Integrating this into the display firmware alongside its Wi-Fi STA + ESP-NOW
+ LVGL load, and mapping real WiFeel state (motion / presence) onto the
switch instead of the demo 10 s flip. See docs/boards.md.
