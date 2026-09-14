#!/usr/bin/env python3
"""
Interactively join HUB-1 to a Wi-Fi network without fighting the
console's ongoing background logging (Wi-Fi reconnects, motion/presence
events) in a raw serial terminal.

Run this yourself, in your own terminal — it prompts for the SSID and
password locally (the password is masked via getpass, never echoed to
the screen, never printed, never sent anywhere but the board itself)
and sends the `join` command directly over the serial port. The board's
own echo of the command line (which would otherwise contain the
password in plaintext) is filtered out of the output.

Usage:
    python tools/join_network.py COM9
"""
import argparse
import getpass
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed — run: pip install -r tools/requirements.txt", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="e.g. COM9")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    ssid = input("Wi-Fi SSID: ").strip()
    if not ssid:
        print("no SSID entered, aborting", file=sys.stderr)
        return 1
    password = getpass.getpass("Wi-Fi password (leave blank for an open network): ")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"failed to open {args.port}: {e}", file=sys.stderr)
        return 1

    cmd = f"join {ssid} {password}" if password else f"join {ssid}"

    try:
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()

        print("sent, waiting up to 20s for a result (background log lines may still appear)...")
        deadline = time.monotonic() + 20.0
        done = False
        while time.monotonic() < deadline and not done:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")

            # Never print the board's echo of what we typed — it contains
            # the password in plaintext. Also mask it defensively if it
            # shows up anywhere else for some reason.
            if text.strip() == cmd.strip():
                continue
            if password and password in text:
                text = text.replace(password, "***")

            print(text)
            if text.startswith("joined ") or "join failed" in text or "timed out" in text:
                done = True
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
