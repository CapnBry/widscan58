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
| `tools/powercal.py` | Power calibration (dBFS → dBm): sweeps a VTX (Betaflight MSP) over channels and power levels on an ImmersionRC RF Power Meter v2 and on the C5 at each gain, then fits the offset per gain and channel. |
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
In RotorHazard mode, input bytes `A`–`P` and `T` (unused by RotorHazard) start one of our
command lines; only a few are allowed there (`rh_allowed`).

**Slim build:** `firmware/CMakeLists.txt` sets `COMPONENTS` to our component
(`src`) and PlatformIO's `__pio_env`, so only the ESP-IDF components that
`src/CMakeLists.txt` `REQUIRES` (and their dependencies) are built: 47 of 108
on the C5, a clean build ~18 s. A new IDF API needs its component added to
`REQUIRES`. Without Bluetooth and 802.15.4 in the build, Wi-Fi coexistence is
off. `sdkconfig.defaults` also drops SoftAP, WPA3-SAE and OWE (~145 KB of
flash): the Wi-Fi driver only runs in `WIFI_MODE_NULL` to bring up the PHY.
lwIP stays: IDF 5.5 can't build Wi-Fi without it (`esp_err_to_name.c`).
Turning off Wi-Fi enterprise support adds files: wpa_supplicant then builds
its own TLS instead of using mbedTLS.

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

