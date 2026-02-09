#!/usr/bin/env bash
set -euo pipefail

BAUD="${BAUD:-115200}"
PORT_OVERRIDE="${PORT_OVERRIDE:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|--serial|-p)
      PORT_OVERRIDE="${2:-}"
      shift 2
      ;;
    --port=*|--serial=*)
      PORT_OVERRIDE="${1#*=}"
      shift
      ;;
    *)
      echo "Unknown argument: $1"
      echo "Usage: $0 [--port <device>]"
      exit 1
      ;;
  esac
done

find_port() {
  if [[ -n "$PORT_OVERRIDE" ]]; then echo "$PORT_OVERRIDE"; return; fi
  if [[ -n "${PORT:-}" && -e "$PORT" ]]; then echo "$PORT"; return; fi
  local p
  p="$(ls /dev/tty.usbmodem* 2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/tty.usbserial* 2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  echo ""
}

PORT="$(find_port)"

if [[ -n "$PORT_OVERRIDE" && ! -e "$PORT_OVERRIDE" ]]; then
  echo "Requested port not found: $PORT_OVERRIDE"
  exit 1
fi

if [[ -z "$PORT" ]]; then
  echo "No /dev/tty.usbmodem* or /dev/tty.usbserial* device found."
  exit 1
fi

echo "Attaching to ${PORT} @ ${BAUD} (Ctrl-] to exit)..."
while true; do
  python3 -m serial.tools.miniterm "$PORT" "$BAUD" --raw
  status=$?
  if [[ -e "$PORT" ]]; then
    exit $status
  fi
  echo "Serial console exited (status=$status). Port missing; waiting to reconnect..."
  while true; do
    if [[ ! -e "$PORT" ]]; then
      sleep 0.5
      continue
    fi
    echo "Reopening ${PORT} @ ${BAUD}..."
    python3 -m serial.tools.miniterm "$PORT" "$BAUD" --raw
    status=$?
    if [[ -e "$PORT" ]]; then
      exit $status
    fi
    echo "Port dropped again; retrying..."
    sleep 0.5
  done
done
