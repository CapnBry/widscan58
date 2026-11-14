# Widscan58 display

A stand-alone screen for a widscan58: a touch display wired to the ESP32-C5
board's UART pins shows the scanner without a computer or phone. It has the
web UI's three modes, monitor (panorama, waterfall and per-channel levels),
lap timer (with spoken lap times through the board's speaker connector) and
power meter, plus a Global screen for the backlight and the widscan58's GPIO
configuration. Swipe left or right to change modes, swipe down for the
mode's settings.

## Hardware

The display is the 2.8" ESP32 "Cheap Yellow Display", **ESP32-2432S028R**
(320x240, resistive touch).

This code may not work on every ESP32-2432S028R. Boards sold under that name
come with different LCD controllers, and the panel driver here is modified
for the one on the board it was developed with. That controller identifies
itself like an **ST7789**, but takes the **ILI9341** setup sequence that
TFT_eSPI sends for it (`ILI9341_2_DRIVER`), with these changes from a stock
ILI9341 setup:

- display inversion on (`TFT_INVERSION_ON`), and BGR colour order with
  landscape rotation (MADCTL 0x28);
- after the ILI9341 sequence, the ST7789's own porch, voltage, VCOM and gamma
  values (from TFT_eSPI's `ST7789_Init.h`), since some ILI9341 commands are
  analog settings on an ST7789: without them, blacks show as medium grey;
- the SPI clock raised from 40 to 80 MHz once the panel is set up.

A board with a real ILI9341, or an ST7789 that expects the ST7789 sequence,
can show a mirrored, colour-inverted or shifted picture. The setup is in
`src/lcd_ili9341.c` (`panel_init`).

### Experimental: 3.5" ESP32-S3 display

A second target, `cyds3`, is for experimenting only: the 3.5" ESP32-S3
display **ES3C35P** (ESP32-S3 with 16 MB flash and 8 MB PSRAM, 480x320 IPS
panel with an ST77922 controller on QSPI, capacitive touch on I2C, USB on
the ESP32-S3's own port). Only the monitor screen is scaled to its larger
screen, as a demonstration; the other screens keep the 320x240 layout in
the top-left corner. There is no sound output, no RemoteID receiver and no
flashing of the widscan58 through it, and it has only been run with the
built-in simulator (`pio run -e cyds3-sim -t upload`). After flashing over
other firmware, unplug it once: its panel only resets at power-up.

## Wiring

The display uses its CN1 connector (GND, GPIO22, GPIO27, 3.3V). Wire TX to
RX each way; the link runs at 921600 baud.

| Display (CN1) | ESP32-C5 board |
|---|---|
| GPIO22 (TX) | GPIO12 (RX) |
| GPIO27 (RX) | GPIO11 (TX) |
| GND | GND |
| 3.3V | 3V3 (optional: powers the C5 board, only while its USB is unplugged) |

GPIO11 and GPIO12 are the widscan58's default UART pins. Leave the C5 board's
USB unplugged when powering it from the display's 3.3V pin: two regulators
would otherwise feed the same rail.

## Sound output

In lap timer mode the display says each lap time through the speaker
connector. As shipped, the board's amplifier is so loud that it pulls down
the input voltage and sounds terrible. Two resistors around the amplifier
(the 8-pin chip by the speaker connector) set its gain, 2 × Rf / Ri:

- Rf, marked `683` (68 kΩ), across the amplifier's pins 4 and 5;
- Ri, marked `472` (4.7 kΩ), on the line from GPIO26.

Replacing the `683` with a `682` (6.8 kΩ) lowers the gain from about 29 to
about 2.9, so full volume is about as loud as 10 % was before. If that is
still too loud, also replace the `472` with a `682`, for a gain of 2.

## Building

```sh
cd display
pio run -e cyd -t upload        # the display, for a widscan58 on its UART
pio run -e cyd-sim -t upload    # with a built-in simulated widscan58, no C5 needed
```

The lap callouts are `voice/voice.bin`, made with `tools/gen_voice.py`
(a paid text-to-speech API; see CLAUDE.md). It is in the repository, so a
build does not need to make it.
