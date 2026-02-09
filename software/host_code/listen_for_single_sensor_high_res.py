#!/usr/bin/env python3
import argparse, logging, re, time, sys, os, datetime
import serial
import numpy as np
import matplotlib.pyplot as plt

BEGIN_RE = re.compile(
    r"^BEGIN_TRACE\s+sensor=(\d+)\s+midi=(\S+)\s+note=(\S+)\s+polarity=(\S+)\s+thr_on=(\d+)\s+thr_off=(\d+)"
)
# Lines starting with "#" are logs from the device; data lines are "t_ns,adc"
END_MARK = "END_TRACE"

def parse_args():
    p = argparse.ArgumentParser(
        description="Read oscilloscope trace from CircuitPython over USB serial and plot."
    )
    p.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem101")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--timeout", type=float, default=6.0,
        help="Seconds to wait for END_TRACE after BEGIN_TRACE"
    )
    p.add_argument(
        "--pre-timeout", type=float, default=0.0,
        help="Optional seconds to wait for BEGIN_TRACE (0 = no limit)"
    )
    p.add_argument("--csv", help="Optional path to save CSV of the captured trace (overrides auto name)")
    p.add_argument("--png", help="Optional path to save PNG plot")
    p.add_argument("--show", action="store_true", help="Show interactive plot")
    p.add_argument("--log", default="INFO", help="Logging level (DEBUG, INFO, WARNING, ERROR)")
    p.add_argument("--outdir", default=".", help="Directory for auto-saved CSV if --csv is not provided")
    return p.parse_args()

def setup_logging(level: str):
    lvl = getattr(logging, level.upper(), logging.INFO)
    logging.basicConfig(
        level=lvl,
        format="%(asctime)s.%(msecs)03d %(levelname)s: %(message)s",
        datefmt="%H:%M:%S"
    )

def auto_csv_path(outdir: str, meta: dict | None) -> str:
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    if meta:
        sensor = meta.get("sensor", "NA")
        note = meta.get("note", "NA")
        midi = meta.get("midi", "NA")
        base = f"trace_{ts}_sensor-{sensor}_note-{note}_midi-{midi}.csv"
    else:
        base = f"trace_{ts}.csv"
    return os.path.join(outdir, base)

