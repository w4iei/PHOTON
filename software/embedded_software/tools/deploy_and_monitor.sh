#!/usr/bin/env bash
# deploy_circuitpy.sh — short waits, one reset, deploy files, then attach serial and stay open.

set -euo pipefail

############### Config ###############
MOUNT_DEFAULT="/Volumes/CIRCUITPY"
BAUD="${BAUD:-115200}"

MAX_WAIT_MOUNT_FIRST=3     # initial wait for CIRCUITPY
MAX_WAIT_MOUNT_AFTER=10    # wait after reset for mount
MAX_WAIT_SERIAL_FIND=5     # look for CDC port up to N seconds

SRC_DIR="${SRC_DIR:-"./src"}"
ASSETS_DIR="${ASSETS_DIR:-"./assets"}"
CONFIG_DIR="${CONFIG_DIR:-"./config"}"
MODE="${MODE:-dev}"          # dev|prod|prod_recovery
TAIL=1                       # default: attach serial at the end
PORT_OVERRIDE=""             # optional --port
MAIN_MARKER=0                # optional --main
SENSOR_NODE_ID=""            # optional --sensor_node_id N

STAMP="$(date '+%Y%m%d-%H%M%S')"
LOG_DIR="${LOG_DIR:-"./.deploy_logs"}"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/deploy-$STAMP.log"
REQS_FILE="${REQS_FILE:-"./requirements-circuitpy.txt"}"
RSYNC_FLAGS=(-rt --delete --modify-window=2 --no-perms --no-owner --no-group)
RSYNC_EXCLUDES=(--exclude "__pycache__/" --exclude "*.pyc" --exclude ".DS_Store" --exclude "._*" --exclude "rs485_sensor_node_cal.json")

############### Args ###############
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-tail) TAIL=0; shift ;;
    --tail)    TAIL=1; shift ;;
    --port)    PORT_OVERRIDE="$2"; shift 2 ;;
    --serial)  PORT_OVERRIDE="$2"; shift 2 ;;
    --main)    MAIN_MARKER=1; shift ;;
    --sensor_node_id) SENSOR_NODE_ID="$2"; shift 2 ;;
    --secondary_id) SENSOR_NODE_ID="$2"; warn "--secondary_id is deprecated; use --sensor_node_id"; shift 2 ;;
    *)         warn "Unknown arg: $1 (ignoring)"; shift ;;
  esac
done

