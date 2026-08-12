#!/usr/bin/env python3
"""PHOTON native firmware smoke test.

Finds the PHOTON CDC console, runs a command, prints the reply.

    python3 smoke.py                 # runs 'id' then 'stats'
    python3 smoke.py "nodes"         # any console command
    python3 smoke.py --port /dev/cu.usbmodemXXXX "help"
"""
import argparse
import glob
import sys
import time

import serial


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* device found — is the board plugged in?")
    if len(ports) > 1:
        print(f"# multiple candidates, using {ports[0]} (others: {ports[1:]})")
    return ports[0]


def run_command(ser: serial.Serial, cmd: str, settle_s: float = 0.6) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    deadline = time.time() + settle_s
    out = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.15  # keep reading while data flows
    return out.decode(errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*", default=None)
    ap.add_argument("--port", default=None)
    args = ap.parse_args()
    commands = args.commands or ["id", "stats"]

    port = args.port or find_port()
    print(f"# port: {port}")
    with serial.Serial(port, 115200, timeout=0.05) as ser:
        time.sleep(0.3)  # let the banner land after DTR assert
        banner = ser.read(4096).decode(errors="replace")
        if banner.strip():
            print(banner, end="")
        ok = False
        for cmd in commands:
            print(f"\n>>> {cmd}")
            reply = run_command(ser, cmd)
            print(reply, end="")
            if "PHOTON native fw" in reply or "[STAT]" in reply:
                ok = True
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
