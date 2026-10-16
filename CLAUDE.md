# Widscan58: developer notes

Technical reference for working on this repo: layout, build, flashing, tools,
device commands and tests. The user-facing description is in
[README.md](README.md); the design, measurements and status log are in
[PLAN.md](PLAN.md).

FPV channel scanner using the ESP32's built-in radio as an SDR, through the raw
I/Q debug path found by [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr).
The firmware steps through up to eight FPV channels and reports RSSI, noise
floor, SNR, peak and video-center frequency per channel, plus a decimated
spectrum. A Web Serial dashboard shows it all live. In RotorHazard mode the
board is a RotorHazard timer node instead: one USB port with a seat per channel
(see "RotorHazard mode" below).

Targets: the **ESP32-C5** (real 5.8 GHz) runs on a Seeed XIAO ESP32-C5 and works
with the web UI: ~4.0 ms/hop, about 60 rotations/s over R1–R8 (PLAN.md, Status,
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
| `tools/rh_check.py` | Runs RotorHazard's own node interface code (from `ref/RotorHazard`) against the board: discovery, seats, polling, passes, comm errors. |
| `tools/gen_twiddle.py` | Generates `firmware/src/dsp/fft_twiddle_q15.h` for the radix-4 FFT. |
| `.github/workflows/` | Builds firmware + web UI and publishes to GitHub Pages (https://capnbry.github.io/widscan58/). Only `esp32c5` firmware is published (`FW_ENVS`). |
| `images/` | Screenshots used by README.md. |
| `ref/esp-sdr/` | Reference checkout of esp-sdr (not tracked here). Restore it with `git clone https://github.com/ESPARGOS/esp-sdr ref/esp-sdr`. |
| `ref/RotorHazard/` | Reference checkout of RotorHazard for `tools/rh_check.py` (not tracked): `git clone https://github.com/RotorHazard/RotorHazard ref/RotorHazard`. Its license (MIT with no-AI-training terms) differs from ours: interoperate with it, don't copy its code. |
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
`ERR boot <step> <error> heap=… largest=…` every second (once a host has sent
something) instead of reboot-looping.

**The firmware boots silent, in RotorHazard mode** (`firmware/src/proto/commands.c`):
it sends nothing until asked, as a RotorHazard server expects of a node, and
there is no `READY` line any more. Our hosts switch with `PROTOCOL MONITOR`
(the web UI, `widscan58cli.py` and `tools/flash.sh` do this themselves; with
`pio device monitor` or a terminal, type it). In monitor mode a lone RotorHazard
probe byte (0x22) after 100 ms of input silence switches back to RotorHazard
mode, so a RotorHazard server can take over a port a monitor left streaming.
In RotorHazard mode, input bytes `A`–`P` (unused by RotorHazard) start one of our
command lines; only a few are allowed there (`rh_allowed`).

Use the PlatformIO Core that pioarduino 55.03.312-1 expects (6.2.x). The
platform pins ESP-IDF 5.5.5. Before changing the platform version, run
`tools/check_phy_abi.sh esp32s3 25fe69f946311abdaf9ad56591f25fedbc20ac98 <new-idf-tag>`,
and the same with `esp32c5`. The firmware depends on private PHY functions, and
they must stay identical. Both targets pass against IDF 5.5.5.

**Scan loop in IRAM (C5):** the code each hop runs is in IRAM, so the hop time
no longer depends on code placement; from flash it moved by up to ~10 % with
unrelated changes (flash cache misses; PLAN.md, Status). Ours is `IRAM_ATTR`
in the sources; the libm helpers and the PLL-only tune's PHY functions are
placed by `src/radio/c5/hot_iram.lf`. When a hot-path function is added, mark
it too. Static RAM below the RF dump bank (0x40820000) is down to a few hundred
bytes: a new static variable or IRAM function can fail the link with "RF dump
bank overlaps BSS" (`sram_guard.ld`). The scanner state and command reply
buffer are on the heap; the cal cache and gain tables are in the dump bank's
lower half (`radio_c5.c`, `RADIO_RAM`). PlatformIO does not regenerate its
`sections.ld` when a `.lf` file changes: delete
`.pio/build/esp32c5/sections.ld` after editing one.

**Firmware version:** `INFO` (and RotorHazard's firmware version) reports
`PROJECT_VER`, the `git describe --tags --always --dirty` that
`firmware/CMakeLists.txt` takes at CMake configure time. PlatformIO only
reconfigures when `CMakeLists.txt` or `sdkconfig*` change, so incremental builds
used to keep an old hash; `firmware/version_check.py` (a pre-build script)
forces a reconfigure when `git describe` differs from the build's
`project_description.json`. That costs one near-full rebuild (~25 s on the C5)
after each commit or clean/dirty change. `INFO`'s fields are separated by `|`
(`widscan58|<target>|<proto>|<version>|<commit time>|<PHY version>`, the PHY
version has spaces). After the version, `INFO` has HEAD's
commit time in ISO 8601 (`git log -1 --format=%cI`, taken at the same time;
only `commands.c` sees it). The date at the end of `INFO` belongs to the PHY
library (`radio_phy_version()`), not to our build.

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
| S16R (default) | ~4.0 ms (tune 0.23, DSP 3.65) | ~60 /s |
| S16 | ~6.4 ms (DSP 5.6) | ~39 /s |
| R4 / R2 (soft float, no FPU) | ~65 / ~67 ms | ~3.8 / ~3.7 /s |
| S16R, `FFT 256` (4 × 256-point, 312.5 kHz bins) | ~2.1 ms (DSP 1.2) | ~122 /s |
| S16, `FFT 256` | ~2.35 ms (DSP 1.6) | ~106 /s |
| S16R, `FFT 128`, `CAPN 512` (4 × 128-point, 625 kHz bins), `SPEC 0` | ~1.69 ms (DSP 0.90) | ~143 /s |
| the same, `METRICS SNR` | ~0.73 ms (tune 0.23, DSP 0.47) | ~318 /s |
| the same, `METRICS RSSI` | ~0.71 ms (tune 0.23, DSP 0.45) | ~332 /s |
| `METRICS RSSI`, `FFT 128`, `SEGMENTS 2`, `CAPN 256` | ~0.96 ms (DSP 0.35) | ~248 /s |
| `METRICS RSSI`, `FFT 64`, `CAPN 256` (4 × 64-point, 1.25 MHz bins) | ~0.51 ms (tune 0.23, DSP 0.25) | ~464 /s |
| `METRICS RSSI`, `FFT 64`, `SEGMENTS 8`, `CAPN 512` | ~1.04 ms (DSP 0.45) | ~227 /s |

The tune includes the per-LO calibration cache restore (`RECALMODE CACHE`, the
C5 default); DSP is most of the hop (PLAN.md, Status). Each hop tunes the next
hop's LO right after its capture, so the settle time (`SETTLE`, 100 µs) runs
during the DSP; the capture only waits for what is left of it (none below
~350 µs at `FFT 64`). The default row, the `METRICS SNR|RSSI` rows and the
`FFT 64` row are from 2026-10-13, with the PLL-only retune (`TUNESKIP 3`)
and the scan loop in IRAM (below); the others are older, with a full channel
setup every hop (tune ~0.5–0.7 ms) and the scan loop in flash. The DSP includes the
digital IQ correction (`IQCORR`, on by default): ~0.17 ms per hop at 1024
points, ~0.07 ms at 256, ~0.04 ms at 4 × 128 (its coefficients are worked
out once per capture). The R4/R2 rows predate it. With `METRICS RSSI|SNR` the
per-channel work drops from ~0.27–0.4 ms to ~0.035 / ~0.05 ms per hop, and the
tune measures ~0.15 ms shorter (not yet explained; the frames are much smaller).

ESP32-S3:

| Setup | Updates per channel |
|---|---:|
| All 8 channels (4 pair LOs), S16R FFT (default) | ~64 /s |
| All 8 channels, R4 FFT | ~57 /s |
| One channel, or one neighbouring pair (1 LO: the tune is skipped) | ~445 /s (S16R), ~344 /s (R4) |

- A hop with 4 LOs is about 0.83 ms tune (median; bursty long tunes add ~15 %), 0.17 ms capture (4096 samples at 40 MS/s), and about 2.2 ms DSP with S16R.
- The display spectrum is sent every 4th hop by default (`SPEC`), one hop later each time the LOs would repeat: every 5th hop with 4 LOs.
- Full tables and the FFT accuracy study are in PLAN.md.

## Useful commands

Send these with `tools/widscan58cli.py cmd …` or the UI's Protocol console.

| Command | Purpose |
|---|---|
| `INFO`, `STATUS?`, `PROFILE?`, `TIMING?`, `SPURS?` | Identity, full configuration (including `rawiq_max`), band profile, DSP time breakdown, masked board spurs |
| `CHANNELS <name>=<MHz> …`, `CHANNELS DEFAULT` | Channel plan: up to 8 channels by their 5.8 GHz frequency, e.g. `CHANNELS R1=5658 A8=5725` (the S3 maps them into its scaled band). Ids follow the order given; all are enabled. The UI's Channels → Edit sends this on every connect |
| `ENABLE <mask>`, `MODE GROUP\|SINGLE`, `SCAN START\|STOP` | Which channels are scanned, and how. `GROUP` (default) puts channels within 40 MHz under one LO (bands A, B and F take 3 LOs; R and E take 4), keeping it clear of every channel center where it can; `SINGLE` gives each channel its own LO |
| `GAIN MANUAL <idx>\|HARDWARE`, `FILTER <code>\|AUTO` | RX gain and analog filter |
| `FFTTYPE R2\|R4\|S16\|S16R`, `FFTTYPE?` | FFT implementation. Default S16R (rounded 16-bit: esp-sdr's SIMD radix-2 on the S3, a radix-4 kernel elsewhere); R4 is exact float and best for step-on/IMD measurement. On the C5, float FFTs take ~68 ms/hop (no FPU) |
| `FFTEVAL SYNTH <noise×100> <tone_dBFS×10> <reps>`, `FFTEVAL LIVE <lo_kHz> <reps>` | Compare all FFT types on identical samples, against float R4 (R2 for sizes that are not a power of 4). A type that cannot load says `FFTEVAL … <type> ERR <error>` |
| `CAPN <samples>`, `FFT <n>` | Capture length (from 256) and FFT size (from 64; S3: up to 2048; C5: up to 1024). In RotorHazard mode `CAPN` is refused: the capture is always 4 × the FFT size |
| `METRICS FULL\|RSSI\|SNR`, `METRICS?` | Per-channel measurements. `FULL` (default): everything, `METRICS` frames. `RSSI`: only the RSSI-window power; `SNR`: that plus noise floor and SNR. Both send compact `LEVELS` frames of byte codes (integer math per channel, for lap timing); the UI leaves the rest blank. They do not mask board spurs (at `FFT 128` the mask would cover ~1.9 MHz of R8 on the C5; the unmasked spur adds ~2 dB to the empty channel) |
| `SPEC <every_n_hops> <bins> <span_hz> MAX\|MEAN` | Display spectrum rate and resolution (default `SPEC 4 256 <usable span> MAX`: 40000000 on the C5, 11300000 on the S3). Spectra are N hops apart, N+1 each time the LO sequence would repeat (when N shares a factor with the number of LOs), so they cycle through all LOs |
| `RAWIQ <lo_kHz> <samples> [rate]` | One raw I/Q snapshot (8-bit, auto-scaled), sent as consecutive 1024-sample `RAWIQ` frames (header: total `n` and this chunk's `offset`). At most `rawiq_max` samples, the capture buffer: 16380 on both targets. The C5 tunes in whole MHz, so `lo_kHz` must be a multiple of 1000 there |
| `RATE <code>` | Sample rate. S3: 0/1/2 = 80/40/16 MS/s. C5: 0–5 = 80/40/20/10/8/4 MS/s |
| `IQCORR 0\|1` | Digital IQ imbalance correction before the FFT (default 1). Removes the mirror image of strong signals, e.g. R1 showing on R2 in pair mode. |
| `RECAL`, `RECALMODE ONCE\|EVERY\|CACHE`, `SETTLE <us>`, `TUNESKIP <mask>` | Calibration and tuning (debug). `CACHE` (the C5 default) calibrates each LO once and restores it on later visits; on the S3 it behaves like `EVERY`. `RECAL` clears the cache. `SETTLE` overlaps the DSP: the next capture waits for what is left of it. `TUNESKIP` bit 0 retunes only the PLL between full tunes (S3 default 7, C5 default 3; `TUNESKIP 0` runs the C5's full channel setup every hop), bit 1 skips the RX path re-apply, bit 2 the gain re-apply (wrong on the C5: +27 dB). Reported as `tskip` in `STATUS?`/`CONFIG?`, and set from the UI's Scanner controls |
| `AVG <shift>` | Averaging of the reported most-occupied frequency, center and 99 % OBW: weight 2^-shift per hop, 4–8 (default 7: about 2.3 s at 56 hops/s, longer when fewer hops reach a channel) |
| `PROTOCOL MONITOR\|RH`, `PROTOCOL?` | Host protocol: our commands and frames (`MONITOR`), or RotorHazard's node protocol (`RH`, the boot default, silent until polled). Each mode keeps its own scan settings across switches, but every `PROTOCOL MONITOR` applies `METRICS FULL`, and every `PROTOCOL RH` (also a RotorHazard probe taking over, and boot) applies the saved RotorHazard defaults (`CONFIG SAVE`) |
| `CONFIG SAVE` | RotorHazard mode only (`ERR rh_only` otherwise): save the current gain, `FFT`, `METRICS RSSI\|SNR`, `MODE` and `LEVELFILTER` to NVS as the RotorHazard mode defaults, since RotorHazard's UI can't set them (the capture follows the FFT size). Without a save: `FFT 64` (256 samples), `METRICS RSSI`, `MODE GROUP`, `GAIN MANUAL 45` (as monitor mode), `LEVELFILTER MEDIAN` |
| `CONFIG RESET` | RotorHazard mode only: erase the saved RotorHazard mode defaults and apply the built-in ones |
| `CONFIG?` | `STATUS?` under another name (`CONFIG {…}`), allowed in RotorHazard mode. In RotorHazard mode only `PROTOCOL…`, `INFO`, `CONFIG?`, `CONFIG SAVE`, `CONFIG RESET`, `METRICS RSSI\|SNR\|?`, `GAIN …`, `FFT`, `FILTER`, `IQCORR`, `MODE GROUP\|SINGLE`, `LEVELFILTER …` and `RECAL` work; the rest answer `ERR rh_mode` |
| `LEVELFILTER MEDIAN\|EMA`, `LEVELFILTER?` | RotorHazard mode only (`ERR rh_only` otherwise; the query works in both): the seats' level filter. `MEDIAN` (default): the 255 ms running median, as RotorHazard's nodes. `EMA`: a median of 3, then an exponential average with a 2.72 ms time constant at any update rate (its weight comes from each update's interval), as FPVGateC5MK (alpha 0.3 at ~1030 /s). A change resets the seats' levels. Reported as `level_filter` in `STATUS?`/`CONFIG?` |
| `REBOOT` | Restart (USB stays connected). The board comes back silent in RotorHazard mode; the web UI notices the missing telemetry and switches it back to monitor mode |

## RotorHazard mode

The board answers RotorHazard's serial node protocol (node API level 36) as one
multi-node processor with 8 seats, like an S32_BPill board with 8 receivers.
RotorHazard's server needs no changes: add the port to `GENERAL` →
`SERIAL_PORTS` in its config (e.g. `"SERIAL_PORTS": ["/dev/ttyACM0"]`).

- **Firmware:** `src/proto/rh.c` (protocol; written from RotorHazard's
  server-side code, `src/interface/serial_node.py` and `RHInterface.py`) and
  `src/scan/lap.c` (crossing detection per seat, host-tested). A seat set to an
  unknown frequency (RotorHazard writes 1111 for "off") is left out of the scan
  plan. The capture is always 4 × the FFT size (4 segments; `CAPN` answers
  `ERR rh_mode`). The defaults in this mode: `FFT 64` (256 samples),
  `METRICS RSSI`, no display spectrum; with R1–R6 that is ~1.5 ms between
  updates of a seat (~660 /s; R1–R8 ~1.96 ms, ~510 /s; `rh_check.py`'s
  `loop_time`, docs/RotorHazard.md). The default gain is manual 45, as in
  monitor mode (gain sweeps with a 25 mW VTX at 0.9 m: PLAN.md, "Gain
  sweep"). `CONFIG SAVE` replaces the gain, FFT, metric, LO plan and level filter defaults (NVS blob version 3, `commands.c`; versions 1
  and 2 load with the median filter); they are applied on every switch to this
  mode.
- **Levels** are the `METRICS` byte codes, in 0.25 dB steps: RSSI (dBFS =
  code / 4 − 63.75) or SNR (dB = code / 4), clamped to 1–254. The RSSI range
  starts just below the ADC noise in the ±5 MHz window (about −59 to −62
  dBFS, codes ~13–21, at any gain up to ~40; ~21–35 at the default 45,
  ~27–41 at 50, ~65 at 60). `METRICS
  RSSI|SNR` picks one and resets the seats' levels. EnterAt/ExitAt are in the
  same codes (defaults before the server writes its own: 125 / 105, −32.5 /
  −37.5 dBFS). The `LEVELS` frame's per-bin floor stays a 0.5 dB spectrum
  code (code / 2 − 127.5): per-bin noise is below −63.75 dBFS.
- **Detection:** a running median over the last 255 ms (at most 255 updates,
  so shorter above 1000 updates/s), as RotorHazard's own nodes use (255
  samples at ~1 kHz); nothing is reported for the first 255 ms after a reset.
  It comes from a histogram of the level codes, with a timestamp every 8th
  update; the seats are on the heap (~7.6 KB for 8), since the C5's static RAM
  below the RF dump bank is nearly full (scan loop in IRAM). Then a crossing from EnterAt
  until below ExitAt, and the pass at the middle of the crossing's highest
  plateau. Peaks
  and nadirs (4-code, 1 dB hysteresis, 8 queued each) feed the server's RSSI graph.
  A frequency change keeps the lap count (the server counts passes by its
  change). `LEVELFILTER EMA` replaces the median with a median of 3 and an
  exponential average (time constant `LAP_EMA_TAU_US`, 2.72 ms; the weight
  1 − exp(−dt/τ) per update from its interval, as an integer Padé
  approximant), reported from the first update with the middle update's time
  less ~3 ms of average delay.
- **Check:** `tools/rh_check.py` runs RotorHazard's own interface code against
  the board (no VTX: thresholds inside the filtered noise make noise
  crossings; at manual gain 60 the seats sit at codes ~64–71 and `--enter 67
  --exit 67` gave 239 passes in 30 s).
- **Testing in RotorHazard mode:** `PROTOCOL RH` applies the user's saved
  defaults, so after switching, *set* `MODE`, `FFT`, `GAIN`,
  `METRICS` and `LEVELFILTER` to what the test needs, but never `CONFIG SAVE` them (or `CONFIG RESET`). Leave them
  as the test set them afterwards; there is no need to restore anything.
  RotorHazard's port open (DTR and RTS cleared, as `rh_check.py` does through
  its code) resets the C5, which boots with the saved defaults: settings
  sent before `rh_check.py` connects are lost. Give them to it instead:
  `--cmd "METRICS RSSI" --cmd "GAIN MANUAL 60"` (sent after discovery, on its
  own port; it stops on an `ERR` reply and refuses `CONFIG SAVE` and `CONFIG RESET`).

## Web UI

```sh
cd web
npm install
npm run dev                         # http://localhost:5173 (Web Serial: Chrome/Edge, Firefox, Android Chrome)
```

- **Menu** (☰, top left): Monitor mode / RotorHazard mode, and the dark-mode
  switch. The choice is remembered and applied on every connect (`PROTOCOL?`,
  then `PROTOCOL MONITOR|RH`; firmware without `PROTOCOL` is monitor only).
  An About section below them links to the GitHub project and shows the
  connected device's firmware version (from `INFO`; over Web Serial also a
  newer version the page can flash).
- **RotorHazard mode page** (`web/src/model/rh.ts`, `views/rh.ts`): polls the
  board as RotorHazard's server does (every 100 ms: per seat, select it and read
  the pass stats and one queued peak/nadir), and shows each seat's level with
  EnterAt/ExitAt (editable, "now" = average of the last second, kept in
  localStorage and written on connect; a metric change leaves them as they
  are), a level graph with passes, the detection events with lap times, and
  the receiver settings this mode allows.
  The page takes the metric and receiver settings from the device (`CONFIG?`)
  and never sets them on connect; Receiver (gain, LO plan, FFT size, output
  value, output filter) → Save sends `CONFIG SAVE`, Reset `CONFIG RESET`. The
  output filter shows Median for firmware without `level_filter` in `CONFIG?`.
  Seats take the channels of the shared channel plan in frequency order (Edit
  channels). Disconnect before a RotorHazard server opens the port.
- **Connect device**: Web Serial. Chrome, Edge, Firefox and Android Chrome all
  work, flashing included. Chrome remembers the grant, so later visits and
  board resets reconnect automatically.
- **Flash device**: flashes the firmware the page was deployed with
  (`web/src/serial/flasher.ts`; binaries come from `web/public/firmware`, staged
  by CI, never committed).
- **Simulate**, or open `http://localhost:5173/?sim`: synthetic VTXs, Wi-Fi
  bursts, board spurs and IMD3 products. It simulates the C5 (`c5-race`:
  80 MS/s, whole-MHz LOs, the C5's measured spurs); `?sim=s3` simulates the
  S3's scaled 2.4 GHz band (`s3-race24`) instead, for the Simulate button
  too. It boots in RotorHazard mode like the firmware, and
  there flies a pass past every seat every 5–9 s.
- **Bridge**: `python3 tools/serial_bridge.py`, then open
  `http://localhost:5173/?bridge=ws://127.0.0.1:5809`. This does not work from
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
(cd firmware && pio test -e native)        # firmware host tests: radix-4 FFT vs a DFT, metrics, lap detection
cd web
npm test                                   # parser/protocol unit tests
node e2e/headless.mjs --sim                # headless Chrome vs. the simulator
python3 ../tools/serial_bridge.py &        # then, against the real board:
node e2e/headless.mjs                      # writes screenshots + results.json to e2e-out/
```

The e2e run covers both modes (switching through the menu) and recovery after
`REBOOT`. For RotorHazard compatibility, `tools/rh_check.py` (see "RotorHazard
mode").
