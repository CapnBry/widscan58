# esp-rx5808

FPV channel scanner using the ESP32's built-in radio as an SDR, through the raw
I/Q debug path found by [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr).
The firmware steps through FPV channels (Raceband) and reports RSSI, noise floor,
SNR, peak and video-centre frequency per channel, plus a decimated spectrum. A
Web Serial dashboard shows it all live.

Status: the **ESP32-S3** runs a scaled 2.4 GHz mock-up of Raceband (channels
S1–S8, see [PLAN.md](PLAN.md) §2.2). The **ESP32-C5** (real 5.8 GHz Raceband)
backend comes once hardware is available.

License: GPL-3.0-or-later (see [LICENSE](LICENSE)). Files derived from esp-sdr
say so in their headers.

## Layout

| Path | What |
|---|---|
| `firmware/` | PlatformIO + ESP-IDF 5.5.5 (pioarduino). `src/radio/` is the only target-specific code. |
| `web/` | Vite + TypeScript dashboard (Web Serial, simulator, replay). |
| `tools/rx5808cli.py` | Python client: `info`, `monitor`, `rawiq`, `spectrum`, `cmd`, `bootloader`. |
| `tools/serial_bridge.py` | Serial ↔ WebSocket bridge (headless tests, remote viewing). |
| `tools/flash.sh` | Build and flash with esptool (`--manual` after BOOT + EN). Experimental, see the flashing notes. |
| `tools/check_phy_abi.sh` | Compares the private PHY functions we call between two ESP-IDF versions. |
| `ref/esp-sdr/` | Reference checkout of esp-sdr (not tracked here). Restore it with `git clone https://github.com/ESPARGOS/esp-sdr ref/esp-sdr`. |
| `backup/` | Full flash dump of the S3 as it arrived (esp-sdr WebSDR firmware). Local only, not tracked. |

## Firmware

```sh
cd firmware
pio run -e esp32s3 -t upload      # esptool resets via USB (usb-reset); usually no buttons needed
```

**Flashing is not fully reliable yet** (see PLAN.md, "Open: flashing reliability"). Uploads sometimes fail partway through, and a failure after the erase step leaves the board unbootable (the ROM prints `invalid header: 0xffffffff`). The reliable path:

1. Hold BOOT, tap EN, release BOOT.
2. Run `tools/flash.sh --manual`, or `esptool --before no-reset --after watchdog-reset write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin` from `firmware/.pio/build/esp32s3`.

Use `--after watchdog-reset`: an RTS reset does not always leave download mode.

Use the PlatformIO Core that pioarduino 55.03.312-1 expects (6.2.x). The
platform pins ESP-IDF 5.5.5. Before changing the platform version, run
`tools/check_phy_abi.sh esp32s3 25fe69f946311abdaf9ad56591f25fedbc20ac98 <new-idf-tag>`.
The firmware depends on private PHY functions, and they must stay identical.

Ways back into the bootloader: esptool's `usb-reset` (automatic on upload), or
hold BOOT while pressing EN. The `BOOTLOADER` command left the board
unresponsive in testing; don't rely on it yet.

To restore the original WebSDR firmware:
`esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0 backup/websdr-s3-flash-4MB.bin`.

## Performance (ESP32-S3, measured)

| Setup | Updates per channel |
|---|---:|
| All 8 channels (4 pair LOs), S16R FFT (default) | ~64 /s |
| All 8 channels, R4 FFT | ~57 /s |
| One channel, or one neighbouring pair (1 LO: the tune is skipped) | ~445 /s (S16R), ~344 /s (R4) |

- A hop with 4 LOs is about 0.83 ms tune (median; bursty long tunes add ~15 %), 0.17 ms capture (4096 samples at 40 MS/s), and about 2.2 ms DSP with S16R.
- The display spectrum is sent every 4th hop by default (`SPEC`).
- Full tables and the FFT accuracy study are in PLAN.md.

## Useful commands

Send these with `tools/rx5808cli.py cmd …` or the UI's Protocol console.

| Command | Purpose |
|---|---|
| `INFO`, `STATUS?`, `PROFILE?`, `TIMING?`, `SPURS?` | Identity, full configuration, band profile, DSP time breakdown, masked board spurs |
| `ENABLE <mask>`, `MODE PAIR\|SINGLE`, `SCAN START\|STOP` | Which channels are scanned, and how |
| `GAIN MANUAL <idx>\|HARDWARE`, `FILTER <code>\|AUTO` | RX gain and analog filter |
| `FFTTYPE R2\|R4\|S16\|S16R`, `FFTTYPE?` | FFT implementation. Default S16R; R4 is exact float and best for step-on/IMD measurement |
| `FFTEVAL SYNTH <noise×100> <tone_dBFS×10> <reps>`, `FFTEVAL LIVE <lo_kHz> <reps>` | Compare all FFT types on identical samples |
| `SPEC <every_n_hops> <bins> <span_hz> MAX\|MEAN` | Display spectrum rate and resolution (default `SPEC 4 256 11300000 MAX`) |
| `RAWIQ <lo_kHz> <samples> [rate]` | One raw I/Q snapshot (8-bit, auto-scaled) |
| `RECAL`, `RECALMODE ONCE\|EVERY`, `SETTLE <us>`, `TUNESKIP <mask>` | Calibration and tuning (debug) |
| `REBOOT` | Restart (USB stays connected; the UI re-syncs on `READY`) |

## Web UI

```sh
cd web
npm install
npm run dev                         # http://localhost:5808 (Chrome/Edge for Web Serial)
```

- **Connect device**: Web Serial. Chrome remembers the grant, so later visits
  and board resets reconnect automatically.
- **Simulate**, or open `http://localhost:5808/?sim`: synthetic VTXs, Wi-Fi
  bursts, board spurs and IMD3 products.
- **Bridge**: `python3 tools/serial_bridge.py`, then open
  `http://localhost:5808/?bridge=ws://127.0.0.1:5809`.
- **Record / Replay**: saves the raw stream to a `.rx5808` file and plays it back.

Only one program can hold `/dev/ttyACM0` at a time: browser tab, bridge,
`rx5808cli.py` or esptool.

### Tests

```sh
cd web
npm test                                   # parser/protocol unit tests
node e2e/headless.mjs --sim                # headless Chrome vs. the simulator
python3 ../tools/serial_bridge.py &        # then, against the real board:
node e2e/headless.mjs                      # writes screenshots + results.json to e2e-out/
```
