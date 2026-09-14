#!/usr/bin/env python3
"""
Non-interactive serial capture for WiFeel boards.

`idf.py monitor` needs a real TTY, which isn't available when Claude Code
drives flashing/verification from a non-interactive shell (see CLAUDE.md
and the esp32-firmware-flashing skill's post-flash verification step).
This is the substitute: open the port, print everything for N seconds
(or until a marker line appears), then exit cleanly.

Usage:
    python tools/serial_log.py COM5
    python tools/serial_log.py COM5 --baud 115200 --seconds 20
    python tools/serial_log.py COM5 --until "free heap"   # stop early once seen
    python tools/serial_log.py COM5 --reset               # toggle DTR/RTS to reset first
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed — run: pip install -r tools/requirements.txt", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="e.g. COM5")
    parser.add_argument("--baud", type=int, default=115200, help="firmware's Serial baud rate (default 115200)")
    parser.add_argument("--seconds", type=float, default=20.0, help="capture duration (default 20s)")
    parser.add_argument("--until", default=None, help="stop early once a line contains this substring")
    parser.add_argument("--reset", action="store_true",
                         help="toggle DTR/RTS to reset the board before capturing (native-USB boards: "
                              "may not actually reset — see CLAUDE.md's manual bootloader entry notes)")
    parser.add_argument("--out", default=None, help="also write captured lines to this file")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"failed to open {args.port}: {e}", file=sys.stderr)
        return 1

    if args.reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.1)

    out_fh = open(args.out, "w", encoding="utf-8", errors="replace") if args.out else None
    deadline = time.monotonic() + args.seconds
    found = False
    try:
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            print(text)
            if out_fh:
                out_fh.write(text + "\n")
            if args.until and args.until in text:
                found = True
                break
    finally:
        ser.close()
        if out_fh:
            out_fh.close()

    if args.until and not found:
        print(f"--- {args.seconds}s elapsed without seeing {args.until!r} ---", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