############### Logging ###############
log()  { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"; }
ok()   { log "✔ $*"; }
warn() { log "⚠ $*"; }
fail() { log "✖ $*"; exit 1; }

############### Helpers ###############
detect_circuitpy_mount() {
  if [[ -n "${MOUNT:-}" && -d "$MOUNT" ]]; then echo "$MOUNT"; return; fi
  [[ -d "$MOUNT_DEFAULT" ]] && { echo "$MOUNT_DEFAULT"; return; }
  for m in /Volumes/CIRCUITPY*; do [[ -d "$m" ]] && { echo "$m"; return; }; done
  if [[ "$(uname -s)" == "Linux" ]]; then
    for m in /media/"$USER"/CIRCUITPY*; do [[ -d "$m" ]] && { echo "$m"; return; }; done
  fi
  echo ""
}

wait_for_circuitpy() {
  local secs="$1" t=0
  while [[ $t -lt $secs ]]; do
    local m; m="$(detect_circuitpy_mount)"
    [[ -n "$m" ]] && { echo "$m"; return 0; }
    sleep 0.5; t=$((t+1))
  done
  return 1
}

find_port() {
  if [[ -n "$PORT_OVERRIDE" && -e "$PORT_OVERRIDE" ]]; then echo "$PORT_OVERRIDE"; return; fi
  if [[ -n "${PORT:-}" && -e "$PORT" ]]; then echo "$PORT"; return; fi
  local p
  p="$(ls /dev/tty.usbmodem*  2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  p="$(ls /dev/tty.usbserial* 2>/dev/null | head -n 1 || true)"; [[ -n "$p" ]] && { echo "$p"; return; }
  echo ""
}

serial_break_reset_verbose() {
  local port="$1" baud="$2"
  [[ -n "$port" ]] || { warn "No serial port for reset"; return 1; }

  log "Attempting REPL break/reset on $port @ $baud ..."
  python3 - "$port" "$baud" <<'PY'
import sys, time
try:
    import serial
except Exception as e:
    print("pyserial not installed?", e); sys.exit(2)

port = sys.argv[1]; baud = int(sys.argv[2])
PROMPT = b">>> "

def read_for(ser, dur=0.2):
    t0=time.time(); buf=b""
    while time.time()-t0<dur:
        try:
            b=ser.read(256)
        except Exception as e:
            print("serial read failed:", e)
            break
        if b: buf+=b
        else: time.sleep(0.02)
    return buf

def read_until_prompt(ser, timeout=1.2):
    t0=time.time(); buf=b""
    while time.time()-t0<timeout:
        try:
            buf+=ser.read(256)
        except Exception as e:
            print("serial read failed:", e)
            return buf, False
        if PROMPT in buf: return buf, True
        time.sleep(0.02)
    return buf, False

def show(tag, data):
    if data:
        print(f"[SERIAL:{tag}] >>>", data.decode("utf-8","ignore").replace("\r","\\r").replace("\n","\\n"))
    else:
        print(f"[SERIAL:{tag}] (no output)")

try:
    ser = serial.Serial(port, baud, timeout=0.15)
except Exception as e:
    print("Could not open serial:", e); sys.exit(3)

try: ser.reset_input_buffer()
except Exception: pass

# Ctrl-C twice; wait for prompt
ser.write(b'\x03'); ser.flush(); time.sleep(0.10)
ser.write(b'\x03'); ser.flush(); time.sleep(0.10)
buf, ok = read_until_prompt(ser, 1.0)
show("after-ctrl-c", buf)
if not ok:
    ser.write(b'\x03'); ser.flush(); time.sleep(0.10)
    b2, ok2 = read_until_prompt(ser, 1.0)
    show("after-ctrl-c-2", b2)
    ok = ok2

# Import + reset with one retry if needed
def send(line, read_window=0.8):
    try:
        ser.write(line.encode("utf-8")+b"\r\n"); ser.flush()
    except Exception as e:
        print("serial write failed:", e)
        return b""
    time.sleep(0.05)
    out = read_for(ser, dur=read_window)
    return out

def send_no_read(line):
    try:
        ser.write(line.encode("utf-8")+b"\r\n"); ser.flush()
    except Exception as e:
        print("serial write failed:", e)
        return False
    return True

def parse_last_value(buf):
    try:
        text = buf.decode("utf-8", "ignore")
    except Exception:
        return None
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in reversed(lines):
        if line.startswith(">>>"):
            continue
        return line
    return None

out = send("import microcontroller", 0.8)
show("import-microcontroller", out)
if b"Traceback" in out or b"SyntaxError" in out or b"NameError" in out:
    ser.write(b'\x03'); ser.flush(); time.sleep(0.10)
    b3, _ok = read_until_prompt(ser, 0.8); show("retry-prompt", b3)
    out = send("import microcontroller", 0.8)
    show("import-retry", out)

out = send("import app.nvm_flags as nvm", 0.6)
show("import-nvm-flags", out)

flags_out = send("print(nvm.get_flags())", 0.3)
show("nvm-flags", flags_out)

disabled_out = send("print(nvm.is_usb_drive_disabled())", 0.3)
show("nvm-usb-disabled", disabled_out)
disabled_token = parse_last_value(disabled_out)
if disabled_token == "True":
    show("nvm-enable-usb", b"usb disk mode disabled; enabling")
    enable_out = send("print(nvm.set_usb_drive_disabled(False))", 0.4)
    show("nvm-set-usb", enable_out)
    disabled_out = send("print(nvm.is_usb_drive_disabled())", 0.3)
    show("nvm-usb-disabled", disabled_out)
else:
    show("nvm-enable-usb", b"usb disk mode already enabled")
time.sleep(0.3)

send_no_read("microcontroller.reset()")
show("after-reset-cmd", b"reset sent")

try: ser.close()
except Exception: pass
print("Sent break/reload/reset sequence.")
sys.exit(0)
PY
}

copy_root_file() {
  local src="$1" dst="$2"
  [[ -f "$src" ]] || return 0
  if [[ -f "$dst" ]] && cmp -s "$src" "$dst"; then
    log "Unchanged $(basename "$src") → /$(basename "$dst") (skipping copy)"
    return 0
  fi
  cp -f "$src" "$dst"
  log "Copied $(basename "$src") → /$(basename "$dst")"
}
sync_dir_to_mount() {
  local src="$1" name; [[ -d "$src" ]] || return 0
  name="$(basename "$src")"
  mkdir -p "$MOUNT/$name"
  rsync "${RSYNC_FLAGS[@]}" "${RSYNC_EXCLUDES[@]}" "$src"/ "$MOUNT/$name"/ | tee -a "$LOG_FILE"
}
install_circuitpy_libs() {
  local req_file="$1"
  [[ -f "$req_file" ]] || { warn "No requirements file at $req_file; skipping CircuitPython libs install"; return; }
  command -v circup >/dev/null || { warn "circup not installed; skipping CircuitPython libs install"; return; }
  log "Installing CircuitPython libs from $req_file into $MOUNT via circup ..."
  if ! CIRCUITPY="$MOUNT" circup install -r "$req_file" | tee -a "$LOG_FILE"; then
    warn "circup install returned non-zero; check log for details"
  fi
}
ensure_mount_dir() {
  local dir="$1"
  if [[ -d "$dir" ]]; then return; fi
  if mkdir -p "$dir" 2>/dev/null; then
    log "Created $(basename "$dir") at $(dirname "$dir")"
  else
    warn "Could not create $dir (read-only?)"
  fi
}

############### Preflight ###############
command -v python3 >/dev/null || fail "python3 not found"
[[ -d "$SRC_DIR" ]] || fail "Missing src dir: $SRC_DIR"

############### Mount / Reset flow ###############
log "Waiting up to ${MAX_WAIT_MOUNT_FIRST}s for CIRCUITPY to mount..."
MOUNT="$(wait_for_circuitpy "$MAX_WAIT_MOUNT_FIRST" || true)"

if [[ -z "$MOUNT" ]]; then
  warn "CIRCUITPY not mounted; trying ONE serial break/reset..."
  PORT="$(find_port)"
  if [[ -z "$PORT" ]]; then
    for _ in $(seq 1 "$MAX_WAIT_SERIAL_FIND"); do
      PORT="$(find_port)"; [[ -n "$PORT" ]] && break; sleep 1
    done
  fi
  if [[ -n "$PORT" ]]; then
    serial_break_reset_verbose "$PORT" "$BAUD" || warn "Serial reset failed to run"
  else
    warn "No USB CDC port found to send reset"
  fi

  log "Waiting up to ${MAX_WAIT_MOUNT_AFTER}s for CIRCUITPY after reset..."
  MOUNT="$(wait_for_circuitpy "$MAX_WAIT_MOUNT_AFTER" || true)"
  if [[ -z "$MOUNT" && "$(uname -s)" == "Darwin" ]]; then
    log "Trying 'diskutil mount CIRCUITPY' ..."
    diskutil mount CIRCUITPY || true
    MOUNT="$(detect_circuitpy_mount)"
  fi
fi

[[ -n "$MOUNT" ]] || fail "CIRCUITPY not mounted. (check cable, boot.py MSC, power)"
ok "Using CIRCUITPY at: $MOUNT"

# Write test
touch "$MOUNT/.write_test" 2>/dev/null || fail "Cannot write to $MOUNT (MSC disabled?)"
rm -f "$MOUNT/.write_test" || true

# Ensure /sd exists so runtime can mount the TF card even if CIRCUITPY is later read-only.
ensure_mount_dir "$MOUNT/sd"

# Install CircuitPython libs (if circup + requirements-circuitpy.txt present)
install_circuitpy_libs "$REQS_FILE"

############### Deploy ###############
log "Deploying from $SRC_DIR → $MOUNT ..."
copy_root_file "$SRC_DIR/code.py" "$MOUNT/code.py"

case "$MODE" in
  dev)           BOOT_SRC="$SRC_DIR/boot_dev.py" ;;
  prod)          BOOT_SRC="$SRC_DIR/boot_prod.py" ;;
  prod_recovery) BOOT_SRC="$SRC_DIR/boot_prod_recovery.py" ;;
  *)             BOOT_SRC="$SRC_DIR/boot.py" ;;
