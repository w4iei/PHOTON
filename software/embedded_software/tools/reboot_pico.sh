#!/usr/bin/env bash
set -euo pipefail

# Usage: ./pico-reboot.sh [SERIAL_DEVICE]
# If no device is given, it will auto-pick the first matching port.

DEV="${1:-}"

if [[ -z "${DEV}" ]]; then
  # macOS first, then Linux fallbacks
  for d in /dev/tty.usbmodem* /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$d" ]] && DEV="$d" && break
  done
fi

if [[ -z "${DEV}" || ! -e "${DEV}" ]]; then
  echo "No serial device found. Pass it explicitly, e.g.:"
  echo "  ./pico-reboot.sh /dev/tty.usbmodem1101"
  exit 1
fi

echo "Target device: ${DEV}"

# 1) Kill any process holding the port
if command -v lsof >/dev/null 2>&1; then
  PIDS="$(lsof -t "${DEV}" || true)"
  if [[ -n "${PIDS}" ]]; then
    echo "Killing processes using ${DEV}: ${PIDS}"
    kill -9 ${PIDS} || true
    sleep 0.3
  fi
fi

# 2) Put the TTY in a sane state (baud is irrelevant for CDC-ACM, but set anyway)
if [[ "$OSTYPE" == "darwin"* ]]; then
  stty -f "${DEV}" 115200 -echo raw || true
else
  stty -F "${DEV}" 115200 -echo raw || true
fi

# 3) Send Ctrl-C (0x03) to break, then Ctrl-D (0x04) to soft reboot
printf '\x03' > "${DEV}"
sleep 0.05
printf '\x04' > "${DEV}"

echo "Soft reboot sent to ${DEV}"