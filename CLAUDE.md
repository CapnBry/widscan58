# Widscan58: developer notes

Technical reference for working on this repo: layout, build, flashing, tools,
device commands and tests. The user-facing description is in
[README.md](README.md); the design, measurements and status log are in
[PLAN.md](PLAN.md).

FPV channel scanner using the ESP32's built-in radio as an SDR, through the raw
I/Q debug path found by [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr).
The firmware steps through up to eight FPV channels and reports RSSI, noise
floor, SNR, peak and video-centre frequency per channel, plus a decimated
spectrum. A Web Serial dashboard shows it all live.

Targets: the **ESP32-C5** (real 5.8 GHz) runs on a Seeed XIAO ESP32-C5 and works
with the web UI: ~4.75 ms/hop, about 53 rotations/s over R1–R8 (PLAN.md, Status,
has the breakdown). The **ESP32-S3** runs a scaled 2.4 GHz mock-up of the 5.8 GHz
band (PLAN.md §2.2); it is a development target only.

License: GPL-3.0-or-later. Files derived from esp-sdr say so in their headers;
keep that for anything new that is copied or derived from it.

## Layout

| Path | What |
|---|---|
| `firmware/` | PlatformIO + ESP-IDF 5.5.5 (pioarduino). `src/radio/` is the only target-specific code. |
| `web/` | Vite + TypeScript dashboard (Web Serial, simulator, replay). |
| `tools/widscan58cli.py` | Python client: `info`, `monitor`, `rawiq`, `spectrum`, `cmd`, `bootloader`. |
| `tools/serial_bridge.py` | Serial ↔ WebSocket bridge (headless tests, remote viewing). |
| `tools/flash.sh` | Build and flash with esptool (`--manual` after BOOT + EN; `RX_ENV=esp32c5` for the C5). Experimental, see the flashing notes. |
| `tools/pages_firmware.py` | Stages the built firmware into `web/public/firmware` for the in-page flasher (used by CI). |
| `tools/check_phy_abi.sh` | Compares the private PHY functions we call between two ESP-IDF versions. |
| `tools/ttyfix.py` | Leaves a serial port at VMIN=1 after pyserial use (see the Web UI "device has been lost" note). |
| `tools/gen_twiddle.py` | Generates `firmware/src/dsp/fft_twiddle_q15.h` for the radix-4 FFT. |
| `.github/workflows/` | Builds firmware + web UI and publishes to GitHub Pages (https://capnbry.github.io/widscan58/). Only `esp32c5` firmware is published (`FW_ENVS`). |
| `images/` | Screenshots used by README.md. |
| `ref/esp-sdr/` | Reference checkout of esp-sdr (not tracked here). Restore it with `git clone https://github.com/ESPARGOS/esp-sdr ref/esp-sdr`. |
| `backup/` | Full flash dump of the S3 as it arrived (esp-sdr WebSDR firmware). Local only, not tracked. |

## Firmware

```sh
cd firmware
pio run -e esp32s3 -t upload      # esptool resets via USB (usb-reset); usually no buttons needed
pio run -e esp32c5 -t upload      # ESP32-C5 (tested on a XIAO ESP32-C5), native USB port
```

Each target has one band profile, fixed at build time: `s3-race24` on the S3,
`c5-race` on the C5. `PROFILE?` reports it, with the active channel plan.

**Flashing is not fully reliable yet** (see PLAN.md, "Open: flashing reliability"). Uploads sometimes fail partway through, and a failure after the erase step leaves the board unbootable (the ROM prints `invalid header: 0xffffffff`). The reliable path:

1. Hold BOOT, tap EN, release BOOT.
2. Run `tools/flash.sh --manual`, or `esptool --before no-reset --after watchdog-reset write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin` from `firmware/.pio/build/esp32s3`.
   For the C5 use `RX_ENV=esp32c5 tools/flash.sh`. The C5 bootloader goes at **0x2000**, not 0x0.

Use `--after watchdog-reset`: an RTS reset does not always leave download mode
(on the C5 it never does).

**ESP32-C5 (XIAO, chip rev v1.0):** uploads work hands-free (`RX_ENV=esp32c5
tools/flash.sh` or `pio run -e esp32c5 -t upload`). Rev v1.0 silicon has a ROM bug
([esp-idf#18089](https://github.com/espressif/esp-idf/issues/18089)): after a USB
(RTS) reset, the chip hangs with USB dead if UART0's clock was gated. The firmware
re-enables that clock at boot, so this only bites before our firmware is running,
e.g. on a board flashed with something else. Recover with BOOT + RESET, then
`RX_ENV=esp32c5 tools/flash.sh --manual`. If a boot step fails, the firmware prints
`ERR boot <step> <error> heap=… largest=…` every second instead of
reboot-looping.

Use the PlatformIO Core that pioarduino 55.03.312-1 expects (6.2.x). The
platform pins ESP-IDF 5.5.5. Before changing the platform version, run
`tools/check_phy_abi.sh esp32s3 25fe69f946311abdaf9ad56591f25fedbc20ac98 <new-idf-tag>`,
and the same with `esp32c5`. The firmware depends on private PHY functions, and
they must stay identical. Both targets pass against IDF 5.5.5.

**Web UI says "The device has been lost" right after connecting** (while the board
stays plugged in): an earlier pyserial user of the port, such as esptool,
`pio device monitor` or a script, left the tty at VMIN=0. Chrome (up to at least
153) reads that as a lost device. Fix: `stty -F /dev/ttyACM0 min 1`, or press
RESET. `tools/flash.sh`, `widscan58cli.py` and `serial_bridge.py` leave the port in
the right state (`tools/ttyfix.py`); `pio run -t upload` does not.

Ways back into the bootloader: esptool's `usb-reset` (automatic on upload), or
hold BOOT while pressing EN. The `BOOTLOADER` command left the board
unresponsive in testing; don't rely on it yet.

To restore the original WebSDR firmware:
`esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0 backup/websdr-s3-flash-4MB.bin`.

## Performance (measured)

ESP32-C5, all 8 channels (4 pair LOs), 4096 samples at 80 MS/s, 4 × 1024-point Welch:

| FFT | Hop | Updates per channel |
|---|---:|---:|
| S16R (default) | ~4.75 ms (tune 0.70, capture 0.12, DSP 3.9) | ~53 /s |
| S16 | ~6.4 ms (DSP 5.6) | ~39 /s |
| R4 / R2 (soft float, no FPU) | ~65 / ~67 ms | ~3.8 / ~3.7 /s |
| S16R, `FFT 256` (4 × 256-point, 312.5 kHz bins) | ~2.1 ms (DSP 1.2) | ~122 /s |
| S16, `FFT 256` | ~2.35 ms (DSP 1.6) | ~106 /s |

The tune includes the per-LO calibration cache restore (`RECALMODE CACHE`, the
C5 default); DSP is most of the hop (PLAN.md, Status). The DSP includes the
digital IQ correction (`IQCORR`, on by default): ~0.17 ms per hop at 1024
points, ~0.07 ms at 256. The R4/R2 rows predate it.

ESP32-S3:

| Setup | Updates per channel |
|---|---:|
| All 8 channels (4 pair LOs), S16R FFT (default) | ~64 /s |
| All 8 channels, R4 FFT | ~57 /s |
| One channel, or one neighbouring pair (1 LO: the tune is skipped) | ~445 /s (S16R), ~344 /s (R4) |

- A hop with 4 LOs is about 0.83 ms tune (median; bursty long tunes add ~15 %), 0.17 ms capture (4096 samples at 40 MS/s), and about 2.2 ms DSP with S16R.
- The display spectrum is sent every 4th hop by default (`SPEC`).
- Full tables and the FFT accuracy study are in PLAN.md.

## Useful commands

Send these with `tools/widscan58cli.py cmd …` or the UI's Protocol console.

| Command | Purpose |
|---|---|
| `INFO`, `STATUS?`, `PROFILE?`, `TIMING?`, `SPURS?` | Identity, full configuration (including `rawiq_max`), band profile, DSP time breakdown, masked board spurs |
| `CHANNELS <name>=<MHz> …`, `CHANNELS DEFAULT` | Channel plan: up to 8 channels by their 5.8 GHz frequency, e.g. `CHANNELS R1=5658 A8=5725` (the S3 maps them into its scaled band). Ids follow the order given; all are enabled. The UI's Channels → Edit sends this on every connect |
| `ENABLE <mask>`, `MODE GROUP\|SINGLE`, `SCAN START\|STOP` | Which channels are scanned, and how. `GROUP` (default) puts channels within 40 MHz under one LO (bands A, B and F take 3 LOs; R and E take 4), keeping it clear of every channel centre where it can; `SINGLE` gives each channel its own LO |
| `GAIN MANUAL <idx>\|HARDWARE`, `FILTER <code>\|AUTO` | RX gain and analog filter |
| `FFTTYPE R2\|R4\|S16\|S16R`, `FFTTYPE?` | FFT implementation. Default S16R (rounded 16-bit: esp-sdr's SIMD radix-2 on the S3, a radix-4 kernel elsewhere); R4 is exact float and best for step-on/IMD measurement. On the C5, float FFTs take ~68 ms/hop (no FPU) |
| `FFTEVAL SYNTH <noise×100> <tone_dBFS×10> <reps>`, `FFTEVAL LIVE <lo_kHz> <reps>` | Compare all FFT types on identical samples, against float R4 (R2 for sizes that are not a power of 4). A type that cannot load says `FFTEVAL … <type> ERR <error>` |
| `CAPN <samples>`, `FFT <n>` | Capture length and FFT size (S3: up to 2048; C5: up to 1024) |
| `SPEC <every_n_hops> <bins> <span_hz> MAX\|MEAN` | Display spectrum rate and resolution (default `SPEC 4 256 11300000 MAX`) |
| `RAWIQ <lo_kHz> <samples> [rate]` | One raw I/Q snapshot (8-bit, auto-scaled), sent as one frame. At most `rawiq_max` samples: 16380 on the S3, 9180 on the C5 (smaller link queue). The C5 tunes in whole MHz, so `lo_kHz` must be a multiple of 1000 there |
| `RATE <code>` | Sample rate. S3: 0/1/2 = 80/40/16 MS/s. C5: 0–5 = 80/40/20/10/8/4 MS/s |
| `IQCORR 0\|1` | Digital IQ imbalance correction before the FFT (default 1). Removes the mirror image of strong signals, e.g. R1 showing on R2 in pair mode. |
| `RECAL`, `RECALMODE ONCE\|EVERY\|CACHE`, `SETTLE <us>`, `TUNESKIP <mask>` | Calibration and tuning (debug). `CACHE` (the C5 default) calibrates each LO once and restores it on later visits; on the S3 it behaves like `EVERY`. `RECAL` clears the cache. |
| `AVG <shift>` | Averaging of the reported most-occupied frequency, centre and 99 % OBW: weight 2^-shift per hop, 4–8 (default 7: about 2.3 s at 56 hops/s, longer when fewer hops reach a channel) |
| `REBOOT` | Restart (USB stays connected; the UI re-syncs on `READY`) |

## Web UI

```sh
cd web
npm install
npm run dev                         # http://localhost:5808 (Web Serial: Chrome/Edge, Firefox, Android Chrome)
```

- **Connect device**: Web Serial. Chrome, Edge, Firefox and Android Chrome all
  work, flashing included. Chrome remembers the grant, so later visits and
  board resets reconnect automatically.
- **Flash device**: flashes the firmware the page was deployed with
  (`web/src/serial/flasher.ts`; binaries come from `web/public/firmware`, staged
  by CI, never committed).
- **Simulate**, or open `http://localhost:5808/?sim`: synthetic VTXs, Wi-Fi
  bursts, board spurs and IMD3 products. The simulator only has the S3
  (`s3-race24`) profile.
- **Bridge**: `python3 tools/serial_bridge.py`, then open
  `http://localhost:5808/?bridge=ws://127.0.0.1:5809`. This does not work from
  the HTTPS Pages site (Chrome blocks it from reaching a localhost WebSocket);
  use the local dev server.
- **Record / Replay**: saves the raw stream to a `.widscan58` file and plays it back.
  The frame layout is not kept compatible across firmware versions, so
  recordings only replay with a UI built from the same version.

Only one program can hold `/dev/ttyACM0` at a time: browser tab, bridge,
`widscan58cli.py` or esptool. A connected UI tab reconnects by itself when the
board comes back (after a flash or reset), so click Disconnect before using
other tools.

README screenshots (`images/`) were taken with headless Chrome
(puppeteer-core from `web/node_modules`) at a 1600 px wide viewport, against the
C5 through the bridge and the local dev server. `images/webui.png` is the top
1370 px of the full page; `images/channel-editor.png` is the `dialog.plan-dialog`
element after clicking Channels → Edit.

### Tests

```sh
(cd firmware && pio test -e native)        # firmware host tests: radix-4 FFT vs a DFT, metrics
cd web
npm test                                   # parser/protocol unit tests
node e2e/headless.mjs --sim                # headless Chrome vs. the simulator
python3 ../tools/serial_bridge.py &        # then, against the real board:
node e2e/headless.mjs                      # writes screenshots + results.json to e2e-out/
```