esac
if [[ -f "$BOOT_SRC" ]]; then
  copy_root_file "$BOOT_SRC" "$MOUNT/boot.py"
elif [[ -f "$SRC_DIR/boot.py" ]]; then
  copy_root_file "$SRC_DIR/boot.py" "$MOUNT/boot.py"
fi

shopt -s nullglob
for f in "$SRC_DIR"/*.py "$SRC_DIR"/*.mpy; do
  base="$(basename "$f")"
  [[ "$base" == "code.py" || "$base" == "boot.py" ]] && continue
  copy_root_file "$f" "$MOUNT/$base"
done
shopt -u nullglob

[[ -d "$SRC_DIR/app" ]]  && sync_dir_to_mount "$SRC_DIR/app"  || true
[[ -d "$ASSETS_DIR" ]]   && sync_dir_to_mount "$ASSETS_DIR"   || true
[[ -d "$CONFIG_DIR" ]]   && sync_dir_to_mount "$CONFIG_DIR"   || true

if [[ "$MAIN_MARKER" -eq 1 ]]; then
  log "--main flag detected; creating /main on CIRCUITPY."
  if touch "$MOUNT/main" 2>/dev/null; then
    if [[ -f "$MOUNT/main" ]]; then
      log "Touched /main marker on CIRCUITPY."
    else
      warn "Touch reported success but /main is missing on CIRCUITPY."
    fi
  else
    warn "Failed to touch /main marker on CIRCUITPY."
  fi
fi

if [[ -n "$SENSOR_NODE_ID" ]]; then
  if [[ -f "$MOUNT/main" ]]; then
    log "--sensor_node_id detected; removing /main marker on CIRCUITPY."
    if rm -f "$MOUNT/main" 2>/dev/null; then
      if [[ -f "$MOUNT/main" ]]; then
        warn "rm reported success but /main still exists on CIRCUITPY."
      else
        log "Removed /main marker on CIRCUITPY."
      fi
    else
      warn "Failed to remove /main marker on CIRCUITPY."
    fi
  fi
  if [[ -f "$MOUNT/secondary_sensor" ]]; then
    log "--sensor_node_id detected; removing legacy /secondary_sensor marker."
    rm -f "$MOUNT/secondary_sensor" || warn "Failed to remove /secondary_sensor on CIRCUITPY."
  fi
  log "--sensor_node_id detected; creating /sensor_node_id on CIRCUITPY."
  log "--sensor_node_id detected; updating /config/rs485_sensor_node.json."
  python3 - "$MOUNT/config/rs485_sensor_node.json" "$SENSOR_NODE_ID" <<'PY' 2>&1 | tee -a "$LOG_FILE"
import json
import os
import sys

path = sys.argv[1]
device_id = int(sys.argv[2])

data = {}
if os.path.exists(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except Exception:
        data = {}

data["device_id"] = device_id
data["left_term"] = bool(device_id == 1)
data["log_event_details"] = bool(device_id == 4)
if device_id in (2, 4):
    data["disabled_sensors"] = [30]
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
print(
    f"Updated {path} device_id={device_id} left_term={data['left_term']} "
    f"log_event_details={data['log_event_details']}"
)
PY
  if printf "%s" "$SENSOR_NODE_ID" > "$MOUNT/sensor_node_id" 2>/dev/null; then
    if [[ -f "$MOUNT/sensor_node_id" ]]; then
      log "Wrote /sensor_node_id with value '$SENSOR_NODE_ID'."
    else
      warn "Write reported success but /sensor_node_id is missing on CIRCUITPY."
    fi
  else
    warn "Failed to write /sensor_node_id on CIRCUITPY."
  fi
fi

ok "Deploy complete."

############### Soft reload + Tail ###############
PORT="$(find_port || true)"
if [[ -n "$PORT" ]]; then
  log "Sending soft reload (Ctrl-C/D) on $PORT @ $BAUD ..."
  python3 - "$PORT" "$BAUD" <<'PY' 2>&1 | sed 's/^/[SOFT]/' | tee -a "$LOG_FILE"
import sys, time
try:
    import serial
except Exception as e:
    print("pyserial not installed?", e); sys.exit(0)
port = sys.argv[1]; baud = int(sys.argv[2])
try:
    ser = serial.Serial(port, baud, timeout=0.5)
    time.sleep(0.12)
    ser.write(b'\x03'); time.sleep(0.12)  # Ctrl-C
    ser.write(b'\x04'); time.sleep(0.12)  # Ctrl-D
    t0 = time.time(); out=b""
    while time.time() - t0 < 1.0:
        out += ser.read(256)
    if out: print(out.decode("utf-8","ignore"))
    ser.close()
except Exception as e:
    print("Soft reload failed:", e)
PY

  if [[ "$TAIL" -eq 1 ]]; then
    log "Attaching serial console (press Ctrl-] to exit miniterm)..."
    cleanup() { stty sane || true; }
    trap cleanup EXIT
    while true; do
      log "Opening serial console on $PORT @ $BAUD ..."
      python3 -m serial.tools.miniterm "$PORT" "$BAUD" --raw
      status=$?
      if [[ -e "$PORT" ]]; then
        exit $status
      fi
      warn "Serial console exited (status=$status). Port missing; retrying for 5s..."
      deadline=$(( $(date +%s) + 5 ))
      while (( $(date +%s) < deadline )); do
        if [[ ! -e "$PORT" ]]; then
          sleep 0.5
          continue
        fi
        log "Reopening serial console on $PORT @ $BAUD ..."
        python3 -m serial.tools.miniterm "$PORT" "$BAUD" --raw
        status=$?
        if [[ $status -eq 0 ]]; then
          exit 0
        fi
        sleep 0.5
      done
      warn "Reconnect timed out."
      exit $status
    done
  else
    ok "Done (no tail requested)."
  fi
else
  warn "No CDC port found for soft reload or tail."
fi
