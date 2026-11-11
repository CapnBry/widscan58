#!/usr/bin/env python3
"""Flash the widscan58 (ESP32-C5) through the display's USB port.
GPL-3.0-or-later.

The display (display/, a CYD on the C5's host UART) restarts into a USB-UART
bridge on its "passthru" console command, puts the C5 in ROM download mode
(BOOTLOADER, so the C5 must be running our firmware) and follows esptool's
baud rate changes (display/src/passthru.c). This script builds the
firmware, sends "passthru", then runs esptool on the same open port: opening
the CYD's port resets it, so the port stays open from the command to the end
of the flash. The C5 leaves download mode by a watchdog reset; the display
goes back to normal 8 s later.

  tools/displayflash.py [--port P] [--baud 921600] [--no-build]

Run with PlatformIO's Python (it has esptool):
  ~/.platformio/penv/bin/python tools/displayflash.py
"""
import argparse
import glob
import os
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENV = "esp32c5"


def default_port():
    ports = sorted(glob.glob("/dev/serial/by-id/usb-1a86_USB_Serial*"))
    if not ports:
        sys.exit("no CH340 (display) port found; give --port")
    return ports[0]


def open_display(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 921600
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    # Opening resets the board: wait until it is up (or, if it did not reset,
    # for its 2 s stats line).
    seen, end = b"", time.time() + 10
    while time.time() < end and b"main: up" not in seen and b"main: pano" not in seen:
        seen = (seen + s.read(4096))[-8192:]
    if b"main: up" not in seen and b"main: pano" not in seen:
        sys.exit("display did not answer (is it running the display firmware?)")
    time.sleep(0.3)
    return s


def enter_passthru(s):
    s.reset_input_buffer()
    s.write(b"\npassthru\n")
    seen, end = b"", time.time() + 10
    while time.time() < end and b"passthru: ready" not in seen:
        seen = (seen + s.read(4096))[-8192:]
    i = seen.find(b"passthru: ready")
    if i < 0:
        sys.exit("display did not enter passthrough (display firmware too old?)")
    while b"\n" not in seen[i:]:
        seen += s.read(64)
    status = seen[i:].split(b"\n", 1)[0].decode(errors="replace").split()[-1]
    if status != "download":
        print("warning: the C5's download mode banner was not seen; trying anyway", file=sys.stderr)
    s.baudrate = 115200  # the ROM's rate; the display has switched too


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="the display's USB port (default: the first CH340)")
    ap.add_argument("--baud", type=int, default=921600, help="flashing rate after the sync at 115200")
    ap.add_argument("--no-build", action="store_true", help="flash what is in firmware/.pio/build")
    args = ap.parse_args()
    import esptool
    from esptool.targets import ESP32C5ROM

    build = os.path.join(ROOT, "firmware", ".pio", "build", ENV)
    if not args.no_build:
        subprocess.run(["pio", "run", "-e", ENV], cwd=os.path.join(ROOT, "firmware"), check=True)
    port = args.port or default_port()

    s = open_display(port)
    enter_passthru(s)
    esp = ESP32C5ROM(s, 115200)
    esp.connect("no-reset")
    esptool.main(
        ["--chip", ENV, "--baud", str(args.baud), "--before", "no-reset", "--after", "watchdog-reset",
         "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "4MB",
         "0x2000", os.path.join(build, "bootloader.bin"),
         "0x8000", os.path.join(build, "partitions.bin"),
         "0x10000", os.path.join(build, "firmware.bin")],
        esp=esp,
    )
    s.close()


if __name__ == "__main__":
    main()
