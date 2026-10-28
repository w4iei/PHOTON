#!/usr/bin/env python3
"""Terminal client for console_broker.py that sends each key as it is typed.

    python3 console_client.py                # localhost:7777
    python3 console_client.py --port 7778

`nc localhost 7777` works too, but it holds a line until Enter and echoes
it locally, so the calibration view (redrawn four times a second) paints
over what is being typed. Here the terminal is in cbreak mode: every key
goes straight to the board, the board echoes it, and the view redraws the
typed line under the keyboard. Ctrl-C quits and restores the terminal.
"""
import argparse
import os
import select
import socket
import sys
import termios
import tty


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=7777)
    a = ap.parse_args()

    s = socket.create_connection((a.host, a.port))
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)  # no line buffering, no local echo; Ctrl-C still works
        while True:
            r, _, _ = select.select([s, fd], [], [])
            if s in r:
                data = s.recv(65536)
                if not data:
                    break
                os.write(1, data)
            if fd in r:
                keys = os.read(fd, 64)
                if not keys:
                    break
                s.sendall(keys.replace(b"\n", b"\r"))
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        print("\n[console closed]")


if __name__ == "__main__":
    main()
