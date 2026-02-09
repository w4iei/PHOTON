#!/usr/bin/env bash
set -euo pipefail

# Break into CircuitPython REPL, set next reset to ROM bootloader, then reset.

BAUD="${BAUD:-115200}"
PORT_OVERRIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|--serial) PORT_OVERRIDE="$2"; shift 2 ;;
    --baud) BAUD="$2"; shift 2 ;;
    *) echo "Usage: $0 [--port /dev/tty...] [--baud 115200]"; exit 1 ;;
  esac
done

find_port() {
  if [[ -n "$PORT_OVERRIDE" && -e "$PORT_OVERRIDE" ]]; then echo "$PORT_OVERRIDE"; return; fi
  if [[ -n "${PORT:-}" && -e "$PORT" ]]; then echo "$PORT"; return; fi
  local p
  p="$(ls /dev/tty.usbmodem* 2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/cu.usbmodem*  2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/tty.usbserial* 2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/ttyACM*       2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/ttyUSB*       2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  echo ""
}

PORT="$(find_port)"
if [[ -z "$PORT" ]]; then
  echo "No serial port found. Use --port /dev/tty.usbmodemXXXX"
  exit 1
fi

python3 - "$PORT" "$BAUD" <<'PY'
import sys
import time

try:
    import serial
except Exception as exc:
    print("pyserial not installed?", exc)
    sys.exit(2)

port = sys.argv[1]
baud = int(sys.argv[2])
PROMPT = b">>> "

def read_for(ser, dur=0.25):
    t0 = time.time()
    buf = b""
    while time.time() - t0 < dur:
        try:
            chunk = ser.read(256)
        except Exception:
            break
        if chunk:
            buf += chunk
        else:
            time.sleep(0.02)
    return buf

def has_error(buf):
    return b"Traceback" in buf or b"Error" in buf or b"NameError" in buf or b"AttributeError" in buf

def show(tag, buf):
    if not buf:
        print(f"[{tag}] (no output)")
        return
    text = buf.decode("utf-8", "ignore").replace("\r", "\\r").replace("\n", "\\n")
    print(f"[{tag}] {text}")

ser = serial.Serial(port, baud, timeout=0.15)
try:
    ser.reset_input_buffer()
except Exception:
    pass

# Some boards need multiple breaks or a soft reload to land in the prompt.
got_prompt = False
for attempt in range(5):
    ser.write(b"\x03")
    ser.flush()
    time.sleep(0.10)
    ser.write(b"\x03")
    ser.flush()
    time.sleep(0.10)
    out = read_for(ser, dur=0.4)
    show(f"break-{attempt+1}", out)
    if PROMPT in out or b"KeyboardInterrupt" in out:
        got_prompt = True
        break
    ser.write(b"\r\n")
    ser.flush()
    time.sleep(0.05)
    out = read_for(ser, dur=0.3)
    show(f"newline-{attempt+1}", out)
    if PROMPT in out:
        got_prompt = True
        break
    ser.write(b"\x04")  # soft reload can revive the REPL on some boards
    ser.flush()
    time.sleep(0.20)
    out = read_for(ser, dur=0.6)
    show(f"reload-{attempt+1}", out)
    if PROMPT in out:
        got_prompt = True
        break

if not got_prompt:
    print("No REPL prompt detected; continuing anyway.")

def send(line, wait=0.6):
    ser.write(line.encode("utf-8") + b"\r\n")
    ser.flush()
    time.sleep(0.05)
    return read_for(ser, dur=wait)

out = send("import microcontroller", wait=0.6)
show("import", out)
if has_error(out):
    print("Failed to import microcontroller.")
    ser.close()
    sys.exit(5)

cmds = [
    "microcontroller.on_next_reset(microcontroller.RunMode.BOOTLOADER)",
    "microcontroller.on_next_reset(microcontroller.RunMode.UF2)",
    "microcontroller.bootloader()",
]
for cmd in cmds:
    out = send(cmd, wait=0.6)
    show("set-bootloader", out)
    if not has_error(out):
        break
else:
    print("Failed to set bootloader mode.")
    ser.close()
    sys.exit(6)

try:
    send("microcontroller.reset()", wait=0.2)
except Exception:
    pass
print("Reset sent; device should re-enumerate as RP2350 bootloader.")
try:
    ser.close()
except Exception:
    pass
PY
