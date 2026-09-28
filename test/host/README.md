# Host tests for `components/wifeel_csi`

Run with `./test/host/run.sh` (optionally `./test/host/run.sh <git-rev>` to
print an older revision's numbers alongside, for before/after comparisons).

Needs nothing but `gcc` — no ESP-IDF, no board. `stubs/` supplies the handful
of ESP-IDF types `wifeel_csi.c` touches (`wifi_csi_info_t`, `esp_err_t`, the
`ESP_LOG*` macros), so the **real** component source is compiled and exercised,
not a copy of it.

## What is tested

`rate_independence.c` — feeds two streams the *same* synthetic channel at
different packet rates (3 pkt/s like S1, 100 pkt/s like S3) and reports the
ratio of their `fast_jitter` values. A rate-independent metric gives ~1.0.

`false_positives.c` — replicates `firmware/sense/main/motion.c`'s scoring
(adaptive floor, hysteresis, 300 ms tick) on top of the real stream, and counts
MOTION events in a quiet room plus detections during simulated walk-bys, across
a few noise levels.

## Why

The synthetic channel is a crude stand-in for a real room — it says nothing
about whether the amplitude metric tracks human bodies, which only on-device
testing can establish (see `docs/boards.md`). What it *does* catch is the class
of bug that hid in this pipeline for weeks: arithmetic that silently depends on
packet arrival rate. If you change bucketing, the grid, or any feature the
motion/presence thresholds read, run this first — it is far cheaper than a
flash-and-walk-around cycle, and it fails loudly where hardware testing merely
looks noisy.

Keep `MOTION_*` and `FLOOR_CREEP_ALPHA` in `false_positives.c` in sync with
`motion.c` if you retune them there; the file says which constants are copies.
