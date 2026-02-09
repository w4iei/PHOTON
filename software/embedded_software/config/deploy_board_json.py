#!/usr/bin/env python3
# config/merge_board_config.py
# Merge defaults + per-board JSON → active.json and validate against your schema.

import argparse, csv, json, re, sys
from pathlib import Path

# ---------- Paths (relative to repo root) ----------
DEFAULTS_PATH = Path("config/defaults.json")
BOARDS_DIR    = Path("config/boards")
OUT_PATH      = Path("config/active.json")

# ---------- Merge ----------
def deep_merge(a, b):
    if isinstance(a, dict) and isinstance(b, dict):
        out = dict(a)
        for k, v in b.items():
            out[k] = deep_merge(a.get(k), v)
        return out
    return b if b is not None else a

# ---------- Resolve board file ----------
def find_board_json_by_id(board_id: str) -> Path | None:
    p = BOARDS_DIR / f"{board_id}.json"
    return p if p.exists() else None


# ---------- Validation (tailored to your schema) ----------
# Allow GP0..GP47 (RP2350 has up to 48 GPIOs), A0..A15 (you use up to A7), and LED
_GP_PAT = r"GP([0-9]|[1-3][0-9]|4[0-7])"
_A_PAT  = r"A([0-9]|1[0-5])"
_PIN_RE = re.compile(rf"^({_GP_PAT}|{_A_PAT}|LED)$", re.IGNORECASE)

def _ok_pin(s: str) -> bool:
    return isinstance(s, str) and bool(_PIN_RE.match(s.strip()))

def _require_keys(obj: dict, keys: list[str], ctx: str):
    for k in keys:
        if k not in obj:
            raise SystemExit(f"{ctx}: missing required key '{k}'")

def _validate_manual(name: str, m: dict, base_low_f: int, require_len: int, allow_index1_minus1: bool):
    if not isinstance(m, dict):
        raise SystemExit(f"{name}: must be an object")
    _require_keys(m, ["midi_channel", "adc_lines", "midi_map"], name)

    # midi_channel
    ch = m["midi_channel"]
    if not isinstance(ch, int) or not (0 <= ch <= 15):
        raise SystemExit(f"{name}.midi_channel must be int in 0..15 (got {ch!r})")

    # adc_lines: exactly 4 pins
    adc = m["adc_lines"]
    if not (isinstance(adc, list) and len(adc) == 4 and all(_ok_pin(p) for p in adc)):
        raise SystemExit(f"{name}.adc_lines must be a list of 4 valid pin names (GPxx/Ax)")

    # midi_map: exactly 64 ints; index 1 is 29 (low F) or -1 (if you chose to allow)
    mm = m["midi_map"]
    if not isinstance(mm, list):
        raise SystemExit(f"{name}.midi_map must be a list")
    if len(mm) != require_len:
        raise SystemExit(f"{name}.midi_map must have {require_len} entries (got {len(mm)})")
    for i, v in enumerate(mm):
        if not isinstance(v, int):
            raise SystemExit(f"{name}.midi_map[{i}] must be int (use -1 for unused)")
    if allow_index1_minus1:
        if mm[1] not in (-1, base_low_f):
            raise SystemExit(f"{name}.midi_map[1] must be {base_low_f} or -1 (got {mm[1]})")
    else:
        if mm[1] != base_low_f:
            raise SystemExit(f"{name}.midi_map[1] must be {base_low_f} (got {mm[1]})")

