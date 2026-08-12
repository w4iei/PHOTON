#!/usr/bin/env python3
"""Timed RS-485 throughput/loss measurement, run against the bridge console.

Samples the bridge's per-node and bus counters with host timestamps, drives
the broadcast pseudorandom load generator, and prints per-phase rates so
successive transport-tuning builds can be compared number-to-number:

    python3 measure.py --port /dev/cu.usbmodemXXXX --label baseline \
        [--rate 100] [--idle-s 10] [--load-s 30]

Phases: idle window -> broadcast `test <rate>` -> load window -> `test stop`
-> halt check.  All rates are computed from counter deltas / host wall time,
so smoke-style command latency does not skew them.
"""
import argparse
import re
import time

import serial

NODE_RE = re.compile(
    r"node (\d+): alive polls=(\d+) events=(\d+) dup=(\d+) gaps=(\d+) "
    r"malformed=(\d+) retries=(\d+) timeouts=(\d+)")
BUS_RE = re.compile(
    r"bus tx=(\d+) rx=(\d+) crc_err=(\d+) hdr_err=(\d+) cmd_drops=(\d+)")
CYC_RE = re.compile(r"poll_cycles=(\d+)")


def command(ser, cmd, settle_s=0.6, cap_s=3.0):
    # cap_s bounds the whole read: with the bridge's [EVT] logging on, the
    # stream never goes quiet under load and a pure quiet-detect loop hangs.
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    hard = time.time() + cap_s
    deadline = time.time() + settle_s
    out = bytearray()
    while time.time() < min(deadline, hard):
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.15
    return out.decode(errors="replace")


def sample(ser):
    txt = command(ser, "nodes") + command(ser, "stats")
    t = time.time()
    nodes = {int(m[0]): [int(x) for x in m[1:]] for m in NODE_RE.findall(txt)}
    bus = BUS_RE.search(txt)
    cyc = CYC_RE.search(txt)
    return {
        "t": t,
        "nodes": nodes,  # id -> [polls, events, dup, gaps, malformed, retries, timeouts]
        "bus": [int(x) for x in bus.groups()] if bus else None,
        "cycles": int(cyc.group(1)) if cyc else None,
        "raw": txt,
    }


def report(tag, s0, s1):
    dt = s1["t"] - s0["t"]
    print(f"--- {tag} ({dt:.1f} s window)")
    for nid in sorted(s1["nodes"]):
        if nid not in s0["nodes"]:
            print(f"  node {nid}: (new since window start)")
            continue
        a, b = s0["nodes"][nid], s1["nodes"][nid]
        d = [y - x for x, y in zip(a, b)]
        print(f"  node {nid}: polls/s={d[0]/dt:8.0f}  events/s={d[1]/dt:7.1f}  "
              f"dup={d[2]} gaps={d[3]} malformed={d[4]} retries={d[5]} timeouts={d[6]}")
    if s0["cycles"] is not None and s1["cycles"] is not None:
        print(f"  poll cycles/s={(s1['cycles'] - s0['cycles'])/dt:8.0f}")
    if s0["bus"] and s1["bus"]:
        d = [y - x for x, y in zip(s0["bus"], s1["bus"])]
        print(f"  bus: tx/s={d[0]/dt:.0f} rx/s={d[1]/dt:.0f} "
              f"crc_err={d[2]} hdr_err={d[3]} cmd_drops={d[4]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--label", default="run")
    ap.add_argument("--rate", type=int, default=100)
    ap.add_argument("--idle-s", type=float, default=10)
    ap.add_argument("--load-s", type=float, default=30)
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.05) as ser:
        ser.dtr = True
        time.sleep(0.3)
        ser.read(4096)

        print(f"### measure '{args.label}': rate={args.rate} ev/s/node "
              f"idle={args.idle_s:.0f}s load={args.load_s:.0f}s")
        command(ser, "log off")  # [EVT] flood would swamp the counter reads
        s0 = sample(ser)
        time.sleep(args.idle_s)
        s1 = sample(ser)
        report("idle", s0, s1)

        command(ser, f"test {args.rate}")
        time.sleep(1.0)  # let the broadcast land before the timed window
        s2 = sample(ser)
        time.sleep(args.load_s)
        s3 = sample(ser)
        report(f"load {args.rate} ev/s/node", s2, s3)

        command(ser, "test stop")
        time.sleep(2.0)
        s4 = sample(ser)
        time.sleep(5.0)
        s5 = sample(ser)
        report("post-stop", s4, s5)
        command(ser, "log on")  # restore console ergonomics for the bench

        bad = []
        for nid, (a, b) in ((n, (s2["nodes"].get(n), s3["nodes"].get(n)))
                            for n in s3["nodes"]):
            if a and b and (b[3] - a[3] or b[4] - a[4] or b[2] - a[2]):
                bad.append(nid)
        print(f"### verdict: {'LOSS on nodes ' + str(bad) if bad else 'zero loss'}")


if __name__ == "__main__":
    main()
