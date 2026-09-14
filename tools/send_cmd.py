#!/usr/bin/env python3
"""
Send a single line to a WiFeel board's esp_console REPL and capture the
reply — for commands that take no sensitive arguments (e.g. `status`).

Never use this to send credentials (e.g. `join <ssid> <password>`) — typing
those belongs to the person at the keyboard, in their own terminal (see
tools/serial_log.py's docstring and CLAUDE.md), not to an automated script
whose invocation may be logged.

Usage:
    python tools/send_cmd.py COM9 status
    python tools/send_cmd.py COM9 status --seconds 5
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed — run: pip install -r tools/requirements.txt", file=sys.stderr)
    sys.exit(1)

# Commands known to take a password/credential as an argument — refused
# outright, so this tool can't be misused to funnel a secret through an
# automated call even by accident.
_CREDENTIAL_COMMANDS = {"join", "passive"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="e.g. COM9")
    parser.add_argument("command", help="command line to send, e.g. 'status'")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=4.0, help="how long to capture the reply for")
    args = parser.parse_args()

    first_word = args.command.strip().split()[0].lower() if args.command.strip() else ""
    if first_word in _CREDENTIAL_COMMANDS:
        print(f"refusing to send '{first_word}': it takes a credential as an argument — "
              f"type this one yourself in an interactive terminal instead.", file=sys.stderr)
        return 1

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"failed to open {args.port}: {e}", file=sys.stderr)
        return 1

    try:
        ser.reset_input_buffer()
        ser.write((args.command + "\n").encode("utf-8"))
        ser.flush()

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = ser.readline()
            if line:
                print(line.decode("utf-8", errors="replace").rstrip("\r\n"))
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
