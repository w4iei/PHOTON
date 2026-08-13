#!/bin/bash
# Flash all three PHOTON boards with build/photon.uf2, then run a measurement.
# Usage: flash_all.sh <label> [--bridge-only]
# Run from firmware/. Board 2's console port is held by the broker, so its
# bootsel command goes through the broker's TCP side (localhost:7777).
set -u
LABEL="${1:?label}"
SCOPE="${2:-all}"
UF2=build/photon.uf2
BRIDGE=/dev/cu.usbmodem11201
B1=/dev/cu.usbmodem11401
B2=/dev/cu.usbmodem11301

wait_drive()      { for i in $(seq 1 80); do [ -d /Volumes/RP2350 ] && return 0; sleep 0.5; done; return 1; }
wait_drive_gone() { for i in $(seq 1 80); do [ ! -d /Volumes/RP2350 ] && return 0; sleep 0.5; done; return 1; }
wait_port()       { for i in $(seq 1 120); do [ -e "$1" ] && return 0; sleep 0.5; done; return 1; }

flash_one() { # $1 = name, $2 = port, $3 = bootsel method: smoke|tcp
  echo "== $1: entering bootsel"
  if [ "$3" = tcp ]; then
    printf 'bootsel\r' | nc -w 2 localhost 7777
  else
    python3 tools/smoke.py --port "$2" bootsel >/dev/null 2>&1
  fi
  wait_drive || { echo "FAIL: $1 bootsel drive never appeared"; exit 1; }
  sleep 1.5
  # cp often reports an I/O error because the board reboots the instant the
  # last block lands; the flash itself is fine, so don't treat it as fatal.
  cp "$UF2" /Volumes/RP2350/ 2>/dev/null || true
  sync 2>/dev/null || true
  wait_drive_gone || { echo "FAIL: $1 drive stuck (copy did not complete?)"; exit 1; }
  wait_port "$2" || { echo "FAIL: $1 CDC port did not return"; exit 1; }
  echo "== $1: flashed, port back"
}

if [ "$SCOPE" = all ]; then
  flash_one "board1" "$B1" smoke
  flash_one "board2" "$B2" tcp
fi
flash_one "bridge" "$BRIDGE" smoke

sleep 5   # discovery pings
python3 -u tools/measure.py --port "$BRIDGE" --label "$LABEL"
