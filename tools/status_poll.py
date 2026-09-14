#!/usr/bin/env python3
"""
Poll the hub's `status` command over ONE serial connection and summarize
S1/S3 packet rates and free heap — for before/after radio measurements
(e.g. BLE scan duty cycle vs. CSI packet rate).

Keeps a single connection open for the whole run: opening and closing the
port repeatedly was a confounder in earlier reliability investigations
(see docs/boards.md).

Usage:
    python tools/status_poll.py COM9 --minutes 5 --every 15
    python tools/status_poll.py COM9 --minutes 3 --label "duty 10%"
"""
import argparse
import re
import statistics
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed — run: pip install -r tools/requirements.txt", file=sys.stderr)
    sys.exit(1)

S1_RE = re.compile(r"S1 \(router->hub\): ([\d.]+) pkt/s")
S3_RE = re.compile(r"S3 \(display->hub\): ([\d.]+) pkt/s")
HEAP_RE = re.compile(r"free heap: (\d+) bytes")
WIFI_RE = re.compile(r"wifi:\s+(connected|not connected)")


def summarize(name, values, unit):
    if not values:
        return f"{name}: no samples"
    return (f"{name}: median {statistics.median(values):.1f}{unit}  "
            f"min {min(values):.1f}  max {max(values):.1f}  (n={len(values)})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--minutes", type=float, default=5.0)
    parser.add_argument("--every", type=float, default=15.0, help="seconds between status queries")
    parser.add_argument("--label", default="", help="free-text label printed with the summary")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.3)
    except serial.SerialException as e:
        print(f"failed to open {args.port}: {e}", file=sys.stderr)
        return 1

    s1, s3, heap = [], [], []
    disconnected_polls = 0
    polls = 0
    end = time.monotonic() + args.minutes * 60
    next_poll = time.monotonic() + 2.0  # let any in-flight log lines drain first

    try:
        while time.monotonic() < end:
            if time.monotonic() >= next_poll:
                ser.write(b"status\n")
                ser.flush()
                polls += 1
                next_poll += args.every
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace")
            if m := S1_RE.search(text):
                s1.append(float(m.group(1)))
            if m := S3_RE.search(text):
                s3.append(float(m.group(1)))
            if m := HEAP_RE.search(text):
                heap.append(int(m.group(1)))
            if (m := WIFI_RE.search(text)) and m.group(1) == "not connected":
                disconnected_polls += 1
    finally:
        ser.close()

    print(f"=== {args.label or 'status poll'}: {polls} polls over {args.minutes:g} min ===")
    print(summarize("S1 pkt/s", s1, ""))
    print(summarize("S3 pkt/s", s3, ""))
    print(summarize("free heap", [h / 1024 for h in heap], " KB"))
    print(f"polls where hub STA was disconnected: {disconnected_polls}/{polls}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
