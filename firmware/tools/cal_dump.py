#!/usr/bin/env python3
"""Save a knee calibration's captured swings and its before/after table.

After calibrating ('cal reset 1 2', slow-press every key, 'cal save 1 2'),
every key's last full swing is still on its board. This asks the console for
them ('cal dump <board>') and for the before/after table ('cal compare
<board>'), and writes one directory per run:

    board1_key00_F1.csv    sample, t_ms, value (raw ADC) of one swing
    ...
    swings.csv             one row per key: status, knee sample/value/%, rest, top
    compare_board1.txt     the before/after table as printed
    swings_board1.png      every swing with its knee marked (--plot)

    python3 tools/cal_dump.py --port /dev/cu.usbmodem101 --boards 1 2 --plot

Works on the bridge's console (any board) and on a sensor board's own
console (its own id). The console must not be held by another program; with
the broker running, point --port at nothing and use --tcp instead.
"""
import argparse
import csv
import datetime
import os
import re
import socket
import sys
import time

KV = re.compile(r"(\w+)=(\S+)")


class Console:
    def __init__(self, port=None, tcp=None):
        self.buf = b""
        if tcp:
            host, _, p = tcp.partition(":")
            self.sock = socket.create_connection((host or "localhost", int(p or 7777)))
            self.sock.settimeout(0.2)
            self.ser = None
        else:
            import serial  # pyserial

            self.ser = serial.Serial(port, 115200, timeout=0.2)
            self.sock = None

    def send(self, line):
        data = (line + "\r").encode()
        if self.ser:
            self.ser.write(data)
        else:
            self.sock.sendall(data)

    def _read(self):
        if self.ser:
            return self.ser.read(4096)
        try:
            return self.sock.recv(4096)
        except socket.timeout:
            return b""

    def lines(self, timeout):
        """Yield complete lines until `timeout` seconds pass without any."""
        last = time.time()
        while time.time() - last < timeout:
            chunk = self._read()
            if chunk:
                last = time.time()
                self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line.decode(errors="replace")).rstrip()


def dump_board(con, board, out, timeout):
    con.send(f"cal dump {board}")
    swings, cur, samples = [], None, []
    for line in con.lines(timeout):
        if line.startswith("BEGIN_SWING"):
            cur, samples = dict(KV.findall(line)), []
        elif line == "END_SWING" and cur is not None:
            cur["samples"] = samples
            swings.append(cur)
            cur = None
        elif cur is not None and re.fullmatch(r"\d+,\d+", line):
            i, v = line.split(",")
            samples.append((int(i), int(v)))
        elif line.startswith("#") and "no swing captured" in line:
            print("  " + line.lstrip("# "))
        elif line.startswith(f"END_DUMP board={board}"):
            break
    else:
        print(f"  board {board}: dump did not finish (timeout); saving what arrived")
    for s in swings:
        period_ms = int(s["period_us"]) / 1000.0
        name = f"board{board}_key{int(s['key']):02d}_{s['note'].replace('#', 's')}.csv"
        with open(os.path.join(out, name), "w", newline="") as f:
            f.write("# " + " ".join(f"{k}={v}" for k, v in s.items() if k != "samples") + "\n")
            w = csv.writer(f)
            w.writerow(["sample", "t_ms", "value"])
            for i, v in s["samples"]:
                w.writerow([i, f"{i * period_ms:.3f}", v])
    return swings


def compare_board(con, board, out, timeout):
    con.send(f"cal compare {board}")
    text = []
    for line in con.lines(timeout):
        if line.startswith("== board") or text:
            text.append(line)
        if line.startswith(f"END_COMPARE board={board}"):
            break
    if text:
        with open(os.path.join(out, f"compare_board{board}.txt"), "w") as f:
            f.write("\n".join(text) + "\n")
        print("\n".join(text))
    else:
        print(f"  board {board}: no before/after table (timeout)")


def plot(board, swings, out):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if not swings:
        return
    cols = 6
    rows = (len(swings) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 2.6, rows * 1.9), squeeze=False)
    for ax in axes.flat:
        ax.axis("off")
    for ax, s in zip(axes.flat, swings):
        ax.axis("on")
        period_ms = int(s["period_us"]) / 1000.0
        t = [i * period_ms for i, _ in s["samples"]]
        v = [x for _, x in s["samples"]]
        ax.plot(t, v, lw=0.8)
        if s.get("knee") == "1":
            k = int(s["knee_idx"])
            ax.axvline(k * period_ms, color="tab:green", lw=0.8)
        ax.set_title(f"{s['key']} {s['note']} {s['status']}", fontsize=7)
        ax.tick_params(labelsize=6)
    fig.suptitle(f"board {board}: last full swing per key (green = knee)", fontsize=9)
    fig.tight_layout()
    path = os.path.join(out, f"swings_board{board}.png")
    fig.savefig(path, dpi=110)
    print(f"  wrote {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="console serial port, e.g. /dev/cu.usbmodem101")
    ap.add_argument("--tcp", help="console broker instead of a port, e.g. localhost:7777")
    ap.add_argument("--boards", type=int, nargs="+", required=True, help="board ids")
    ap.add_argument("--out", help="output directory (default: cal_capture_<time>)")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="seconds of console silence that end a step")
    ap.add_argument("--plot", action="store_true", help="also save a PNG per board")
    args = ap.parse_args()
    if not args.port and not args.tcp:
        ap.error("give --port or --tcp")

    out = args.out or "cal_capture_" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    os.makedirs(out, exist_ok=True)
    con = Console(args.port, args.tcp)
    con.send("cal view off")
    time.sleep(0.3)
    list(con.lines(0.3))  # drop whatever was on the screen

    summary = []
    for b in args.boards:
        print(f"board {b}: swings")
        swings = dump_board(con, b, out, args.timeout)
        print(f"  {len(swings)} swings saved")
        for s in swings:
            summary.append({"board": b, **{k: v for k, v in s.items() if k != "samples"}})
        print(f"board {b}: before/after")
        compare_board(con, b, out, args.timeout)
        if args.plot:
            plot(b, swings, out)
    if summary:
        keys = list(summary[0].keys())
        with open(os.path.join(out, "swings.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            w.writerows(summary)
    print(f"saved in {out}/")


if __name__ == "__main__":
    sys.exit(main())
