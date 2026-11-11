#!/usr/bin/env python3
"""Screenshots and touch input for the widscan58 display over its USB port
(the debug console, display/src/debug.c). GPL-3.0-or-later.

  cydshot.py shot out.png           whole screen to a PNG
  cydshot.py swipe l|r|u|d          inject a swipe
  cydshot.py tap X Y                inject a tap
  cydshot.py drag X0 Y0 X1 Y1       inject a drag (sliders)
  cydshot.py fakelaps SEAT N        N sample laps on a seat (screenshots)
  cydshot.py bench                  time full-screen redraws (see log)
  cydshot.py log [SECONDS]          print the console (frame rate stats)

Several steps in one run: cydshot.py swipe d -- sleep 0.5 -- shot s.png
Opening the port resets the board; the steps run once it is up again.
"""
import struct
import sys
import time

import serial
from PIL import Image


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 921600
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    # Opening resets the board: wait for the firmware to say it is up (or,
    # if it did not reset, for its 2 s stats line) before sending anything.
    seen, end = b"", time.time() + 10
    while time.time() < end and b"main: up" not in seen and b"main: pano" not in seen:
        seen = (seen + s.read(4096))[-8192:]
    time.sleep(0.3)
    return s


def read_exact(s, n, deadline):
    buf = bytearray()
    while len(buf) < n:
        if time.time() > deadline:
            raise TimeoutError("screenshot timed out")
        buf += s.read(n - len(buf))
    return bytes(buf)


def shot(s, path):
    s.reset_input_buffer()
    deadline = time.time() + 30
    pending = b""
    asked = 0
    while b"#SHOT" not in pending:
        if time.time() > deadline:
            raise TimeoutError("no screenshot header")
        if time.time() - asked > 3:  # again, in case the board was still booting
            s.write(b"shot\n")
            asked = time.time()
        pending = (pending + s.read(256))[-4096:]
    rest = pending[pending.index(b"#SHOT") :]
    while b"\n" not in rest:
        rest += s.read(64)
    head, rest = rest.split(b"\n", 1)
    w, h = (int(v) for v in head.split()[1:3])
    img = Image.new("RGB", (w, h))
    px = img.load()
    buf = bytearray(rest)

    def take(n):
        nonlocal buf
        while len(buf) < n:
            buf += read_exact(s, n - len(buf), deadline)
        out, buf = bytes(buf[:n]), buf[n:]
        return out

    while True:
        tag = take(1)
        if tag == b"E":
            break
        if tag != b"A":
            raise ValueError(f"bad record {tag!r}")
        x1, y1, x2, y2 = struct.unpack("<4H", take(8))
        aw = x2 - x1 + 1
        n = aw * (y2 - y1 + 1)
        j = 0
        while j < n:
            k, v = struct.unpack("<BH", take(3))
            v = (v >> 8 | v << 8) & 0xFFFF  # LVGL's byte-swapped RGB565
            rgb = ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
            for i in range(j, j + k):
                px[x1 + i % aw, y1 + i // aw] = rgb
            j += k
    img.save(path)
    print(f"{path}: {w}x{h}")


def main():
    # Steps by hand: argparse would drop the first "--" separator.
    args = sys.argv[1:]
    port = "/dev/ttyUSB0"
    if len(args) >= 2 and args[0] == "--port":
        port, args = args[1], args[2:]
    if not args or args[0] in ("-h", "--help"):
        sys.exit(__doc__)
    s = open_port(port)
    steps, cur = [], []
    for t in args:
        if t == "--":
            steps.append(cur)
            cur = []
        else:
            cur.append(t)
    steps.append(cur)
    for st in steps:
        if not st:
            continue
        if st[0] == "shot":
            shot(s, st[1] if len(st) > 1 else "shot.png")
        elif st[0] in ("swipe", "tap", "drag", "bench", "peek", "reopen", "fakelaps"):
            s.write((" ".join(st) + "\n").encode())
            time.sleep(0.3)
        elif st[0] == "sleep":
            time.sleep(float(st[1]))
        elif st[0] == "log":
            end = time.time() + float(st[1] if len(st) > 1 else 5)
            while time.time() < end:
                d = s.read(4096)
                if d:
                    sys.stdout.write(d.decode("utf-8", "replace"))
                    sys.stdout.flush()
        else:
            sys.exit(f"unknown step {st[0]}")


if __name__ == "__main__":
    main()
