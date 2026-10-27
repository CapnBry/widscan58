#!/usr/bin/env python3
"""Record one channel's raw level codes as lap timer mode sees them (monitor
mode with lap timer mode's scan settings), for offline replay through lap.c
(replay.c). GPL-3.0-or-later.

Writes CSV: seq,t_ms,rssi,saturated. Needs widscan58cli.py and ttyfix.py next
to this file or in its parent directory (tools/)."""
import argparse
import os
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [here, os.path.dirname(here)]
import widscan58cli as w  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--port", default="/dev/ttyACM0")
ap.add_argument("-s", "--seconds", type=float, default=180)
ap.add_argument("-o", "--out", required=True)
ap.add_argument("--chan", default="R1=5658", help="NAME=MHz of the one channel scanned")
ap.add_argument("--gain", type=int, default=45)
ap.add_argument("--fft", type=int, default=64)
args = ap.parse_args()

link = w.Link(args.port)
setup = ["PROTOCOL MONITOR", f"CHANNELS {args.chan}", "ENABLE 1", "MODE GROUP", f"GAIN MANUAL {args.gain}",
         f"FFT {args.fft}", "METRICS RSSI", "SPEC 0 256 40000000 MAX", "THROTTLE 1050"]
print(link.command("PROTOCOL MONITOR"))
print(link.command("INFO", "INFO"))
print(link.command("PLATEAU?", "PLATEAU"))
for c in setup:
    r = link.command(c)
    print(f"{c}: {r}")
    if not r or r.startswith("ERR"):
        sys.exit(f"setup failed at {c!r}")
print("STATUS:", (link.command("STATUS?", "STATUS") or "")[:400])

n, last_seq, gaps, t0 = 0, None, 0, time.monotonic()
next_report = t0 + 5
with open(args.out, "w") as f:
    f.write("seq,t_ms,rssi,saturated\n")
    while time.monotonic() - t0 < args.seconds:
        for item in link.poll():
            if item[0] != "frame" or item[1] != w.FRAME_LEVELS:
                continue
            h = w.LEVELS_HDR.unpack_from(item[2])
            seq, t_ms, sat, nch = h[0], h[1], h[6], h[12]
            for j in range(nch):
                cid, flags, rssi, snr = w.LEVEL.unpack_from(item[2], w.LEVELS_HDR.size + j * w.LEVEL.size)
                if cid == 0:
                    f.write(f"{seq},{t_ms},{rssi},{sat}\n")
                    n += 1
            if last_seq is not None and seq != last_seq + 1:
                gaps += 1
            last_seq = seq
        if time.monotonic() >= next_report:
            next_report += 5
            print(f"{time.monotonic() - t0:5.0f} s  {n} updates ({n / (time.monotonic() - t0):.0f} /s), seq gaps {gaps}",
                  flush=True)
print(f"done: {n} updates, seq gaps {gaps}, crc errors {link.crc_errors} -> {args.out}")
