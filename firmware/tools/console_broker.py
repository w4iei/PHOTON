#!/usr/bin/env python3
"""Shared console broker for the PHOTON CDC port.

Owns the (exclusive) serial device once and fans it out over TCP so any
number of terminals can watch and type at the same time:

    python3 console_broker.py                # broker on localhost:7777
    nc localhost 7777                        # ...in as many terminals as you like

Everything from the device goes to every client and to --log (default
/tmp/photon-console.log); anything any client types goes to the device.
Survives reboots/reflashes: if the device disappears (bootsel, reboot,
replug) the broker retries until it returns.
"""
import argparse
import glob
import socket
import threading
import time

import serial

clients = []
clients_lock = threading.Lock()


def find_port(preferred=None):
    while True:
        if preferred:
            ports = [preferred] if glob.glob(preferred) else []
        else:
            ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if ports:
            return ports[0]
        time.sleep(0.5)


def broadcast(data: bytes, log):
    log.write(data)
    log.flush()
    with clients_lock:
        dead = []
        for c in clients:
            try:
                c.sendall(data)
            except OSError:
                dead.append(c)
        for c in dead:
            clients.remove(c)


def client_thread(conn, ser_ref):
    with clients_lock:
        clients.append(conn)
    try:
        conn.sendall(b"# [broker] connected\r\n")
        while True:
            data = conn.recv(256)
            if not data:
                break
            ser = ser_ref[0]
            if ser is not None:
                try:
                    ser.write(data)
                except OSError:
                    pass  # device mid-reconnect; keystroke lost, device will re-prompt
    except OSError:
        pass
    finally:
        with clients_lock:
            if conn in clients:
                clients.remove(conn)
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial device (default: first cu.usbmodem*)")
    ap.add_argument("--tcp", type=int, default=7777)
    ap.add_argument("--log", default="/tmp/photon-console.log")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.tcp))
    srv.listen(8)
    print(f"# broker: tcp localhost:{args.tcp}, log {args.log}")

    ser_ref = [None]
    log = open(args.log, "ab")

    def acceptor():
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=client_thread, args=(conn, ser_ref), daemon=True).start()

    threading.Thread(target=acceptor, daemon=True).start()

    while True:
        port = find_port(args.port)
        try:
            # exclusive: a stray direct miniterm/screen gets EBUSY instead of
            # silently splitting the byte stream with us.
            ser = serial.Serial(port, 115200, timeout=0.05, exclusive=True)
            ser.dtr = True  # firmware gates output on DTR (tud_cdc_connected)
        except (OSError, serial.SerialException):
            time.sleep(0.5)
            continue
        ser_ref[0] = ser
        banner = f"# [broker] attached {port}\r\n".encode()
        print(banner.decode().strip())
        broadcast(banner, log)
        try:
            while True:
                data = ser.read(4096)
                if data:
                    broadcast(data, log)
        except (OSError, serial.SerialException):
            ser_ref[0] = None
            try:
                ser.close()
            except Exception:
                pass
            broadcast(b"\r\n# [broker] device lost, waiting for it to return...\r\n", log)
            time.sleep(0.5)


if __name__ == "__main__":
    main()
