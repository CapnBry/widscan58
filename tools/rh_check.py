#!/usr/bin/env python3
"""Check the board against RotorHazard's own interface code. GPL-3.0-or-later.

Runs RotorHazard's server-side node interface (src/interface: RHInterface and
serial_node) against the board as its server does: discovery, seat
frequencies and thresholds, then the 100 ms update loop. Prints what the
server sees each second, the passes and crossings it records, and its
communication error report. Uses RotorHazard's code from a checkout, not a
copy of it:

  git clone https://github.com/RotorHazard/RotorHazard ref/RotorHazard
  python3 -m venv /tmp/rhvenv && /tmp/rhvenv/bin/pip install gevent pyserial
  /tmp/rhvenv/bin/python tools/rh_check.py [-p /dev/ttyACM0] [-s 8] [--enter 145 --exit 125]

Without a VTX, --enter / --exit inside the filtered noise (e.g. 67 / 67 in
RSSI codes at manual gain 60) make noise crossings, which exercises the pass
path.

Opening the port as RotorHazard does resets the board into its saved
defaults, so receiver settings for the test go in with --cmd, sent once
discovery is done (before the seats are set up), e.g.
  --cmd "METRICS RSSI" --cmd "GAIN MANUAL 60"
Only commands RotorHazard mode allows work (see CLAUDE.md); CONFIG SAVE is
refused here, so a test never changes the saved defaults.
"""
import argparse
import logging
import os
import sys
import time

from gevent import monkey

monkey.patch_all()
import gevent  # noqa: E402

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("-p", "--port", default="/dev/ttyACM0")
ap.add_argument("-s", "--seconds", type=float, default=8)
ap.add_argument("--rh", default=os.path.join(here, "..", "ref", "RotorHazard"), help="RotorHazard checkout")
ap.add_argument("--enter", type=int, default=145, help="EnterAt for every seat (level code)")
ap.add_argument("--exit", type=int, default=125, help="ExitAt for every seat (level code)")
ap.add_argument("--seats", type=int, default=6, help="seats given R1.. (the rest off, as RotorHazard does)")
ap.add_argument("--cmd", action="append", default=[], metavar="LINE",
                help="device command to send after discovery, e.g. 'METRICS RSSI' (repeatable; not saved)")
args = ap.parse_args()
if any(c.strip().upper().startswith("CONFIG SAVE") for c in args.cmd):
    ap.error("--cmd CONFIG SAVE would change the saved RotorHazard defaults")
sys.path.insert(0, os.path.join(args.rh, "src", "interface"))
logging.basicConfig(level=logging.INFO, format="%(name)s %(levelname)s %(message)s")

import RHInterface  # noqa: E402


class Config:
    def get_item(self, section, key):
        return [args.port] if (section, key) == ("GENERAL", "SERIAL_PORTS") else None


t0 = time.monotonic()
intf = RHInterface.get_hardware_interface(config=Config(), isS32BPillFlag=False)
nodes = list(intf.nodes)
print(f"discovered {len(nodes)} nodes in {time.monotonic() - t0:.1f} s")
for n in nodes:
    print(f"  node {n.index + 1}: api {n.api_level} fw {n.firmware_version_str!r} freq {n.frequency} "
          f"enter {n.enter_at_level} exit {n.exit_at_level} slot {n.multi_node_slot_index}")
# Our command lines, on RotorHazard's own port before its polling starts.
ser = nodes[0].serial if nodes else None
for c in args.cmd:
    ser.reset_input_buffer()
    ser.write(c.encode() + b"\n")
    reply, deadline = "", time.monotonic() + 2
    while time.monotonic() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        # Commands answer OK/ERR; queries (CONFIG?, METRICS?) one line of their own.
        if line.startswith(("OK", "ERR")) or (c.strip().endswith("?") and line):
            reply = line
            break
    print(f"  cmd {c!r}: {reply[:120] or 'no reply'}")
    if not reply.startswith("OK") and not c.strip().endswith("?"):
        sys.exit(f"command failed: {c!r}")
passes, crossings = [], []
intf.pass_record_callback = lambda node, ts, src, peak=None: passes.append((node.index + 1, round(ts, 3), peak))
intf.node_crossing_callback = lambda node: crossings.append((node.index + 1, node.crossing_flag))
intf.new_enter_or_exit_at_callback = lambda node, enter: None
raceband = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]
for j, n in enumerate(nodes):
    ok = intf.set_frequency(j, raceband[j] if j < args.seats else 0)
    intf.set_enter_at_level(j, args.enter)
    intf.set_exit_at_level(j, args.exit)
    print(f"  set node {j + 1}: {'ok' if ok else 'FAILED'} freq {n.frequency} enter {n.enter_at_level} exit {n.exit_at_level}")
intf.start()
end = time.monotonic() + args.seconds
while time.monotonic() < end:
    gevent.sleep(1.0)
    print("level " + " ".join(f"{n.current_rssi:3}" for n in nodes) +
          "  peak " + " ".join(f"{n.node_peak_rssi:3}" for n in nodes) +
          "  nadir " + " ".join(f"{n.node_nadir_rssi:3}" for n in nodes) +
          "  loop_us " + " ".join(f"{n.loop_time}" for n in nodes if n.frequency))
intf.stop()
print(f"passes ({len(passes)}):", passes[:20])
print(f"crossing changes ({len(crossings)}):", crossings[:20])
print(intf.get_intf_error_report_str(True))