**ADC rate on the C5:** the PHY runs 5 GHz at ADC rate 0 with no decimation
filter, and only above 5830 MHz at rate 1 with RX filter mode 8
(`phy_rfpll_set_adc_rate`). `adc_path_set` (`radio_c5.c`) puts every LO on
rate 1 + mode 8 after each tune: aliases from 50–60 MHz off the LO drop below
the noise (24–30 dB down at rate 0), and the odd 80 MHz board spurs (5680,
5840 MHz) disappear, at the same SNR. The cost is the skirt R7/R8 always had:
−3 dB at ±24 MHz, ~0.8 dB at a pair channel (PLAN.md, "The 5830/5831 MHz
skirt"). The analog filter code (`FILTER`) does not change the ADC rate. The
web UI's power calibration was made at rate 0; R1–R5 are lowered 0.5 dB for
it (`CALIBRATION`), an estimate until it is redone.

**Firmware version:** `INFO` (and RotorHazard's firmware version) reports
`PROJECT_VER`, the `git describe --tags --always --dirty` that
`firmware/CMakeLists.txt` takes at CMake configure time. PlatformIO only
reconfigures when `CMakeLists.txt` or `sdkconfig*` change, so incremental builds
used to keep an old hash; `firmware/version_check.py` (a pre-build script)
forces a reconfigure when `git describe` differs from the build's
`project_description.json`. That costs a CMake reconfigure (~7 s on the C5;
only `esp_app_format` and `commands.c` recompile) after each commit or
clean/dirty change. `INFO`'s fields are separated by `|`
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
~350 µs at `FFT 64`). Rotations (each enabled channel once) are capped at
1050 /s (`THROTTLE`; `MAX_ROTATION_HZ`, `scanner.c`): a rotation that ends early waits
for its 952 µs slot, blocking on a timer (the CPU idles) when at least 100 µs
are left, so a single LO at `FFT 64` (~0.28 ms hops) reads 1050 /s. The hop
times in the frames and telemetry leave the wait out. `main.c` clears
`PCR_CPU_WAIT_MODE_FORCE_ON` so the CPU clock stops in WFI (IDF leaves it set
on the C5). USB current at 5.23 V, RotorHazard mode, one seat (2026-10-17, with the cap at 1100 /s):
99.4 mA unthrottled (~3500 /s), 93.5 mA throttled, 93.3 mA throttled with
the clock gating, 90.4 mA with the scan stopped. The rest is the radio and
board, not the CPU: powering the RF down in the wait (`PHYIDLE 4`) gives
80.5 mA at 1100 /s (PLAN.md, "Rotation throttle and radio power-down").
Measure current with the meter's energy counter, not its 4 Hz samples, which
alias with the throttle's on/off cycle. The default row, the `METRICS SNR|RSSI` rows and the
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
| `CHANNELS <name>=<MHz> …`, `CHANNELS DEFAULT` | Channel plan: up to 8 channels by their 5.8 GHz frequency, e.g. `CHANNELS R1=5658 A8=5725`. The C5 takes 5345–5945 MHz (`tune_lo_khz`/`tune_hi_khz` in `profile.c`: bands A, B, E, F, R and low band L, 5362–5621 MHz; L is untested on hardware and has no spur list or power calibration); the S3 maps them into its scaled band, which has no room for L. The web UI's panorama spans the LO segments scanned (`bandRange`, `views/panorama.ts`; `band_khz`, 5645–5945 MHz, only before the first spectrum), so board spurs and channels outside the scanned channels' span are not shown. Ids follow the order given; all are enabled. The UI's Channels → Edit sends this on every connect |
| `ENABLE <mask>`, `MODE GROUP\|SINGLE`, `SCAN START\|STOP` | Which channels are scanned, and how. `GROUP` (default) puts channels within 40 MHz under one LO (bands A, B and F take 3 LOs; R, E and L take 4), keeping it clear of every channel center where it can; `SINGLE` gives each channel its own LO |
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
| `THROTTLE <hz>` | Rotation rate cap, 10–100000 or 0 (none); default 1050. Reported as `throttle_hz` in `STATUS?`/`CONFIG?`; each mode keeps its own, not saved by `CONFIG SAVE` |
| `PHYIDLE <depth> [min_us]`, `PHYIDLE?` | Debug, C5 (default 0, nothing): power the radio down while the throttle waits, for waits of at least `min_us` that fit the wake-up. 1 RX path (saves nothing), 2 RF with the full channel setup on wake (~0.6 ms), 3 also the modem clocks, 4 as 3 with a PLL-only retune (~0.38 ms; −12 mA at 1100 /s with one LO). The first power-down raises the gain ~1–2.4 dB until a reboot (PLAN.md). `PHYIDLE?` reports and resets the counts and times. Reported as `phy_idle`/`phy_idle_min_us` in `STATUS?`/`CONFIG?` |
| `REGWATCH 0\|1` | Debug, C5: read three modem register blocks (0x600a0000, 0x600a0800: 64 words each; 0x600a7000: 256, with the gain register) right before and after each capture, learn the bits that change by themselves for 5 s, then print `REGDIFF t=<ms> lo=<MHz> w\|x n=<count> <addr>:<old>><new> …` for other changes (`w`: during the capture, `x`: since the same LO's previous capture; 8 LOs, 2000 lines per enable). ~17 KB of heap, kept once used; off, no measurable cost (hop times unchanged). PLAN.md, "Level jumps behind a powered VTX" |
| `AVG <shift>` | Averaging of the reported most-occupied frequency, center and 99 % OBW: weight 2^-shift per hop, 4–8 (default 7: about 2.3 s at 56 hops/s, longer when fewer hops reach a channel) |
| `PROTOCOL MONITOR\|RH`, `PROTOCOL?` | Host protocol: our commands and frames (`MONITOR`), or RotorHazard's node protocol (`RH`, the boot default, silent until polled). Each mode keeps its own scan settings across switches, but every `PROTOCOL MONITOR` applies `METRICS FULL`, and every `PROTOCOL RH` (also a RotorHazard probe taking over, and boot) applies the saved RotorHazard defaults (`CONFIG SAVE`) |
| `CONFIG SAVE` | RotorHazard mode only (`ERR rh_only` otherwise): save the current gain, `FFT`, `METRICS RSSI\|SNR`, `MODE` and `LEVELFILTER` to NVS as the RotorHazard mode defaults, since RotorHazard's UI can't set them (the capture follows the FFT size). Without a save: `FFT 64` (256 samples), `METRICS RSSI`, `MODE GROUP`, `GAIN MANUAL 45` (as monitor mode), `LEVELFILTER MEDIAN` |
| `CONFIG RESET` | RotorHazard mode only: erase the saved RotorHazard mode defaults and apply the built-in ones |
| `CONFIG?` | `STATUS?` under another name (`CONFIG {…}`), allowed in RotorHazard mode. In RotorHazard mode only `PROTOCOL…`, `INFO`, `CONFIG?`, `CONFIG SAVE`, `CONFIG RESET`, `METRICS RSSI\|SNR\|?`, `GAIN …`, `FFT`, `FILTER`, `IQCORR`, `MODE GROUP\|SINGLE`, `LEVELFILTER …`, `RECAL`, `THROTTLE`, `PHYIDLE…` and `GPIO…` work; the rest answer `ERR rh_mode` |
| `LEVELFILTER MEDIAN\|EMA`, `LEVELFILTER?` | RotorHazard mode only (`ERR rh_only` otherwise; the query works in both): the seats' level filter. `MEDIAN` (default): the 255 ms running median, as RotorHazard's nodes. `EMA`: a median of 3, then an exponential average with a 2.72 ms time constant at any update rate (its weight comes from each update's interval), as FPVGateC5MK (alpha 0.3 at ~1030 /s). A change resets the seats' levels. Reported as `level_filter` in `STATUS?`/`CONFIG?` |
| `GPIO <n> NONE\|LOW\|HIGH`, `GPIO SAVE`, `GPIO?` | GPIO outputs set at boot, e.g. an antenna switch (Waveshare ESP32-C5-Zero: GPIO26). A change drives the pin at once (`NONE`: back to the reset state, pull-up on); `GPIO SAVE` makes the settings in effect the boot ones (NVS, base 3 per GPIO number, 40 per `u64` key `gpio0`, `gpio1`; `src/gpio_out.c`), applied right after `nvs_flash_init`. `GPIO?` answers `GPIO {"count":N,"active":"…","saved":"…"}`, one character per GPIO: `-` unused, `L`, `H`, `x` not available (USB, and the flash pins IDF reserves). Works in both modes |
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
  monitor mode (gain sweeps with a 25 mW VTX at 0.9 m, no antenna on the
  board: PLAN.md, "Gain sweep"). `CONFIG SAVE` replaces the gain, FFT, metric, LO plan and level filter defaults (NVS blob version 3, `commands.c`; versions 1
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
  change), and resets the seat, with every seat already on that frequency,
  when the scanner applies the new plan (`rh_plan_applied`): seats on one
  frequency (`MODE GROUP`) get the same updates from the same point, so
  they report identical passes. The server timestamps each seat by its own
  read (read midpoint − ms since the pass, whole ms), so such seats still
  differ by its read jitter and the ms rounding: ~2 ms per lap
  (`rh_check.py --freq`). `LEVELFILTER EMA` replaces the median with a median of 3 and an
  exponential average (time constant `LAP_EMA_TAU_US`, 2.72 ms; the weight
  1 − exp(−dt/τ) per update from its interval, as an integer Padé
  approximant), reported from the first update with the middle update's time
  less ~3 ms of average delay.
- **Check:** `tools/rh_check.py` runs RotorHazard's own interface code against
  the board (no VTX: thresholds inside the filtered noise make noise
  crossings; at manual gain 60 the seats sit at codes ~79–83, drifting with
  temperature, and `--enter 82 --exit 82` gave ~4 passes/s; 2026-10-23).
  `--freq <MHz>` puts every seat on one frequency and reports how far apart
  their pass and lap times are, from events 300 ms clear of others (noise
  passes closer than the 100 ms poll merge differently per seat). 60 s,
  8 seats: lap spread median 2 ms, max 2 ms (2 / 3 ms before same-frequency
  seats were reset together; 1 / 2 ms with µs lap times too, not kept).
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

- **Menu** (☰, top left): Monitor mode / RotorHazard mode / Power meter mode, GPIO Configure,
  and the dark-mode switch. The choice is remembered and applied on every
  connect (`PROTOCOL?`, then `PROTOCOL MONITOR|RH`; firmware without
  `PROTOCOL` is monitor only; power meter mode runs the monitor protocol). GPIO Configure (`views/gpioDialog.ts`,
  `model/gpio.ts`) shows each GPIO's setting (not its live level) as Not
  used / Low / High; a click sends `GPIO <n> …` (in effect at once) and
  reads `GPIO?` back. Pins whose setting differs from the saved one show
  "saved: …", with a note that they need Save (`GPIO SAVE`) to survive a
  restart; Revert puts the saved ones back.
  An About section below them links to the GitHub project and shows the
  connected device's firmware version (from `INFO`; over Web Serial also a
  newer version the page can flash).
- **RotorHazard mode page** (`web/src/model/rh.ts`, `views/rh.ts`): polls the
  board as RotorHazard's server does (every 100 ms: per seat, select it and read
  the pass stats and one queued peak/nadir), and shows each seat's level with
  EnterAt/ExitAt (editable, "now" = average of the last second, kept in
  localStorage and written on connect; a metric change leaves them as they
  are), a level graph with passes, the detection events with lap times, and
  the receiver settings this mode allows. Laps (`web/src/model/laps.ts`): a
  pass under the minimum lap (8 s) after the last counted one is listed as
  ignored, and each seat keeps its fastest 3 consecutive laps (tile, and an
  event row for each new record). Clear, a seat's channel change, a device
  restart and a reconnect break a run (no lap across them); only Clear drops
  the fastest. The gear by Clear opens the events options
  (`views/rhEventsDialog.ts`). Display (saved in `UiSettings`): list
  enter/exit events, list new fastest runs (the tiles show them either way),
  and Keep screen on (a screen wake lock while any device is connected,
  `util/wakeLock.ts`; greyed out without `navigator.wakeLock`). Lap
  processing (`RhModel.lapRules`, saved with the thresholds; a change
  recomputes from the passes since Clear): minimum lap on/off and 0–999 s,
  fastest consecutive laps on/off and 2–99 laps (off: no tile row, no
  records). Voice callouts (`util/speech.ts`, the browser's speech
  synthesis, a Google voice when there is one; saved in `UiSettings`): each
  lap as channel and seconds to 0.1 ("R4 13.7"), with volume, rate, pitch
  and a Test button; hidden while off, an error line without speech
  synthesis.
  The page takes the metric and receiver settings from the device (`CONFIG?`)
  and never sets them on connect; Receiver (gain, LO plan, FFT size, output
  value, output filter) → Save sends `CONFIG SAVE`, Reset `CONFIG RESET`. The
  output filter shows Median for firmware without `level_filter` in `CONFIG?`.
  Seats take the channels of the shared channel plan in frequency order (Edit
  channels). Disconnect before a RotorHazard server opens the port.
- **Power meter mode** (`web/src/model/power.ts`, `views/power.ts`): the
  power at a VTX's connector, through an attenuator, in dBm and mW per
  channel; only as accurate as the attenuation entered (attenuators often
  differ from their rating: a "30 dB" chain measured ~25 dB at 5.8 GHz), so
  the warning dialog presents it as a relative reading for the user's setup.
  It sets the scan the calibration was made with (`MODE SINGLE`,
  `GAIN MANUAL 0`, `WIN 15000000 15000000`, `SPEC 0 …`) and puts monitor
  mode's windows and spectrum back on leaving (the channel plan, LO mode and
  gain come back with the stored device settings). Per channel: the mean
  power over the smoothing time (Off, 0.1–3 s, all channels; `UiSettings`),
  less the window's noise, both leaving out the captures that clipped (OVER
  when more than half of the window clipped; with smoothing Off, when the
  latest capture clipped), plus the calibration offset (`CALIBRATION`, dBm at
  the board's input − dBFS at gain 0, by MHz) and the attenuation the user
  enters. "Over range" above −5 dBm at the input,
  "uncalibrated" outside the calibrated frequencies. Only `c5-race` has a
  calibration: R1–R8, about ±1 dB (2026-10-19; PLAN.md, "Power
  calibration"; R1–R5 lowered 0.5 dB since for ADC rate 1, unmeasured). The page opens with the attenuator warning and size
  table (`ATTENUATOR_TABLE`); below the channel cards a yellow note says the
  values are approximate, from a calibration with a single C5 (keep it short,
  without numbers). `tools/powercal.py` measures the offsets.
- **Installable / offline** (PWA): `public/manifest.webmanifest`, icons from
  `public/icon.svg` and `icon-maskable.svg` (PNGs made with `rsvg-convert`,
  committed), `sw.js` and `src/util/pwa.ts`. The build (`vite.config.ts`,
  `pwaPlugin`) writes `dist/sw.js` with the page's files (`FILES`: public files
  and the entry's static imports), the rest (`FLASH_FILES`: the firmware and
  the flasher's code, esptool-js and its chip stubs) and a cache name from a
  hash of all of them, so a firmware-only deploy updates the app too.
  `FLASH_FILES` are fetched when flashing (~1 MB) and kept for offline use
  until the next deploy. In the installed app (`isInstalledApp()`: display-mode
  standalone etc.), the flash dialog has "Make firmware available offline": it
  downloads them now and with every update (cache `…-fw`; the setting is the
  `widscan58-prefs` cache, which the worker can read). Offline without the
  firmware, the dialog says so and disables Flash; a download that fails while
  flashing (`OfflineError`) says so too. Page loads are network first:
  `index.html` is fetched with `cache: "no-cache"` (Pages sends max-age=600)
  and a 3 s timeout (`NAV_TIMEOUT_MS`), the cached one only when that fails,
  so a deploy shows on the first load after it (it used to show on the second:
  the cached page loaded while the new worker installed). The new worker
  installs silently in the background; there is no install or update prompt,
  keep it that way. Only `npm run build` output registers it (not the dev server), so
  test with `npx vite preview`.
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
