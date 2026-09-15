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

**HUB-1 and DISP-1 are on the same desk, on two sides of it** (confirmed
by the user, 2026-09-14) — same room, short distance apart. This is
consistent with S3's very strong, high-rate link (~100 pkt/s, RSSI
typically -50 to -60 dBm) versus S1's weaker/farther router link (~4
pkt/s, RSSI often -70 dBm or worse) — S3's sensing "coverage" is
concentrated in a small area around the desk itself, not the whole room.
Worth remembering when interpreting S1 vs S3 disagreements: a person at
the desk should show up on both; a person elsewhere in the room mainly
on S1 (if at all).

- Date of last empty-room calibration: _pending — `calib` console
  command exists (presence.c) but hasn't been run yet as of this note;
  see the "Next session should start here" list above._
- Date of last people-count / position training session, and its confusion
  matrix (see `docs/` for saved `train_models.py` output): not started
  (P3/P5 not built yet).

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

### Update: algorithm revision after real-world unreliability report (same day)

User-reported symptom after the above: score bouncing "8-38 quite
randomly, when moving or when still" — the original single-test success
didn't hold up.

**Tried and reverted**: a gain-invariant "turbulence" metric (coefficient
of variation — std/mean — of amplitude across the 8 subcarrier groups,
matching francescopace/espectre's documented ALGORITHMS.md approach),
replacing the temporal amplitude-diff. Verified against real hardware in
a controlled test (user confirmed actively walking for the full window):
**zero response** — raw_jitter stayed flat (0.02-0.19) throughout,
despite confirmed real motion. Reverted. Best-guess reason: the technique
assumes real per-subcarrier frequency data with deliberate spacing;
`WIFEEL_CSI_SUBCARRIER_GROUPS` here are arbitrary contiguous byte-chunks
of a mixed-format buffer (see `extract_frame_amplitude()`'s
SIMPLIFICATION comment), so cross-group variance doesn't carry the
spatial-frequency meaning the technique depends on with this simplified
grouping. A real fix would need actual per-subcarrier decoding first —
out of scope for now.

**What actually fixed it**: kept the original (empirically-proven)
temporal amplitude-diff jitter, and added an **adaptive noise floor** in
`motion.c` (tracks down immediately, creeps up slowly via
`FLOOR_CREEP_ALPHA`) — score is now `(jitter - floor) / MOTION_SCORE_DELTA_RANGE`
rather than jitter against a fixed absolute constant. This means the
score is self-calibrating to whatever this room/network's actual resting
jitter level is, rather than assuming a fixed number transfers across
different connections/environments — likely the real cause of the
"random 8-38" complaint (different sessions' resting jitter levels
weren't the same absolute value, so a fixed threshold produced
inconsistent-looking scores).

**Re-verified live** (multiple walk-bys, same session): baseline mostly
0-15 at rest, four separate real motion events cleanly detected
(peaks of 100, 57, 50, 47; clean returns to single-digit scores each
time). Floor tracked from 0.29 to 0.89 over the ~20s test as ambient
conditions drifted, exactly as designed.

**Still not done**: `MOTION_SCORE_DELTA_RANGE`=2.5 and the enter/exit
score thresholds (40/20) are still first-pass estimates from this one
extended session, not validated over a long unattended period or against
a deliberate false-positive test (e.g. leaving the room untouched for an
hour).

## Second sensing node: hub SoftAP + display STA (same day)

User feedback after the above: motion score still felt unreliable, and
correctly pointed out the system was only using one board for sensing —
the plan's original design called for both. Fix: **the hub now runs its
own SoftAP (`WIFEEL_LINK_AP_SSID`/`WIFEEL_LINK_AP_PASSWORD` in
wifeel_proto.h — a fixed local credential Claude invented, NOT the user's
home network, so no credential relay/handling problem) and the display
joins it as a real Wi-Fi station**, `WIFI_MODE_APSTA` on the hub
(coexists fine with its separate STA connection to the real router).
This gives a second, independent CSI stream — S3, display→hub — using
the same proven data-frame CSI path as S1 (JOIN mode), not raw ESP-NOW
broadcasts (which, per the earlier DIRECT-mode finding, never produce
CSI at all).

**Verified working live**:
- Display joins the hub's SoftAP, gets a DHCP IP (192.168.4.2), and pings
  the hub (192.168.4.1) at 100Hz, exactly like S1's gateway-ping design.
- Hub tracks the display's MAC as S3 automatically via
  `WIFI_EVENT_AP_STACONNECTED` (see `wifi_mgr_set_ap_peer_connected_cb`).
- **S3 pkt/s measured at ~100/s — roughly 25x S1's ~4/s.** Makes sense:
  the hub-display link is short-range/strong-signal (same desk) vs. the
  router link's greater distance/obstacles. This far exceeds the
  original plan's "~80-100 pkt/s" target that JOIN mode alone couldn't
  reach.
- `motion.c` now computes a score per stream (each with its own adaptive
  floor) and fuses by taking the max — real S3-attributed MOTION events
  observed live (e.g. "score=76 ... S3=76/2.06/0.15" with S1 at 0),
  confirming S3 contributes real, independent signal, not just noise.

**Known rough edge, not yet root-caused**: S3's jitter/pkt-rate briefly
went to exactly 0.00 for about 20 seconds mid-session before recovering
on its own (confirmed via `status` before and after). Not yet understood
— possibly a brief display-side reassociation or power-save blip. Worth
watching for if S3 seems to "go quiet" during testing; `status` shows
current S3 pkt/s to check.

**Not done**: S4 (display running its own CSI capture on the hub's AP
traffic, for a symmetric second measurement) — only S3 (hub-side) is
built. The display is still a CSI *source* (via its ping traffic) but not
yet a CSI *receiver* itself. A real "both streams must agree" fusion
policy (vs. today's simple max) also wasn't tried — no dual-stream data
existed yet to evaluate it against.

## Per-stream calibration + trend chart + a real reliability problem (same day, later)

User noticed S3's score reads persistently low relative to S1 on the new
display chart, and asked whether that's expected. Real cause identified:
S3 samples ~25x faster than S1 (~100Hz vs ~3-4Hz), and the fast-jitter
metric measures change *between consecutive samples* — at a much higher
sample rate, consecutive samples are closer together in time and
naturally show smaller diffs for the *same* real motion. Both streams
were sharing one `MOTION_SCORE_DELTA_RANGE` constant (calibrated from S1
alone), unfairly compressing S3's score. Fixed: `motion.c` now takes a
separate `delta_range` per stream (`MOTION_SCORE_DELTA_RANGE_S1` /
`_S3`).

**S3's own range is not yet properly calibrated** — attempts to gather
clean walk-by data for it kept getting corrupted by a real, unresolved
reliability problem (see below), so `MOTION_SCORE_DELTA_RANGE_S3` is
still set equal to S1's value (2.5) as an explicit placeholder, not a
measurement. Revisit once the link below is stable.

**Real, unresolved problem found**: the display's connection to the
hub's SoftAP appears to drop and silently reconnect unpredictably, not
just after a hub reboot (where slow beacon-loss detection, default
~25s, is an understandable explanation) but **also mid-session with
neither board reset** — caught live via the `WIFI_EVENT_AP_STACONNECTED`
log line ("S3 now tracking display...") appearing partway through an
otherwise-idle test. Whenever this happens, S3's jitter/floor reset to
exactly 0.00 until the new association's CSI stream re-accumulates
enough history. This directly corrupted at least one calibration
attempt. Not yet root-caused — candidates to check first: Wi-Pi power-
save interaction despite `WIFI_PS_NONE` being set on both boards, RF
interference/congestion on the shared channel (S1's router link is also
on the same channel 9), or a display-side crash/watchdog reset that
doesn't show up in a partial log capture. **Fix this before trusting any
S3-specific tuning.**

**Also done**: the hub's STATE broadcast rate was raised from 1Hz to 3Hz
(`LINK_RATE_HZ` in firmware/sense/main/link.c) to cut hub-to-display
display latency, and `wifeel_msg_state_t` gained a
`motion_score_streams[]` array so the display can chart each stream's
score separately (not just the fused number) — implemented as a live
2-series `lv_chart` on the display's home screen, user-confirmed
rendering correctly.

**Expected detection-to-screen delay** (from the code's timing constants,
not independently measured end-to-end): roughly up to ~1.0s for an
S3-driven detection (dominated by the hub->display broadcast and display
UI poll, since S3's own fast sample rate settles quickly) and up to
~2.3s for an S1-driven detection (dominated by S1's own sparse ~3-4Hz
sample rate, which is the bottleneck in how fast its jitter EMA can
react at all).

## Empty-room baseline test (same day, ~10 min, nobody present)

Both boards left completely alone for ~590s with the room genuinely
empty (both boards on the same desk, two sides of it — see room layout
above), to investigate the reconnection issue and get a real false-
positive baseline. Full logs saved this session at
`tools/../` (session scratchpad, not in the repo) — key findings:

**Reliability: did NOT reproduce.** Zero AP disconnect/reconnect events
on either board for the entire 10 minutes (the hub's
`WIFI_EVENT_AP_STADISCONNECTED` handler, which logs every drop, never
fired), zero crashes/resets/panics, display log completely silent the
whole time. This strongly suggests the earlier instability is tied to
*active* testing (rapid resets, back-to-back console commands) rather
than a constant background problem — downgrades its urgency, though it's
still unexplained and worth understanding eventually.

**Motion: 4 false positives in 10 minutes, all S1-only.** Scores 52-62,
each lasting 1-2.5s before clearing on its own:

| Time (s since boot) | Fused | S1 score/jitter/floor | S3 score/jitter/floor |
|---|---|---|---|
| 145.8 | 55 | 55 / 1.63 / 0.25 | 7 / 0.28 / 0.09 |
| 442.5 | 52 | 52 / 1.55 / 0.23 | 3 / 0.15 / 0.07 |
| 497.7 | 62 | 62 / 1.90 / 0.33 | 5 / 0.22 / 0.08 |
| 503.4 | 53 | 53 / 1.72 / 0.39 | 3 / 0.19 / 0.11 |

S3 never came close to corroborating any of these (stayed at 3-7 every
time, vs. its own 40-point enter threshold). Since fusion is currently
`max(S1, S3)`, S1's noise alone is sufficient to trigger MOTION. The
last two events (497.7s, 503.4s) are only 5.7s apart — possibly a burst
of real network activity (DHCP renewal, a neighboring device waking up)
rather than four independent unrelated glitches.

**Implication, not yet acted on**: a fusion policy requiring some S3
corroboration (not just S1 alone) would likely have suppressed all four
of these — but this is a real product tradeoff (it would also suppress
genuine S1-only motion somewhere S3 can't see, e.g. near the router but
away from the desk), not a pure bug fix. Flagged for discussion rather
than changed unilaterally.

## Likely root cause of the reconnection instability: our own tooling (same day, later still)

Flashed presence.c (P2) to the hub and tried a live empty-room
calibration test. The hub kept rebooting — uptime visibly reset to a
small number between console queries several times over ~15 minutes,
even though the room was empty and nothing was touched physically. This
looked at first like a new, worse problem than the earlier baseline's
"zero crashes" result. Investigation:

- Confirmed via repeated `status` queries that device uptime kept
  dropping back to single/double-digit seconds between calls — genuine
  reboots, not just link drops (free heap and boot banners consistent
  with a fresh boot each time).
- Caught direct evidence of the mechanism: at one point, **two
  identical `python tools/serial_log.py COM9 ...` processes** were
  found running simultaneously (`Get-CimInstance Win32_Process` showed
  both, same args, different Python interpreters on PATH) — both
  competing for the same COM port. Killing them triggered another
  reboot. A subsequent fresh, single connection *also* triggered a
  reboot moments later.
- Also noticed (via the terminal panel) what looked like a separate
  serial/monitor session that had been connected to COM9 around the
  same time.

**Working theory**: the XIAO ESP32-C6 has no separate USB-UART bridge
chip — Wi-Fi console access goes over the native USB-Serial-JTAG
peripheral, which (like ESP32-S3/C3) implements the same DTR/RTS
auto-reset convenience feature `idf.py flash`/`monitor` rely on, so
normal users don't need a manual BOOT+RESET. That means **any** tool
that opens (or abruptly closes) a handle to COM9 — not just esptool —
can trigger a reset if its DTR/RTS transitions land in a pattern the
peripheral treats as a reset request. This fits every observation this
session:
  - The one **fully hands-off 10-minute baseline** (`baseline_capture.py`
    opens both ports exactly once and holds them for the whole run) saw
    **zero** reboots.
  - Nearly every round of interactive testing — `send_cmd.py`/
    `serial_log.py` calls opening a fresh connection per command — saw
    reboots, sometimes within seconds of each other.
  - The elevated S1 false-positive rate seen right after starting
    calibration (up to 4 events per 30s, scores up to 100, vs. the
    baseline's 4-events-per-10-minutes) is consistent with this too:
    each reboot wipes `motion.c`'s adaptive floor and `fast_jitter_ema`,
    so the first several seconds of CSI after every reboot look like a
    fake motion spike until the floor recatches up. This is very
    likely **not** a real regression in the algorithm — it's a symptom
    of frequent reboots, which is itself a symptom of frequent
    reconnects.
  - It also explains why S3 sometimes failed to recover for minutes at
    a time: when the hub reboots, its SoftAP disappears, but the
    display's `WIFI_EVENT_STA_DISCONNECTED` doesn't always fire
    promptly (link-layer association can outlive the actual dead data
    path), so `ping_gw`'s continuous ICMP session just fails silently
    forever (`ping_sock: send error=0`, hundreds/sec) until something
    (e.g. a manual reset) breaks the deadlock. `firmware/display/main/
    link.c` already calls `ping_gw_stop()` correctly on disconnect —
    the bug is that disconnect isn't always detected, not a missing
    stop call.
  - Presence calibration (`presence.c`) itself is **not buggy** — a
    clean, single-connection 30s test completed correctly
    (`calibration finished` logged right on schedule, `calibrated=yes`
    afterward). The earlier "never completes" observation was a
    casualty of a mid-calibration reboot wiping `presence.c`'s
    in-RAM `s_calibrating`/`s_have_baseline` statics, not a logic bug.

**Not fully proven** — pySerial's exact DTR/RTS behavior on open/close
wasn't directly instrumented, and the correlation, while strong, wasn't
100%. But this is now the leading explanation for the reconnection
instability investigated (and left unresolved) earlier this session,
and it reframes the problem from "mystery RF/firmware reliability bug"
to "diagnostic tooling opens too many short-lived serial connections."

## DTR/RTS theory tested directly, and refuted; two real bugs found and fixed instead (same day, later still)

Ran a controlled experiment before doing any more calibration work:
alternated opening COM9 with pySerial's default DTR/RTS state vs. with
both explicitly held low *before* `open()`, 3 reps each. Result: only 1
reboot in 6 open/close cycles, no clean split between the two
conditions — **inconclusive**, not the confirmation expected. A
follow-up single continuous connection held for 4 minutes straight saw
**zero** reboots at all (clean, unbroken uptime the whole window). So
opening a serial connection does not itself reset the hub — the DTR/RTS
theory from the previous session's entry is **refuted**.

What actually explains the earlier chaos, found while testing this:
`run_in_background` on the agent's own Bash tool was launching each
`tools/serial_log.py`/`send_cmd.py` command through **two different
Python interpreters simultaneously** (confirmed via `Get-CimInstance
Win32_Process` — identical command line, two PIDs), both fighting over
COM9. That's a real, reproducible contributor to the earlier port
contention. Foreground (non-backgrounded) calls don't show this.
Practical fix: avoid `run_in_background` for anything touching a
serial port on this machine.

That same clean 4-minute capture also surfaced a **real, separate bug**:
S1 was producing near-continuous false MOTION (~1 event per 7-8s, jitter
up to 11) despite zero reboots — ruling out the "reboot resets the
floor" explanation. Checking the display board (COM8) explained it:
stuck in the same `ping_sock: send error=0` loop as before, this time
for ~40 minutes straight, with the hub never having rebooted. Root
mechanism, confirmed from the hub's own log: `wifi:station ... leave,
AID = 1, reason = 15` — reason 15 is a **WPA2 4-way handshake timeout**.
The hub's own S1 ping was *also* stuck failing (same bug, hub's own
copy of `ping_gw.c`), spamming `esp_now_send failed:
ESP_ERR_ESPNOW_NO_MEM` from resource exhaustion, and too busy/starved to
complete the SoftAP handshake with the display in time — one cascading
failure breaking both streams, not two independent ones.

**Fixed on both boards**: `ping_gw.c` now tracks time since its last
successful reply via `esp_ping`'s own `on_ping_success` callback
(deliberately not `on_ping_timeout` — a dead link fails at the socket
*send* itself, which doesn't reliably reach a timeout callback either).
Each board runs a small watchdog task (`firmware/display/main/link.c`,
`firmware/sense/main/app_main.c`'s `s1_ping_watchdog_task`) that polls
this and forces a disconnect+reconnect after a 5s stall, rather than
trusting `WIFI_EVENT_STA_DISCONNECTED` alone. Confirmed working live —
watchdog fires, hub reconnects cleanly within ~1-2s each time.

**New finding this exposed**: with the watchdog now recovering instead
of hanging forever, it became visible that the hub's S1 link
disconnects/reconnects on its own roughly **every 6-8 seconds**,
cycling between different BSSIDs each time (`16:33:75:1c:4b:c2` /
`32:bd:13:1e:25:85` / `12:71:b3:13:d2:84`) on a network named
**"Amira_Guest"**. This looks like mesh/guest-network behavior (guest
SSIDs commonly apply session limits, band-steering, or rate limits that
a continuous 100Hz ping could be triggering), not a firmware bug — the
watchdog now papers over it reliably, but the underlying 6-8s churn is
still there and may need a network-side change (e.g. joining the main
network instead of the guest one, if that's an option) rather than more
firmware work.

## Ping-rate test: the ~8s reconnect cycle is NOT load-related (same day)

Hypothesis (user's): the guest network is kicking the hub as
"self-defense" against the sustained 100Hz gateway ping. Tested by
dropping `JOIN_PING_INTERVAL_MS` from 10ms to 50ms (5x less traffic).
Result: the watchdog-forced reconnects kept landing ~8.0s apart
(32378 / 40388 / 48399 / 56411 / 64422 ms) — identical to before. A
load/rate trigger would have shifted that timing; it didn't move at all.
Reverted to 10ms. The connection is WPA3-SAE H2E on a mesh (BSSID
changes each reconnect), so a periodic mesh-side re-key/steering timer
is now the leading guess. Next test: join the main network instead of
"Amira_Guest".

## Correction: the ~8s reconnect cycle was our own watchdog (same day)

The two sections above blamed "Amira_Guest" for the ~8s reconnects. That
was wrong. With the ping sends checked directly (zero `ping_sock: send
error`), the watchdog fired ~6.8s after *every* fresh connection with "no
successful ping" — the gateway on this guest network **never answers
ICMP at all**. The S1 watchdog therefore declared every connection dead
(5s stall + up to 2s check interval) and reconnected, forever. That's why
the ping-rate change couldn't move the timing, and why the hub had stayed
connected for 2+ hours before the watchdog existed. The BSSID changes
were just each forced reconnect landing on a different mesh node.

It also explains S1's rate all along: ~2-5 pkt/s from the AP's own
frames, never the ~100 pkt/s the plan expected from ping replies.

**Fixed**: the hub watchdog only arms after the gateway has answered at
least once on the current connection (`ping_gw_has_succeeded()`), and
`status` now prints whether the gateway answers. The extra
`esp_wifi_connect()` after a forced disconnect was removed on both boards
— the STA_DISCONNECTED handler already reconnects, and the double call
raced ("sta is connecting, return error").

**Baseline after the fix** (5 min, one connection, `tools/status_poll.py`,
no BLE, 10ms ping): S1 median 2.5 pkt/s (1.0-5.0), S3 median 100.0
(98.1-101.0), free heap 268 KB, hub STA disconnected on 0/20 polls.

## Correction: opening the serial port DOES reset HUB-1 — fixed in tools (same day)

The "DTR/RTS theory refuted" conclusion above was too strong. The 4-minute
single-connection test only proved the hub doesn't reboot *while* a
connection stays open; it never tested open/close. During BLE duty-cycle
measurements, runs kept starting with a fresh boot (advert counter back
near zero, duty back to its default, an extra boot-banner "free heap"
line), and one 5-second `send_cmd.py` capture caught **two** boots in a
row. pySerial's default open raises DTR and RTS one after the other (and
close drops them); the C6's USB-Serial/JTAG maps those lines to reset/boot
like esptool's auto-reset, so the transitions can reset the chip.

**Fix**: `tools/serial_util.py` `open_port()` sets DTR and RTS low
*before* `open()` so they never change (ESP-IDF's monitor does the same).
All tools use it now. Verified: 8 back-to-back `send_cmd.py` connections,
advert counter rose 87 → 139 with no boot banner. This very likely
explains many of the unexplained hub reboots seen earlier in the project.

## BLE scan duty vs CSI packet rate (phone detection, step 2)

NimBLE passive observer added to the hub (`ble_scan.c`). Measured with
`tools/status_poll.py`, 3 min per setting, one connection, after the
serial fix above:

| BLE duty | S3 pkt/s median (min) | S1 pkt/s median | Adverts/s |
|---|---|---|---|
| 0% | 100.0 (98.7) | 3.0 | — |
| 10% | 100.0 (95.4) | 2.0 | ~1.4 |
| 25% | 99.0 (95.0) | 3.8 | ~3.5 |
| 50% | 96.7 (91.9) | 2.0 | ~7 |

S1 is too noisy (0.5-12 pkt/s) to show a trend. Default set to **25%**:
effectively free for CSI. First visible S3 cost appears at 50%. Boot free
heap dropped from ~276 KB to ~225 KB with NimBLE; runtime ~211 KB. App
binary 920 KB → 1.2 MB (62% of partition free). Wi-Fi accepted
`WIFI_PS_NONE` with BT coexistence enabled.

Ambient capture (15s, nobody's phone nearby): a Microsoft PC, a Samsung
device, an advert named "BYD BLE3" (likely a car), all at -85 to -100 dBm.
No Apple adverts.

## BLE vendor classifier validated against real devices (2026-09-15)

With 4 known devices on the desk (PC, Zigbee hub, Raspberry Pi, Samsung
phone), captured via `phones raw`:

| Captured signal | Company/UUID (Bluetooth SIG registry, fetched live) | Matched device |
|---|---|---|
| Manufacturer id `0x0006` | Microsoft Corporation | PC |
| Manufacturer id `0x0075` | Samsung Electronics Co. Ltd. | Samsung phone |
| Manufacturer id `0x005D` | Broadcom Corporation | Raspberry Pi (Broadcom/Cypress wireless chip is standard on Pi boards) |
| Service Data (AD 0x16) UUID `0xFCF1` | Google LLC (member UUID) | Android/Google Play Services beacon, seen alongside the Samsung phone's own advert |

Zigbee hub never appeared — plausible, since Zigbee (802.15.4) is a
separate radio from BLE and plenty of Zigbee-only hub chips don't
implement BLE at all.

**`ble_scan.c` updated**: added `COMPANY_BROADCOM` (documented as
non-phone, matches the existing default), and service-data (AD type
`0x16`) parsing for Google's `0xFCF1` member UUID — `ble_scan_classify()`
now scans every AD structure in an advert rather than stopping at the
first one, since a single advert can carry both a manufacturer-data field
and a service-data field. Google-via-service-data is not scored
phone-like (could equally be a Chromecast/speaker/TV); Samsung and
Google-via-manufacturer-data remain provisional per the original plan.

Sources used: [Bluetooth SIG company identifiers](https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml),
[Bluetooth SIG member UUIDs](https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/uuids/member_uuids.yaml).

## Phone detection: device table + Wi-Fi sniffing (2026-09-15)

Steps 3-4 of the phone-detection plan. `devices.c` tracks up to 32
BLE/Wi-Fi sightings (median-of-5 + EMA RSSI smoothing, log-distance
estimate, per-source expiry), fed via a non-blocking queue so radio
callbacks never stall. `phones calib ble|wifi <s>` verified live: picked
the strongest tracked entry and updated the 1m reference (-59.0 ->
-29.0 dBm), all distances recomputed immediately.

`wifi_sniff.c` adds a second promiscuous RX callback (alongside CSI's
own) to see which clients are on the home network — only counts traffic
whose BSSID is already known to be "home" (seeded from the hub's own
association, grown from matching beacons). Correctly distinguishes
client->AP frames (real RSSI, usable for distance) from AP->client
frames (the AP's RSSI, presence-only — `devices_touch()`). Beacon-based
mesh BSSID discovery confirmed live (home BSSID count went 1 -> 2 on its
own). Zero Wi-Fi clients tracked so far — plausible given this board is
2.4GHz-only and known nearby devices are likely on Ethernet/5GHz, not a
confirmed bug; `phones` prints `data_frames_seen/not_home_bssid/
excluded/tracked` counters to keep investigating without guessing.

`phones selftest` now covers both the BLE classifier (4 real captured
adverts) and the 802.11 parser (4 hand-derived synthetic frames: ToDS,
FromDS, IBSS/WDS rejection, beacon SSID extraction) — 8/8 passing. The
parser is split into pure functions (`wifi_sniff_parse_data_frame`,
`wifi_sniff_parse_beacon`) shared by the real callback and the selftest,
so a selftest failure would mean the live path is broken too.

CSI unaffected: S3 still ~100 pkt/s with BLE at 25% duty and Wi-Fi
sniffing both running.

## Backlog (deferred while phone detection is built)

Phone detection (BLE + Wi-Fi sniffing, hub only) was prioritized ahead
of these by the user on 2026-09-14:

- **Console typing**: the hub's constant logging makes the interactive
  console unusable, so `join` can't be typed by hand. Needs a `quiet`
  command or a lower default log level. Workaround written:
  `tools/join_network.py` (prompts locally, masks the password) —
  untested.
- **S1 gets no ping replies on "Amira_Guest"** (gateway doesn't answer
  ICMP), so S1 CSI is only ~2-5 pkt/s from the AP's own frames. Options:
  join the main network, or generate S1 traffic another way (e.g. UDP/DNS
  to the gateway). With no replies, the S1 watchdog can't detect a stuck
  link on this network; an AP-frame-arrival liveness check would cover it.
- **Never use `run_in_background` for serial captures** on this machine
  — it launches duplicate Python processes that fight over the port.
- **Still to validate**: S1 and S3 motion walk-bys (boards sit close on
  one desk, so both should react together), then presence, then the
  fusion policy. `MOTION_SCORE_DELTA_RANGE_S3` is still a placeholder.

### Next session should start here
1. **Ask about the "Amira_Guest" network**: is the hub meant to be on a
   guest network long-term, or would the main/home network avoid the
   ~6-8s reconnect cycle? This is likely the actual remaining reliability
   issue, now that both boards recover from it instead of hanging.
2. Once the link (however it ends up configured) is stable for a
   sustained period, redo the S3-focused walk-by test cleanly and set a
   real `MOTION_SCORE_DELTA_RANGE_S3` (still an unvalidated placeholder
   equal to S1's value) — per-channel validation before any fusion
   policy changes (explicit user instruction).
3. Redo empty-room presence calibration and validate
   `PRESENCE_WANDER_THRESHOLD_S1`/`_S3` (still placeholders) against a
   real "person sitting still nearby" test, one stream at a time.
4. Only after S1 and S3 are independently validated for both motion and
   presence: revisit the S1-alone-triggers-MOTION fusion policy question
   from the original baseline.

## Known per-unit quirks

_None yet — add here as they're discovered (e.g. "HUB-1 needs
`esp_wifi_set_max_tx_power` lowered, connection drops otherwise")._
