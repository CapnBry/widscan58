#!/usr/bin/env bash
# Flash the Widscan58 firmware reliably over the S3's USB Serial/JTAG
# (RX_ENV=esp32c5 for the C5; the S3 notes below are untested on it).
# GPL-3.0-or-later.
#
# esptool's USB reset into download mode can lose the race against the
# firmware's continuous output (~77 KiB/s). A failure after the erase step
# leaves the board without a bootable app, and from then on the bootloader's
# RTC watchdog (which no app ever disables) resets the chip part-way through
# every later flash attempt. So: stop the stream first (SCAN STOP), then let
# esptool reset into download mode, and leave via a watchdog reset (an RTS
# reset does not always exit download mode on USB Serial/JTAG).
#
# Recovery when the board does not run our firmware: hold BOOT, tap EN,
# release BOOT (that full reset also clears the watchdog), then run
#   tools/flash.sh --manual
#
#   [RX_ENV=esp32c5] tools/flash.sh [--manual] [port]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
manual=0
if [[ ${1:-} == --manual ]]; then manual=1; shift; fi
port=${1:-$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_* 2>/dev/null | head -1)}
[[ -n $port ]] || { echo "no Espressif USB Serial/JTAG device found" >&2; exit 1; }
esptool=${ESPTOOL:-$HOME/.platformio/penv/bin/esptool}
env=${RX_ENV:-esp32s3} # or esp32c5
build=$here/firmware/.pio/build/$env

(cd "$here/firmware" && pio run -e "$env")

before=usb-reset
if (( manual )); then
    before=no-reset # already in download mode via BOOT + EN
else
    python3 -c '
import sys, time, serial
s = serial.Serial(sys.argv[1], 115200, timeout=0.1)
s.write(b"\nSCAN STOP\n")
time.sleep(0.3)
s.reset_input_buffer()
' "$port"
fi

boot_offset=0x0
[[ $env == esp32c5 ]] && boot_offset=0x2000 # C5 ROM loads the bootloader from 0x2000
"$esptool" --chip "$env" -p "$port" --before "$before" --after watchdog-reset \
    write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB \
    "$boot_offset" "$build/bootloader.bin" 0x8000 "$build/partitions.bin" 0x10000 "$build/firmware.bin"

# esptool (pyserial) leaves the tty at VMIN=0, which Chrome's Web Serial
# misreports as "The device has been lost"; see tools/ttyfix.py.
for _ in {1..30}; do
    stty -F "$port" min 1 time 0 2>/dev/null && break
    sleep 0.1
done
