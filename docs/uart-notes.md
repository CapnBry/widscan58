# Connecting over the UART pins

Besides USB, the board talks over a serial port on two of its pins: 921600
baud, 8N1, 3.3 V logic. It carries the same protocols as USB (monitor mode
and lap timer mode, so a RotorHazard server can use it as a timer node). The
board answers whichever port last received data: input on the other port
makes that one active and drops anything still queued for the old one.

The default pins are GPIO11 (TX) and GPIO12 (RX), the same pins the chip's
boot ROM uses for its serial port. Wire them crossed to the host, and connect
the grounds:

| ESP32-C5 board | Raspberry Pi header |
|---|---|
| GPIO11 (TX) | GPIO15 / RXD (pin 10) |
| GPIO12 (RX) | GPIO14 / TXD (pin 8) |
| GND | GND (pin 6) |

To move them, open the web UI's menu (☰) → **GPIO Configure**, pick **TX** and
**RX** on the new pins and click **Save**. **Defaults** puts every pin back to
the built-in settings. From the command line, over USB or the UART:

```sh
python3 tools/widscan58cli.py cmd "GPIO 23 TX"
python3 tools/widscan58cli.py cmd "GPIO 24 RX"
python3 tools/widscan58cli.py cmd "GPIO SAVE"     # keep them after a restart
python3 tools/widscan58cli.py cmd "GPIO RESET"    # back to GPIO11 / GPIO12, nothing else set
```

Changing the pins over the UART itself cuts that connection: rewire, then
continue on the new pins.

## Raspberry Pi: the full UART on the header

The Raspberry Pi 3 and 4 give their good UART (a PL011) to Bluetooth and put
the mini UART on the header. The mini UART's baud rate follows the GPU core
clock, which makes 921600 unreliable. Give the header the PL011 instead, in
`/boot/firmware/config.txt` (`/boot/config.txt` before Raspberry Pi OS
Bookworm):

```
enable_uart=1
dtoverlay=disable-bt
```

and stop the Bluetooth modem service, which would otherwise hold the port:

```sh
sudo systemctl disable hciuart
```

`dtoverlay=miniuart-bt` instead keeps Bluetooth, on the mini UART. After a
reboot the header UART is `/dev/ttyAMA0` (also `/dev/serial0`).

- **Raspberry Pi 1 and 2:** the PL011 is already on the header; only
  `enable_uart=1` is needed. Port `/dev/ttyAMA0`.
- **Raspberry Pi 5:** `enable_uart=1` turns on the separate debug connector
  (`/dev/ttyAMA10`), not the header. Add `dtparam=uart0=on` for GPIO14/15;
  the port is `/dev/ttyAMA0`.

## Other notes

- **Turn off the Pi's serial console.** With a login shell on the port, the
  Pi prints its boot messages into the board and answers the board's output.
  `sudo raspi-config` → Interface Options → Serial Port: login shell **No**,
  serial port hardware **Yes**.
- **Boot banner:** at every reset the chip's ROM prints a few lines on TX at
  115200 baud, before the firmware starts. At 921600 the host reads them as a
  burst of garbage. RotorHazard flushes the port before it probes for nodes.
- **Booting with the Pi attached:** GPIO11 and GPIO12 are not strapping pins,
  so the Pi's pull-ups and pull-downs on its UART pins do not change how the
  board boots. The chip's strapping pins are GPIO7, GPIO27, GPIO28 and two of
  its JTAG pins (MTMS, MTDI); keep anything that pulls them away from the
  board.
- **Power both together.** A powered Pi driving the RX pin of an unpowered
  board (or the other way round) feeds current through the pin's protection
  diodes. Power the board from the Pi's 5 V, or from the same supply.
- **Flashing:** the ROM's download mode listens on these same pins, but the
  chip only enters it with BOOT held during a reset. Without access to the
  BOOT button, flash over USB.
- **One host in charge.** Close the web UI or other programs on the
  USB port when a host on the UART is meant to be in charge: whichever port
  sent last gets the output.
- **RotorHazard:** add the Pi's port to `GENERAL` → `SERIAL_PORTS` in its
  config, e.g. `"SERIAL_PORTS": ["/dev/ttyAMA0"]`. It tries 115200 and 921600
  baud when it looks for nodes.
- **Other tools** (`widscan58cli.py`, `serial_bridge.py`) open their port at
  921600 baud; over USB the rate makes no difference.
