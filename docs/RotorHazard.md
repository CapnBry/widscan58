# Widscan58 as a RotorHazard timer node

The board can act as a [RotorHazard](https://github.com/RotorHazard/RotorHazard)
timer node: one USB port with 8 seats, like an S32_BPill board with 8 RX5808
receivers. The RotorHazard server needs no plugins or changes, only the serial
port in its config.

## Installation

1. Open https://capnbry.github.io/widscan58/ in a web browser, plug in the
   ESP32-C5 board and click **Flash device**.
2. Click **Connect device**, open the menu (☰, top left) and pick
   **RotorHazard mode**. The firmware also boots in this mode.
3. Under **Receiver**, set the RX gain and pick the level the seats report:
   - **RSSI**: the power in the channel's ±5 MHz window, in dBFS
     (level / 4 − 63.75, 0.25 dB steps). It moves with the gain and the
     noise: with no VTX it sits around 13–25 at gains up to about 40,
     around 27–41 at the default gain of 50, and around 65 at gain 60.
   - **SNR**: that power over the channel's noise floor, in dB (level / 4). It
     sits near 0 with no VTX on the channel, whatever the gain.
4. Click **Save** so the board keeps these settings. RotorHazard's own UI can't
   set them, and the board applies the saved ones every time it enters
   RotorHazard mode.
5. Click **Disconnect** (only one program can hold the serial port), then plug
   the board into the RotorHazard server.

## Configuring RotorHazard

Add the board's serial port to `SERIAL_PORTS` in the `GENERAL` section of the
server's `config.json`, then restart the server:

```json
{
  "GENERAL": {
    "SERIAL_PORTS": ["/dev/ttyACM0"]
  }
}
```

The same list is under *Advanced Settings* → *Hardware* → *Serial Ports* in
RotorHazard's web UI.

- The ESP32-C5 board connects through its native USB port, which shows up on
  Linux and the Raspberry Pi as `/dev/ttyACM0` or `/dev/ttyUSB0`. On Windows it is a `COMx` port.
- `/dev/ttyACM0` can change number if other USB serial devices are plugged in.
  The `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_…-if00` link
  always points at the same board.
- The server finds 8 nodes on the port. It sets each seat's frequency from the
  current profile. A seat set to off is left out of the scan, which makes the
  other seats update faster (see below).

## Performance

Each update of a seat needs one capture at its LO (local oscillator). In GROUP
mode, channels within 40 MHz share an LO, so one capture covers two Raceband
channels. A full pass over the seats is "number of LOs × hop time". With only
one LO the board never retunes, which is much faster.

Measured on the C5 with the default RotorHazard settings (`FFT 128`, `CAPN 512`:
four 128-point FFTs with 625 kHz bins, averaged), GROUP LO, Raceband
R1 up to Rn, using RotorHazard's own interface code (`tools/rh_check.py`). The
time is the board's `loop_time` for the seat, the interval between updates:

| Seats on | LOs | Time between updates | Updates per seat |
|---:|---:|---:|---:|
| 1 | 1 | ~0.57 ms | ~1750 /s |
| 2 | 1 | ~0.59 ms | ~1700 /s |
| 3 | 2 | ~2.35 ms | ~425 /s |
| 4 | 2 | ~2.4 ms | ~415 /s |
| 5 | 3 | ~3.6 ms | ~280 /s |
| 6 | 3 | ~3.6 ms | ~280 /s |
| 7 | 4 | ~4.75 ms | ~210 /s |
| 8 | 4 | ~4.75 ms | ~210 /s |

- The LO count decides the rate, not the seat count. Bands R and E need 4 LOs
  for 8 channels; A, B and F need 3. Spread-out mixed channels can need one LO
  per seat.
- RSSI and SNR run at the same rate.
- Larger FFTs (256-bin, 512-bin) and longer captures cost more processing per hop and
  lower all the rates. They give finer bins, which RSSI and SNR output typically
  don't need.
- **SINGLE mode** (LO per channel) gives every seat its own LO, so it runs at
  roughly half the GROUP rate for the same seats.

## Smoothing

Each seat's level goes through a running median over the last 255 ms before
pass detection sees it. This copies what RotorHazard's own nodes do, so
levels hold about as still as an RX5808 node's.

**RotorHazard nodes** (Arduino and S32_BPill firmware alike) read each RX5808's
RSSI pin with one ADC read about every millisecond, about 1000 reads per
second per receiver. They take a running median of the last 255 reads, which
covers about 255 ms. Each filtered value is reported with the time of the
window's middle read, about 128 ms back.

**Widscan58** does the same with its own updates:

- The median covers the updates from the last 255 ms, up to 255 of them.
- Each filtered level carries the time of the window's middle update, so
  pass times are not shifted by the filter.
- Nothing is reported for the first 255 ms after power-up or a frequency
  change, while the window fills. A RotorHazard node also waits for a full
  window.

| | RotorHazard node | Widscan58 |
|---|---|---|
| Updates per seat | ~1000 /s | 210–1750 /s (see Performance) |
| One update | one ADC read of the RX5808 RSSI voltage | the power in the channel's ±5 MHz window, from four averaged FFTs of one capture |
| Median window | 255 reads, ~255 ms | 255 ms, at most 255 updates |
| Level steps | ADC counts, 0–255 | 0.25 dB (RSSI or SNR codes, 1–254) |
| Filter delay | ~128 ms, reported with the middle read's time | the same |

## Notes

- **Pass detection runs on the board** at the full update rate: the 255 ms
  median (see Smoothing), a crossing from EnterAt until the level drops below ExitAt, and the
  pass at the middle of the crossing's highest plateau. RotorHazard polls the
  board about every 100 ms, but the pass time comes from the board's own update
  rate, not from that poll.
- **EnterAt / ExitAt** are in the same level units as the seat levels (RSSI or
  SNR codes, 1–254) These can be edited in RotorHazard's settings.
- **Not yet tested with a real quad flying through a gate.** Good thresholds,
  pass-time accuracy against RX5808 nodes, and whether RSSI or SNR works better
  are still open.
- Board spurs are not masked in RSSI/SNR mode (the FFT bins are too wide).
  A spur adds roughly 2dB to the base RSSI.
- In GROUP mode a very strong VTX can show a faint mirror image on its pair
  channel (R1 on R2, for example). The digital IQ correction (`IQCORR`, on by
  default) removes it. Using SINGLE mode also avoids it, at half the rate.
- The web UI's RotorHazard page polls the board the same way the server does.