def validate_config(cfg: dict, *, base_low_f: int = 29, require_len: int = 64, allow_index1_minus1: bool = False):
    if not isinstance(cfg, dict):
        raise SystemExit("Top-level JSON must be an object")

    # MCU
    if "MCU" not in cfg or cfg["MCU"] not in ("pico2", "pico2_w"):
        raise SystemExit("MCU must be either 'pico2' or 'pico2_w'")

    # manuals (flat structure: manual1, manual2 at top level)
    _require_keys(cfg, ["manual1", "manual2"], "root")
    _validate_manual("manual1", cfg["manual1"], base_low_f, require_len, allow_index1_minus1)
    _validate_manual("manual2", cfg["manual2"], base_low_f, require_len, allow_index1_minus1)

    # addressing_gpios: required, with fixed keys
    if "addressing_gpios" not in cfg or not isinstance(cfg["addressing_gpios"], dict):
        raise SystemExit("addressing_gpios must be an object")
    addr = cfg["addressing_gpios"]
    _require_keys(addr, ["Y0_SEL_0", "Y0_SEL_1", "Y0_SEL_2", "Y0_SEL_3"], "addressing_gpios")
    for k, v in addr.items():
        if not _ok_pin(v):
            raise SystemExit(f"addressing_gpios.{k} has invalid pin '{v}'")

    # digital_sensors: optional list of {name,en_pin,read_pin}
    ds = cfg.get("digital_sensors", [])
    if ds is not None:
        if not isinstance(ds, list):
            raise SystemExit("digital_sensors must be a list if present")
        for i, s in enumerate(ds):
            if not isinstance(s, dict):
                raise SystemExit(f"digital_sensors[{i}] must be an object")
            _require_keys(s, ["name", "en_pin", "read_pin"], f"digital_sensors[{i}]")
            if not _ok_pin(s["en_pin"]) or not _ok_pin(s["read_pin"]):
                raise SystemExit(f"digital_sensors[{i}] has invalid pins: en_pin={s['en_pin']!r}, read_pin={s['read_pin']!r}")

# ---------- Main ----------
def main():
    ap = argparse.ArgumentParser(description="Merge defaults + per-board JSON and validate.")
    ap.add_argument("-b", "--board", help="Board ID (e.g., 0000)", required=True)
    ap.add_argument("--defaults", default=str(DEFAULTS_PATH), help="Path to defaults.json")
    ap.add_argument("--boards-dir", default=str(BOARDS_DIR), help="Directory with per-board JSON files")
    ap.add_argument("-o", "--out", default=str(OUT_PATH), help="Output merged JSON")
    ap.add_argument("--allow-index1-minus1", action="store_true", help="Permit midi_map[1] == -1")
    ap.add_argument("--base-low-f", type=int, default=29, help="Expected MIDI note for low F (default: 29)")
    args = ap.parse_args()

    defaults_path = Path(args.defaults)
    boards_dir    = Path(args.boards_dir)
    out_path      = Path(args.out)

    if not defaults_path.exists():
        raise SystemExit(f"Missing defaults file: {defaults_path}")
    if not boards_dir.exists():
        raise SystemExit(f"Missing boards dir: {boards_dir}")

    # Resolve per-board JSON
    board_json = find_board_json_by_id(args.board)
    if board_json is None:
        raise SystemExit(f"Could not find board JSON for id {args.board!r} under {boards_dir}/")


    # Load + merge
    with defaults_path.open("r", encoding="utf-8") as f:
        defaults = json.load(f)
    with board_json.open("r", encoding="utf-8") as f:
        per_board = json.load(f)

    merged = deep_merge(defaults, per_board)

    # Ensure midi_map arrays exist after merge (board file can fill them)
    for mkey in ("manual1", "manual2"):
        m = merged.get(mkey, {})
        if "midi_map" not in m or not isinstance(m["midi_map"], list) or len(m["midi_map"]) == 0:
            raise SystemExit(f"{mkey}.midi_map must be provided by per-board JSON and non-empty")

    # Validate against your schema
    validate_config(
        merged,
        base_low_f=args.base_low_f,
        require_len=64,
        allow_index1_minus1=args.allow_index1_minus1,
    )

    # Write output
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2, ensure_ascii=False, sort_keys=True)
    print(f"OK: wrote merged config → {out_path}")

if __name__ == "__main__":
    main()