def main():
    args = parse_args()
    setup_logging(args.log)

    logging.info("Opening serial: %s @ %d", args.port, args.baud)
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.1)
    try:
        ser.write(b"\n")
        ser.flush()
        logging.info("Triggered capture via newline.")
    except Exception as exc:
        logging.warning("Failed to send capture trigger: %s", exc)

    meta = None
    ts, vs = [], []
    ts_are_seconds = None
    started = False
    begin_seen_at = None
    pre_started_at = time.time()

    try:
        logging.info("Waiting for trace... printing all device logs.")
        while True:
            # Optional pre-trace timeout (off by default)
            if not started and args.pre_timeout and args.pre_timeout > 0:
                if (time.time() - pre_started_at) > args.pre_timeout:
                    logging.error("Pre-trace timeout: BEGIN_TRACE not received.")
                    sys.exit(1)

            # In-trace timeout (after BEGIN_TRACE)
            if started and args.timeout > 0 and begin_seen_at is not None:
                if (time.time() - begin_seen_at) > args.timeout:
                    logging.error("In-trace timeout: END_TRACE not received.")
                    break

            line = ser.readline().decode("utf-8", "ignore").strip()
            if not line:
                continue

            # ----- DEVICE LOGS / DEBUG (always print) -----
            if line.startswith("# LOG"):
                logging.info("DEVICE: %s", line[5:].strip())
                continue

            if line.startswith("#"):
                # other comments/warnings
                logging.warning("DEVICE: %s", line[1:].strip())
                continue

            # ----- BEGIN TRACE -----
            if line.startswith("BEGIN_TRACE"):
                m = BEGIN_RE.match(line)
                if not m:
                    logging.error("Malformed BEGIN_TRACE line: %r", line)
                    # Don't start; keep listening
                    continue

                sensor, midi, note, pol, thr_on, thr_off = m.groups()
                meta = {
                    "sensor": int(sensor),
                    "midi": None if midi == "None" else int(midi),
                    "note": note,
                    "polarity": pol,
                    "thr_on": int(thr_on),
                    "thr_off": int(thr_off),
                }
                logging.info(
                    "BEGIN_TRACE: sensor=%s note=%s midi=%s pol=%s on=%s off=%s",
                    meta["sensor"], meta["note"], str(meta["midi"]),
                    meta["polarity"], meta["thr_on"], meta["thr_off"]
                )
                ts.clear(); vs.clear()
                started = True
                begin_seen_at = time.time()
                continue

            # ----- END TRACE -----
            if line == END_MARK:
                if not started:
                    logging.warning("END_TRACE received before BEGIN_TRACE; ignoring")
                    continue
                logging.info("END_TRACE received. Samples: %d", len(ts))
                break

            # ----- TRACE DATA -----
            if started:
                try:
                    t_str, v_str = line.split(",", 1)
                    if "." in t_str or "e" in t_str or "E" in t_str:
                        t_val = float(t_str)
                        if ts_are_seconds is None:
                            ts_are_seconds = True
                    else:
                        if ts_are_seconds:
                            t_val = float(t_str)
                        else:
                            t_val = int(t_str)
                            if ts_are_seconds is None:
                                ts_are_seconds = False
                    ts.append(t_val)
                    vs.append(int(v_str))
                except Exception:
                    logging.debug("Skipping malformed data line: %r", line)
                continue

            # ----- Everything else before BEGIN_TRACE -----
            logging.debug("DEVICE(outside-trace): %s", line)

    finally:
        ser.close()

    if not ts:
        logging.error("No samples captured.")
        sys.exit(2)

    # Convert to arrays and compute stats
    if ts_are_seconds:
        t = np.array(ts, dtype=float)
    else:
        t = (np.array(ts) - ts[0]) * 1e-9  # seconds from first sample
    v = np.array(vs, dtype=np.int32)
    dur = float(t[-1]) if len(t) > 1 else 0.0
    est_fs = (len(t) - 1) / dur if dur > 0 else 0.0

    # Temporal resolution details
    if len(t) > 1:
        dt = np.diff(t)
        dt_mean   = float(np.mean(dt))
        dt_median = float(np.median(dt))
        dt_min    = float(np.min(dt))
        dt_max    = float(np.max(dt))
        dt_std    = float(np.std(dt))
        fs_inst   = 1.0 / dt
        fs_mean   = float(np.mean(fs_inst))
        fs_median = float(np.median(fs_inst))
        logging.info(
            "Temporal resolution: est_fs=%.1f samples/s | dt_mean=%.6e s dt_median=%.6e s dt_min=%.6e s dt_max=%.6e s dt_std=%.6e s | fs_mean=%.1f fs_median=%.1f",
            est_fs, dt_mean, dt_median, dt_min, dt_max, dt_std, fs_mean, fs_median
        )
    else:
        logging.info("Temporal resolution: insufficient samples for dt/fs stats.")

    logging.info("Duration: %.6f s | Samples: %d", dur, len(v))
    logging.info("ADC min=%d max=%d range=%d", int(v.min()), int(v.max()), int(v.max() - v.min()))

    # ---- Save CSV automatically (t and z) ----
    csv_path = args.csv if args.csv else auto_csv_path(args.outdir, meta)
    try:
        os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    except Exception:
        pass  # ok if current dir

    logging.info("Saving CSV: %s", csv_path)
    with open(csv_path, "w") as f:
        f.write("# t_s,z\n")
        for _t, _v in zip(t, v):
            f.write(f"{_t:.9f},{_v}\n")

    # Plot
    title = "Trace"
    if meta:
        title = f"Sensor {meta['sensor']}  Note {meta['note']}  MIDI {meta['midi']}"
    plt.figure()
    plt.plot(t, v, linewidth=1.0)
    plt.title(title)
    plt.xlabel("Time (s)")
    plt.ylabel("ADC value")
    plt.grid(True)
    plt.tight_layout()

    if args.png:
        logging.info("Saving PNG: %s", args.png)
        plt.savefig(args.png, dpi=350)

    if args.show or not args.png:
        plt.show()

if __name__ == "__main__":
    main()
