# Widscan58 — ESP32 FPV Spectrum / RSSI Scanner (C5 @ 5.8 GHz, S3 @ 2.4 GHz)

**License:** GPL-3.0-or-later. The project builds on code from
[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr) (GPL-3.0-or-later). Every
file copied or derived from it keeps its notice and gets a header naming the source.

The project has two parts:

1. **Firmware** (PlatformIO + ESP-IDF). It reads raw I/Q samples through the ADC/DAC
   debug bypass that esp-sdr found, steps through a set of FPV channels one after
   another (round robin), and reports per channel: RSSI, noise floor, SNR, and the
   estimated center of the video signal. It also streams a reduced spectrum for
   display. Two targets share one codebase:
   - **ESP32-C5:** the real target. Raceband R1–R8 at 5.8 GHz.
   - **ESP32-S3:** a **scaled mock-up** of the C5 system in the 2.4 GHz ISM band.
     We have S3 hardware now, so the S3 is where the pipeline, protocol and UI get
     debugged while we wait for the C5 devkits.
2. **Web UI** (Web Serial): live per-channel signal strength, channel enable/disable,
   and debug/spectrum views (band panorama, waterfall, per-channel zoom, timing and
   health telemetry, and later intermodulation (IMD) statistics).

**Main design rule:** everything band-specific (channel centers, window widths,
LO plan, filter, sample rate, display span) lives in a **band profile** that the
device sends to the UI. Firmware DSP and the web UI work only in Hz from that
profile. The S3 and C5 then run the *same* scanner, metrics, protocol and UI code;
only the radio backend and the profile differ.

---

## 1. What esp-sdr gives us (findings from reading the repo)

Reference commit: `06a5ca4`. Both targets are pinned to ESP-IDF `25fe69f9…`
(IDF **6.2.0-dev**, master 2026-07-11) in `firmware-targets.json`. The private PHY
ABI they use only applies to the `libphy` shipped with that commit.

| Item | ESP32-C5 | ESP32-S3 |
|---|---|---|
| Source | `families/c5_c6_c61/receiver.c`, `targets/esp32c5/*` | `targets/esp32s3/receiver.c` (snapshot `capture()`), `common/ring_capture.c` (continuous ring) |
| Capture trigger | `adctrig(n-1,0,0,divider*2,…)` from the PHY cert-test library | Direct register writes: control `0x60033d5c` (bit 19 trigger, bit 18 done), pack config `0x60033d90`, SRAM owner `0x600c101c` |
| Buffer | `0x40830000`; 128 KiB bank `0x40820000–0x40840000` reserved | `0x3fcd0000`; linker guards keep data/bss below `0x3fcb0000` |
| Max snapshot | 16380 complex samples | 16380 complex samples |
| Sample word | I bits 0–9, Q bits 10–19 (signed 10-bit), gain metadata in bits 20+ | same |
| Sample rates | 80/40/20/10/8/4 MS/s | 80 / 40 (bit 15) / 16 (bit 16) MS/s |
| Analog filter | BBTOP `0x67` regs 6/7. Mode 0: 11–23 MHz. Mode 1: 24–48 MHz or open | BBTOP `0x67` host 0 regs 4/5, **13–69 MHz** (code 60 ≈ 13 MHz, code 0 ≈ 69 MHz) |
| Tuning | `rx_recalibrate()` + `phy_set_chanfreq(mhz, mode)` | `rx_recalibrate()` + `set_chanfreq(2412,0)` + `set_rf_freq_offset(0, mhz, offset_khz)`. **Steps in kHz** (the `FOFS` mechanism) |
| Gain | `force_rx_gain(manual, idx, 0)`; indices are not dB | same; max index ≤ 82 |
| Spectrum | Portable Q15 FFT (`spectrum_fft.h`) | SIMD FFT `s3_fft_rnd.S`; second core used as a bare worker (`CONFIG_FREERTOS_UNICORE=y`) |
| Continuous mode | No (snapshot only) | Yes: 3-bank ring at 16/40/80 MS/s (not needed for v1) |

Shared: newline-terminated ASCII commands plus CRC32 binary frames over USB
Serial/JTAG (`burst_serial.c`), Hann window + DC tracker (`spectrum_dc.h`), 0.5 dB
log-power encoder (`spectrum_math.h`). Retuning reruns RX DC/IQ calibration and is
the main per-hop cost on both chips (§4.4).

## 2. Band profiles

### 2.1 C5 — Raceband (real)

R1–R8 (MHz): 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917. They are 37 MHz apart.

Analog FM video occupies about 15–20 MHz of RF (Carson's rule). **6.5 MHz is the
audio subcarrier offset, not the RF width.** Most of the energy lies within ±4–6 MHz
of the carrier.

| Parameter | C5 value |
|---|---|
| Sample rate | 80 MS/s |
| Analog filter | mode 1, code 0 (≈ 48 MHz full width) |
| Usable span per capture | ±24 MHz (filter) inside ±40 MHz Nyquist |
| RSSI integration window | ±5 MHz |
| Center search window | ±12 MHz |
| Pair-mode LOs | 5677 / 5751 / 5825 / 5899 MHz (the planner rounds the half-MHz midpoints up; channels at −19 / +18 MHz) |
| FFT | 1024 points → 78.1 kHz bins; Welch 4 × 1024 from a 4096-sample snapshot |

R8 (5917 MHz) is above esp-sdr's validated 5150–5895 MHz range and must be checked
on hardware.

### 2.2 S3 — "2.4 GHz Raceband" (scaled mock-up)

**Mapping.** Raceband occupies 8 channel slots of 37 MHz, i.e. 5639.5–5935.5 MHz
(296 MHz) once each outer channel gets half a spacing of margin. That block is
scaled linearly onto 2400.0–2483.5 MHz (83.5 MHz):

- Scale factor **k = 83.5 / 296 = 0.28209**
- Spacing 37 × k = **10.4375 MHz**
- Center of channel *i* (*i* = 0…7) = 2400 + 5.21875 × (2*i* + 1) MHz

| Ch | 5.8 GHz | 2.4 GHz mock (MHz) |
|---|---|---|
| S1 (R1) | 5658 | 2405.219 |
| S2 (R2) | 5695 | 2415.656 |
| S3 (R3) | 5732 | 2426.094 |
| S4 (R4) | 5769 | 2436.531 |
| S5 (R5) | 5806 | 2446.969 |
| S6 (R6) | 5843 | 2457.406 |
| S7 (R7) | 5880 | 2467.844 |
| S8 (R8) | 5917 | 2478.281 |

The channels stay equally spaced, so the **IMD3 relationships carry over exactly**
(2·S2 − S3 = S1, just as 2·R2 − R3 = R1). The mock is therefore valid for
developing the IMD statistics (§4.8).

**Every bandwidth-like parameter is scaled by k**, so one capture "sees" the same
share of the band, in channels, as on the C5:

| Parameter | C5 | × k | S3 setting |
|---|---|---|---|
| Analog filter full width | 48 MHz | 13.5 MHz | **13 MHz** (BBTOP code ≈ 60, the S3's narrowest characterised setting) |
| Usable span per capture | ±24 MHz | ±6.8 MHz | ±6.5 MHz (filter) |
| "Virtual Nyquist" span | ±40 MHz | ±11.3 MHz | Capture at 40 MS/s (±20 MHz); bins outside ±11.3 MHz are discarded |
| RSSI window | ±5 MHz | ±1.41 MHz | ±1.41 MHz |
| Search window | ±12 MHz | ±3.39 MHz | ±3.39 MHz |
| Channel offset from pair LO | ±18.5 MHz | ±5.22 MHz | ±5.219 MHz (exact, using kHz tuning) |
| Pair-mode LOs | 5676.5 … | — | **2410.4375 / 2431.3125 / 2452.1875 / 2473.0625 MHz** |
| Single-mode LO offset | −8 MHz | −2.26 MHz | −2.26 MHz |
| FFT bin width | 78.1 kHz | 22.0 kHz | 2048-pt at 40 MS/s = 19.5 kHz; Welch 4 × 2048 from an 8192-sample (205 µs) snapshot |

**Where the mock differs from the C5 (on purpose, and documented in the UI):**
- **Aliasing.** On the C5 a neighbour outside ±40 MHz folds back into the capture.
  On the S3 at 40 MS/s the scaled neighbour (≈ ±15.7 MHz) is *inside* the real
  Nyquist band and simply discarded, so it leaks only through the filter skirt. An
  optional **alias-emulation profile** decimates by 2 (halfband, 40 → 20 MS/s) in
  software so out-of-span energy folds in, roughly like it would on the C5. This
  exercises the alias-suspect flags. The fold position is not exact.
- **Filter shape.** The S3's 13 MHz setting is its tightest code, and its skirt
  differs from the C5's 48 MHz mode-1 skirt. The response-equalisation step
  (§4.5) flattens the passband on both chips.
- **The environment is not empty.** The 2.4 GHz band has Wi-Fi, BLE and ELRS
  traffic. Good for testing noise-floor and occupancy logic, but a real "empty
  channel" baseline is harder to get. The UI shows bursty-interference markers
  (high variance between hops).
- **Test signals.** Real 2.4 GHz analog VTXs are about 18 MHz wide, i.e. not
  scaled. Test sources, best first:
  1. A spare ESP32 running a `testgen` env that transmits a CW tone, or slow-swept
     tones, at the scaled channel centers (esp-sdr's PHY cert-test TX path; exact
     API to verify).
  2. A signal generator or HackRF, if available.
  3. Ambient Wi-Fi/BLE.

  `testgen` can also turn several "VTXs" on and off on a schedule, which drives the
  IMD statistics work.

## 3. Repository layout

```
widscan58/
├── PLAN.md
├── LICENSE                      # GPL-3.0 text
├── README.md
├── firmware/                    # one PlatformIO project, several envs (also builds with idf.py)
│   ├── platformio.ini           # envs: esp32s3, esp32c5, testgen_s3, native (host tests)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults       # shared
│   ├── sdkconfig.defaults.esp32s3
│   ├── sdkconfig.defaults.esp32c5
│   ├── partitions.csv
│   ├── src/
│   │   ├── CMakeLists.txt       # per-target sources, --wrap flags, -T sram_guard.ld
│   │   ├── idf_component.yml    # espressif/esp-dsp (pinned)
│   │   ├── main.c
│   │   ├── radio/               # the only target-specific code; derived from esp-sdr
│   │   │   ├── radio.h          #   common backend interface (below)
│   │   │   ├── c5/              #   chip.h, rx.c, recal.c, bandwidth.h, sram_guard.ld
│   │   │   └── s3/              #   rx.c (register capture), recal.c, filter.c, sram_guard.ld, fft asm
│   │   ├── dsp/                 # fft wrapper (portable Q15 / S3 SIMD), power_code.h, halfband.c
│   │   ├── scan/                # profiles.c, planner.c, scanner.c, psd.c, metrics.c, calib.c, imd.c
│   │   └── proto/               # proto.c, link.c
│   ├── testgen/                 # separate app: test-tone transmitter for a spare ESP32
│   └── test/                    # host unit tests: planner, metrics, profiles, encoder, IMD predictor
├── web/                         # Vite + TypeScript, static build
│   └── src/{serial,proto,model,views,mock}/
└── tools/
    ├── widscan58cli.py          # pyserial client: dump frames, record, benchmarks
    └── analyze_capture.py       # offline PSD / filter / alias / hop-time analysis
```

**Radio backend interface (`radio.h`)**. The scanner only ever calls these:

```c
typedef struct { uint32_t rate_hz; uint32_t max_samples; int32_t min_khz, max_khz; ... } radio_caps_t;
esp_err_t radio_init(void);
esp_err_t radio_tune(int64_t lo_hz);                    // C5: whole MHz (+ optional offset); S3: kHz
esp_err_t radio_set_filter(unsigned code, unsigned mode);
esp_err_t radio_set_gain(radio_gain_t g);               // manual idx / hardware AGC
esp_err_t radio_capture(unsigned n, unsigned rate_code, const uint32_t **iq, uint32_t *us);
esp_err_t radio_cal_snapshot(int slot); esp_err_t radio_cal_restore(int slot);   // §4.4
const char *radio_phy_version(void);
```

## 4. Firmware design

### 4.1 Build system (PlatformIO)

- Use the **pioarduino** `platform-espressif32` fork with `framework = espidf`.
  Boards: `esp32-s3-devkitc-1` and `esp32-c5-devkitc-1`.
- **Pin ESP-IDF to `25fe69f9`** with
  `platform_packages = framework-espidf @ https://github.com/espressif/esp-idf.git#25fe69f9…`
  (submodules included, because `libphy` lives in `components/esp_phy/lib`). Both
  envs use the same SDK, as esp-sdr does.
- At boot, compare the PHY lib version string with the value recorded at build
  time. On a mismatch, refuse to start RF (`ERR phy_abi`).
- The app version (`INFO`, the image header, the web flasher's manifest) is
  `git describe` at CMake configure time. PlatformIO skips the reconfigure
  after a commit, so `version_check.py` (`extra_scripts = pre:`) forces it when
  the description changes; until 2026-10-11 local builds could report a stale
  hash (a C5 build reported `6e0c22d-dirty` three commits later).
- The layout also builds with plain `idf.py` (`-DIDF_TARGET=…`), as a fallback if
  PlatformIO cannot use the pinned commit.
- sdkconfig from esp-sdr:
  - Both targets: PHY cert test + debug on, USB Serial/JTAG console, 240 MHz CPU,
    1 kHz tick, `-O2`, PM off, Wi-Fi IRAM options off, 2 MB flash.
  - S3 adds: `CONFIG_FREERTOS_UNICORE=y`, `ESP_WIFI_STATIC_RX_BUFFER_NUM=4`.
    Watchdog settings only need relaxing if we later use the continuous ring.
- **S3 board note:** connect through the chip's **native USB** port (USB
  Serial/JTAG, `303A:1001`), not the CH340/CP210x UART port.

### 4.2 Boot

esp-sdr sequence: NVS → `esp_wifi_init` → `WIFI_MODE_NULL` → start → promiscuous
on → `radio_init()` (prepare RX). Logging is muted and debug text goes out as
framed `LOG` messages. Since 2026-10-10 the device then waits silently in
RotorHazard mode (no `READY` line); hosts switch it with `PROTOCOL MONITOR`
(see "RotorHazard mode" in the Status log).

### 4.3 LO planner (profile-driven)

**Group mode (default):** one LO covers every channel within the
profile's `hop_max_span_khz` (40 MHz; 11.29 MHz on the S3). Raceband pairs as
before: the LO sits halfway, the DC spike lands in the gap, and 8 channels
take 4 hops. Bands A, B and F (19–20 MHz spacing) put three channels on one
LO and take 3 hops; E takes 4. Custom plans can group more (e.g. A8, R3, B1
within 8 MHz). Three channels evenly over 40 MHz leave no gap within reach,
so the LO sits 1 MHz off the middle channel; the outer channels sit 20 MHz
out, so their ±5 MHz RSSI windows reach 25 MHz, past the ±24 MHz passband
(and into the skirt measured above LO 5831 MHz, Status below).
**Single mode:** LO = channel center + profile `single_lo_offset_khz`.

The planner sorts the enabled channels by frequency and groups them greedily
from the lowest (minimum hop count for fixed-width windows). For each group it
tries an LO in the middle of every gap, at the single-mode offset outside
either end, and at both ends of the allowed range; keeps those with every
channel within max(span/2, single offset) of the LO; and picks the one
farthest from the nearest channel center, counting at most the single offset
(ties: the smallest farthest offset). It outputs LOs with `(channel_id, offset_hz)`
lists. The channel plan itself is the profile's default (R1–R8) or a
`CHANNELS` list from the host. A panorama mode sweeps the whole profile band in
steps of the usable span as a background pass:
- C5: 5600–5975 MHz in about 36 MHz steps
- S3: 2400–2483.5 MHz in about 10 MHz steps

The C5 rounds LOs to whole MHz and keeps the actual per-channel offsets. The S3
tunes exactly in kHz.

### 4.4 Retune cost and calibration cache

Measured first on the S3 (Phase 0), then again on the C5:
1. Full `rx_recalibrate` + tune.
2. Tune only, after one calibration at a nearby LO.
3. Restoring a per-LO calibration snapshot:
   - C5: the `phy_param` calibration areas plus the gain table.
   - S3: the equivalent areas. On the S3 the recal path sets
     `phy_param + 288 &= ~0x600` and calls `set_rx_gain_table`. The S3 snapshot
     layout needs its own investigation.

Plan:
- Calibrate each planned LO once at boot and on `RECAL`.
- Hop with option 3, or option 2 if 3 turns out to be unsafe.
- Recalibrate in the background periodically, or when the temperature drifts.

Done on the C5 (`RECALMODE CACHE`, the default). Temperature drift is covered by
the PHY's own tracking: when it recalibrates (≥ 30 units of temperature change),
the next tune clears the cache and recalibrates; tunes run under the PHY lock
(Status, "PHY tracking on the C5"). Image rejection left by the PHY's IQ
calibration is fixed up digitally (§4.5).

Target: ≤ 2.5 ms per hop, so ≥ 100 Hz per channel with all 8 enabled.

### 4.5 Per-hop DSP (`psd.c`, `metrics.c`). All sizes come from the profile.

1. Snapshot capture: C5 4096 @ 80 MS/s, S3 8192 @ 40 MS/s.
2. Saturation count (|I| or |Q| ≥ 510).
3. Welch PSD. Hann window, Q15 FFT (S3: SIMD kernel from esp-sdr; C5: portable),
   linear float accumulation. Optional halfband ÷2 first (S3 alias-emulation
   profile).
4. Per-FFT DC removal. Bins [−1, 0, +1] are excluded from metrics.
   Digital IQ imbalance correction (`IQCORR`, default on): I/Q gain ratio and
   correlation averaged per LO, Q rebuilt orthogonal to I before the window.
   Takes image rejection from the PHY's 20–40 dB to 50+ dB, which pair mode
   needs (a channel at −19 MHz images 1 MHz from its partner at +18). Status,
   "Image rejection vs IQ calibration on the C5".
5. **Response EQ:** subtract the per-LO median noise baseline stored by `CALFLAT`.
6. Per channel:
   - **RSSI**: integrated power over ±rssi_window, in dBFS, plus approximate dBm
     via the gain table and a user offset.
   - **Peak level**: highest bin in ±search_window, this hop.
   - **Most occupied frequency, center, occupied BW**: time-averaged (`metrics_avg_t`), because one 51 µs snapshot is shorter than a video line and catches the FM carrier at the picture level or the sync tip. Each SIGNAL hop's power more than 6 dB above the floor is normalised and binned into 64 bins across the search window, then averaged with weight 2^-shift (`AVG <shift>`, 4–8; default 7, ~2.3 s at 56 hops/s). Most occupied = the largest averaged bin (interpolated); center = the same average of each hop's exact power-weighted centroid; occupied BW = the band holding 99 % of the averaged excess (low and high edges are reported). Without SIGNAL hops the average decays, and below 1/8 of its full level (after ~2.1 × 2^shift hops, ~4.8 s at the default) the channel reports no center.
   - **Noise floor**: 20th percentile of the valid bins in the capture.
   - **Clear-channel floor**: EMA of RSSI, updated only while the channel is empty.
   - **SNR**.
   - **Flags**: empty / signal / saturated / alias-suspect / bursty-interference.
7. Fast and smoothed RSSI per channel. Lap-style peak detection runs in the web UI
   for v1.
8. Display spectrum: max-pool the usable span down to 128 bins, as 0.5 dB codes.

CPU: C5 4 × 1024-pt scalar FFTs ≈ 1 ms. S3 4 × 2048-pt SIMD FFTs: likely well
under 1 ms. Both are benchmarked in Phase 0. The S3 could later move the DSP to the
second core, as esp-sdr does.

### 4.6 Gain

Manual gain while scanning (AGC would hide RSSI changes). `CALGAIN` builds a
gain-index → dB table per band and stores it in NVS. Optional auto-range per LO with
hysteresis, compensated through the table. Every frame carries the gain index used.

### 4.7 Tasks

- `scan_task`: plan → tune → capture → DSP → lock-free ring buffer. It never blocks
  on USB.
- `link_task`: non-blocking USB Serial/JTAG output. If the host falls behind, it
  drops spectrum frames first, then metrics, and counts the drops.
- `cmd_task`: parses commands. Config changes apply at the next hop boundary.

The capture SRAM bank is off-limits to all other code (linker guard plus reserved
region).

### 4.8 IMD / interference statistics

Goal: measure how each channel gets "stepped on" when someone powers up a VTX.

1. **Event detection:** a channel changes empty → occupied (SNR above threshold for
   N hops), or occupied → empty.
2. **Before/after snapshot:** for every *other* channel, the mean and variance of
   RSSI, noise floor and clear-channel floor over a window before and after the
   event (e.g. 1 s each, with a settle gap). The delta is the "step-on" amount.
3. **IMD product prediction:** for the set of active carriers {f_i}, list the
   third-order products 2f_i − f_j (and optionally f_i + f_j − f_k) and the fifth
   order. Raceband (and the scaled S3 mock) is equally spaced, so these land
   **exactly on other channels**. Each channel's delta is then labelled as:
   - predicted IMD3 / IMD5 hit,
   - adjacent-channel leakage (|Δch| = 1),
   - broadband desense (all channels rise),
   - unexplained.
4. **Over-the-air vs. our own receiver:** after an event, optionally step the
   manual gain down by X dB for a few hops. A product generated in *our* front
   end/ADC drops about 3X dB, while a product generated over the air or in the VTXs
   drops only X dB. Each IMD observation is classified accordingly. Saturation
   counts are recorded alongside.
5. **Accumulated statistics:**
   - matrix M[source channel → victim channel] of the step-on dB distribution
     (count, mean, p50, p90, max);
   - per-pair-of-sources IMD hit rates;
   - time-stamped event log.

   Kept in RAM on the device (summary), and computed in full from the recorded
   METRICS stream in the UI.
6. **Test path:** `testgen` turns S-channel tones on and off on a schedule, giving
   reproducible events and known IMD3 products on the S3 mock.

v1: event detection and per-channel deltas in the UI (from METRICS). The firmware
`EVENT` frames and the gain-step discrimination come in Phase 4.

### 4.9 Wire protocol (v1)

Host → device: ASCII lines; each replies `OK …` or `ERR <reason>`. This is
monitor mode; `PROTOCOL RH|MONITOR` switches between it and RotorHazard's binary
node protocol (`firmware/src/proto/rh.c`), which the device boots in.

| Command | Purpose |
|---|---|
| `INFO` | `widscan58\|<target>\|<proto_ver>\|<fw_ver>\|<commit_time>\|<phy_ver>` (separated by `\|`: the PHY version has spaces) |
| `PROFILE?` | Dump the band profile as JSON (channel plan, windows, rate, span); one profile per target, no switching |
| `CHANNELS <name>=<MHz> …` / `CHANNELS DEFAULT` | Replace the channel plan (up to 8, 5.8 GHz equivalent MHz); `PROFILE?` reports it |
| `ENABLE <mask>` / `ENABLE?` | Enabled-channel bitmask |
| `MODE GROUP\|SINGLE` | Hop strategy |
| `SCAN START\|STOP` | Start/stop round robin and streaming |
| `PANO <every_n_rotations>` / `PANO OFF` | Background panorama sweep |
| `GAIN MANUAL <idx>\|AUTO\|HARDWARE`, `GAIN?` | Gain |
| `FILTER <code> [mode]` | Override the analog filter |
| `CAPN` / `FFT` / `AVG` / `WIN <rssi_hz> <search_hz>` | DSP overrides |
| `SPECRATE <k>` / `SPECBINS <n>` | Display spectrum decimation |
| `RECAL` / `CALGAIN` / `CALFLAT` | Calibration routines |
| `GAINSTEP <dB> <hops>` | IMD discrimination probe (Phase 4) |
| `RAWIQ <lo_khz> <samples>` | One-shot raw I/Q dump (IQ8, CRC32) |
| `SYNC <nonce>` | Resynchronise |

Device → host: binary frames
`0xA5 'R' type ver | u16 len | payload | u32 crc32` (little-endian). Text replies
start with an ASCII letter. The parser resynchronises on the magic and CRC.

As built, the layouts are in `firmware/src/proto/frames.h`, mirrored by
`web/src/proto/frames.ts` and `tools/widscan58cli.py`. That header is authoritative
over the plan table below, and it static-asserts the sizes the parsers hard-code
(metrics header 34 bytes, channel record 26, levels header 32, level record 4,
telemetry 62). All µs timings are u32.
The stream makes no compatibility promise across firmware versions: change all
three together, and old recordings stop replaying.

| Frame | Contents |
|---|---|
| `METRICS` (per hop) | seq, t_us, LO kHz, gain idx, sat count, noise floor (cdB), hop timing (tune / capture / dsp µs), then per channel: `{id, rssi, rssi_ema, snr, clear_floor (cdB), peak_off, centroid_off (Hz/10), obw (kHz), flags}` |
| `LEVELS` (per hop, `METRICS RSSI\|SNR`) | seq, LO kHz, hop timing, gain, sat count, mode, floor per bin (u8 spectrum code, dBFS = code / 2 − 127.5; SNR mode only), then per channel: `{id, flags, rssi (u8 level code), snr (dB × 4; SNR mode only)}`. Level codes are 0.25 dB: dBFS = code / 4 − 63.75 |
| `SPECTRUM` / `PANO` | seq, LO kHz, start kHz, bin Hz, n, detector, u8 codes[] |
| `TELEMETRY` (≈ 4 Hz) | rotation Hz, hop timing min/avg/max, CPU ‰, heap, TX drops, temperature, recal age per LO |
| `EVENT` (Phase 4) | channel on/off transition plus the before/after delta vector |
| `RAWIQ` | LO, rate, n, offset, conjugated, shift, IQ8 payload: one 1024-sample chunk per frame, in order |
| `LOG` | level + text |

About 100 KB/s at 100 rotations/s, well within USB Serial/JTAG.

**Stretch goal:** an esp-sdr command compatibility shim (`INFO/CAPS/FREQ/CAP16/SPEC`)
so the official ESP-SDR browser viewer can use our firmware.

## 5. Web UI design

**Stack:** Vite + TypeScript, vanilla DOM, Canvas2D (WebGL for the waterfall if
needed). Builds to a static bundle. Web Serial requires Chromium and HTTPS or
localhost.

**Fully profile-driven:** on connect, the UI calls `INFO` and `PROFILE?`. Channel
names, frequencies, windows, panorama span and axis labels all come from the
profile, so the same UI shows R1–R8 at 2.4 GHz on the S3 and at 5.8 GHz on
the C5. The channel plan (Channels → Edit), hop mode and gain are kept in
localStorage and sent on every connect. A banner marks the S3 as a scaled
mock-up; its frequencies are always shown as tuned (2.4 GHz).

**Transport:** `requestPort` filtered to `303A:1001`. Stream parser splits text
lines from frames, checks CRC, counts resyncs. One outstanding command at a time
while streaming. Automatic reconnect after a board reset.

**Mock device** (`web/src/mock/`): generates frames for either profile (moving
carriers, drifting VTX, Wi-Fi bursts, saturation, scheduled VTX power-ups with IMD3
products), so UI and IMD analytics can be developed without hardware. Recorded
sessions replay through the same parser.

**Views:**
1. **Connection bar:** versions, target, profile, link statistics, start/stop.
2. **Channel grid:** per-channel enable toggle, RSSI bar, SNR, noise floor, center
   offset, OBW, flags, 10 s sparkline, update rate (Hz). Also shows the current LO
   plan.
3. **Band panorama:** stitched spectrum with channel overlays, live trace,
   max-hold, average, persistence, and segment-age shading.
4. **Waterfall.**
5. **Channel zoom:** markers for nominal center, peak, centroid, OBW edges, noise
   floor, DC and alias shading.
6. **RSSI history:** lap-timer style, with threshold enter/exit detection.
7. **IMD / step-on view:**
   - source → victim heat-map matrix (median and p90 step-on dB);
   - event log;
   - predicted IMD3/IMD5 overlays on the panorama for the currently active
     carriers;
   - per-event detail (before/after per channel, classification);
   - CSV export.
8. **Debug panel:** hop timing histogram, rotation rate, CPU, heap, drops,
   saturation and gain per LO, recal age, METRICS inspector, protocol log, command
   console.
9. **Raw I/Q tool:** time trace, constellation, high-resolution FFT.
10. **Session tools:** record/replay (IndexedDB + file), CSV export, screenshot.

## 6. Phases

### Phase 0-S3 — S3 bring-up (now)
1. PlatformIO `esp32s3` env builds against the pinned IDF. `INFO` reports the PHY
   version.
2. Port the S3 snapshot capture, recal, kHz tuning, filter and gain from esp-sdr
   into `radio/s3/`, behind `radio.h`. Add `RAWIQ`. `tools/widscan58cli.py` plots
   captures. **Done when:** a known 2.4 GHz source (e.g. Wi-Fi channel 6 at
   2437 MHz, or a `testgen` tone) appears at the right frequency, including at
   kHz-offset LOs.
3. Measurements (recorded in `docs/bringup-s3.md`):
   - retune options (§4.4) and settle time;
   - FFT/DSP time;
   - filter shape at code 60 vs. other codes, and whether 13 MHz ≈ k × 48 MHz in
     practice;
   - gain index → dB;
   - leakage from a strong neighbour tone at ±15.7 MHz.
4. `testgen` env: tone transmitter at the S-channel centers (verify the cert-test
   TX API and its frequency granularity).

### Phase 1 — Scanner MVP (S3)
Profiles, planner, scanner, Welch PSD, metrics, METRICS/TELEMETRY frames,
ENABLE/SCAN/PROFILE, manual gain. Host tests (`pio test -e native`). **Done when:**
8 S-channels at ≥ 50 Hz each; a `testgen` tone is found on the right channel;
centroid tracks a tone deliberately offset by ±200 kHz within ±50 kHz (scaled
precision); noise floor is stable within ±1 dB in a quiet part of the band.

### Phase 2 — Web UI MVP
Transport, parser, mock device (both profiles), connection bar, channel grid with
toggles, RSSI history, debug panel. **Done when:** it works against both the mock
and the real S3, and toggling channels changes the hop plan live.

### Phase 3 — Spectrum views
SPECTRUM/PANO, panorama, waterfall, zoom, raw I/Q tool, record/replay, the scaled
5.8 GHz axis toggle.

### Phase 4 — Calibration, robustness, IMD
Calibration cache, `CALFLAT`, `CALGAIN`, auto-range, alias-emulation profile,
saturation/alias/bursty flags, reconnect handling, ≥ 1 h soak. IMD: firmware
`EVENT` frames, `GAINSTEP` discrimination, step-on matrix and IMD view, validated
with scheduled `testgen` multi-tone runs.

### Phase 0-C5 → C5 port (when the devkits arrive)
1. ~~`esp32c5` env + `radio/c5/` (`adctrig` capture, `phy_set_chanfreq`, mode-1
   filter).~~ Done 2026-10-08, running on a XIAO ESP32-C5; see Status.
2. Rerun the Phase 0 measurements at 5.8 GHz, plus:
   - real alias rejection (R3 seen from LO 5676 at −24 MHz);
   - whether R8 at 5917 MHz is usable.
3. Finalise the `c5-race` profile (pair vs. single mode, filter code).
4. Everything above the radio backend should run unchanged. **Done when:** the
   Phase 1 acceptance criteria pass with real VTXs on Raceband.

### Phase 5 — Stretch goals
Other bands (A/B/E/F, low band), analog/digital classification, firmware lap
detection, esp-sdr viewer shim, multiple scanner nodes.

## Status (2026-10-06)

**Phase 0-S3: done.** **Phase 1: done**, apart from firmware host unit tests. **Phase 2: done.** **Phase 3: mostly done** (no IMD view yet).

Measured on the ESP32-S3 Micro (4 MB flash, 2 MB PSRAM, native USB only):

| Item | Result |
|---|---|
| Toolchain | pioarduino 55.03.312-1 / ESP-IDF 5.5.5. Every S3 PHY function used is instruction-identical to esp-sdr's pinned IDF (`tools/check_phy_abi.sh`). 55.03.39 fails with PlatformIO Core 6.2 (SCons 4.8.1 vs 4.11.1 conflict). |
| Spectrum orientation | Conjugated (RF above the LO shows at negative baseband) confirmed with LO-offset captures. Profile default `iq_conjugate = true`. |
| Filter code 60 | Flat to about ±6.5 MHz, roughly -10 dB at ±8 MHz, so about 13 MHz as designed. |
| Board spurs | 2400.0, 2440.0, 2480.0 MHz (40 MHz crystal harmonics) and 2448.0 MHz (likely 48 MHz × 51, USB). Fixed in RF, found by LO sweep. Masked per target (`SPURS`), flagged `SPUR`. No LO-relative spurs. |
| Tuning accuracy | 2448 MHz spur measured within 1 kHz of its expected offset from every LO. |
| Tune cost | Full sequence 1.2 ms. PLL offset only (`set_rf_freq_offset`) 0.9 ms, which is now the default, with a full tune at least every 200 ms. Channel setup resets RX gain to AGC, so a full tune must re-apply it. |
| DSP | esp-dsp float FFT 0.41 ms per 1024 points. Bit-reversal pass removed (shifted index table). Per hop: prep 0.6 + FFT 1.65 + power 0.26 + metrics 0.46 + spectrum 0.52 ms. |
| Hop / rotation | About 4.6 ms per hop. **About 51–55 rotations/s = 51–55 RSSI updates/s per channel with all 8 enabled** (4 pair LOs). |
| Profile defaults | 4096 samples @ 40 MS/s, 4 × 1024-pt Welch (39 kHz bins), passband ±6.77 MHz (C5 ±24 × k). 2048-pt FFTs halve the hop rate. |
| ADC level | At gain 60, receiver noise is about 3–4 counts RMS of 512 (ADC quantisation is well below that). Wi-Fi bursts reach about -10 dBFS without clipping. |
| Link | 0 CRC errors. About 77 KiB/s with 256-bin spectra every hop. USB writes are chunked to 4 KiB (the driver drops larger writes). |
| Web UI | 16/16 headless checks pass against both the simulator and the real board (via `tools/serial_bridge.py`): commands, plan changes, raw I/Q, light/dark, 390 px width. |

**FFT comparison (2026-10-06).** `FFTTYPE R2|R4|S16|S16R`, `FFTEVAL SYNTH|LIVE` runs every FFT on identical samples.
- **R4 (radix-4 float)** was briefly the default. It matches R2 to within 0.01 dB on every bin and the FFT is 30 % faster (1.15 vs 1.64 ms per hop).
- **S16R** is esp-sdr's rounding-corrected S3 16-bit FFT with block-floating-point input scaling. The FFT takes 0.27 ms per hop.
  - Noise-only and live captures: per-bin error vs float 0.10–0.28 dB RMS, floor bias −0.1 to −0.2 dB, spur level and tone-to-spur range equal to float.
  - With a strong signal in the same capture (−10 / −3 dBFS), weak bins read 1.0–1.4 dB low (1.3–2.6 dB RMS). This is inherent to a 16-bit FFT and matters for step-on/IMD measurement next to a strong VTX, S16R is now the default anyway (see below); use `FFTTYPE R4` for step-on/IMD work.
- Plain esp-dsp S16 (truncation bias) is worse in every case. Keep it only for comparison.
- FFT tables load on demand: free heap is 62.8 KB with R4, 97.5 KB with S16R (it was 35 KB with everything loaded).
- **Open: bursty long tunes.** 13–20 % of hops take ≥ 3 ms to tune, in bursts (6–60 ms apart, then quiet for several seconds), on every LO. Promiscuous mode is not the cause. Probably pre-emption on core 0; this costs about 15 % of the rotation rate.

Open items:
- Firmware host unit tests (`pio test -e native`) for planner, metrics and frame encoding: not written yet.
- Per-LO calibration cache (§4.4): not needed at current hop rates; revisit on C5.
- Faster FFT (sc16 SIMD as in esp-sdr) if more hop rate is wanted.
- Gain → dB table (`CALGAIN`), flatness EQ (`CALFLAT`), auto-ranging: Phase 4.
- IMD/step-on statistics (§4.8): not started.
- Alias-emulation profile: not started.
- ~~Re-verify Web Serial in a real browser~~ Done (2026-10-06, Chrome via Claude in Chrome): auto-reconnect from the stored grant, 12/12 in-page checks, 20 s sustained at about 77 KiB/s with 0 CRC errors and no break events. A firmware `REBOOT` keeps USB connected; the UI now re-syncs settings when it sees `READY`.

**Update (2026-10-06, evening).**
- **Default FFT is now S16R** (rounded 16-bit FFT with block-floating-point scaling). Caveat: with a strong signal in the same capture, weak bins read about 1–1.4 dB low. Switch to `FFTTYPE R4` for step-on/IMD measurement.
- **R4 tables are allocated at boot and kept** (16 KB, sized for 1024 points), because after boot the heap is too fragmented to find a 16 KB block. Other FFT tables load on demand. A failing PSD now reports `ERR psd <err> fft <type>` once per configuration, instead of the scanner going silent.
- **Tune skip:** when the next hop uses the same LO (one channel enabled, or one pair-mode LO), `radio_tune` skips the PLL reprogram and settle delay. The 200 ms safety full tune and recalibration still run.
- **Display spectrum default:** every 4th hop, rotating through the LOs.

Measured timing (S16R default, spectrum every 4th hop, 8 s per row):

| FFT | Enabled | Hops/s | Updates per channel | Tune median / mean | Capture | DSP | Free heap |
|---|---|---:|---:|---:|---:|---:|---:|
| S16R | all 8 (4 LOs) | 255–261 | **64–65 /s** | 835 / ~1400 µs | 168 µs | 2.21 ms | 80 KB |
| S16R | S1 only (1 LO) | 445 | **445 /s** | 3 / 25 µs | 176 µs | 2.03 ms | 80 KB |
| S16R | S1+S2 pair (1 LO) | 430 | **430 /s each** | 3 / 25 µs | 177 µs | 2.14 ms | 80 KB |
| R4 | all 8 | 227 | 57 /s | 822 / 1286 µs | 168 µs | 2.84 ms | 80 KB |
| R4 | S1 only | 344 | 344 /s | 2 / 28 µs | 175 µs | 2.66 ms | 80 KB |
| R2 | all 8 | 202 | 51 /s | 806 / 1331 µs | 169 µs | 3.32 ms | 68 KB |
| R2 | S1 only | 293 | 293 /s | 2 / 29 µs | 174 µs | 3.13 ms | 68 KB |

- With one LO, the hop is capture plus DSP only, so the FFT type sets the rate directly.
- With 4 LOs, the bursty long tunes (≥ 3 ms on 13–20 % of hops) cost about 15 % of the rate. Still open.

**Open: flashing reliability (S3).** Uploads sometimes fail partway through (`No more data to read`, `MD5 mismatch`, `written flash region is empty`).
- A failure after the erase step leaves the board unbootable: the ROM prints `invalid header: 0xffffffff`.
- Reliable recovery: hold BOOT, tap EN, release BOOT, then `esptool --before no-reset --after watchdog-reset write-flash …` (`tools/flash.sh --manual`).
- An RTS hard reset does not exit download mode; use `--after watchdog-reset`.
- The `BOOTLOADER` command left the board unresponsive (not in usable download mode) on its only test.
- `tools/flash.sh` (SCAN STOP, then usb-reset) also failed mid-image on its only test.
- Root cause unknown. Next step: check the kernel log for re-enumeration and the ROM reset reason during a failure, and whether the chip resets while idle in download mode (watchdog) versus only during writes (power).
- Last state: the board may be unbootable after that failed `flash.sh` run. Reflash it with BOOT + EN first.

**C5 target (2026-10-08). Runs on a Seeed XIAO ESP32-C5 (chip rev v1.0, 8 MB flash), scans R1–R8, and passes the headless web suite.**

What exists:
- `pio run -e esp32c5` builds without warnings (board `esp32-c5-devkitc-1`, 4 MB, same pioarduino / IDF 5.5.5 as the S3). All 14 C5 PHY functions the backend calls are instruction-identical between esp-sdr's IDF pin and IDF 5.5.5 (`tools/check_phy_abi.sh esp32c5 25fe69f9... v5.5.5`).
- `src/radio/c5/radio_c5.c`, ported from esp-sdr's `families/c5_c6_c61/receiver.c`:
  - Capture: `adctrig(n-1, 0, 0, 2*rate_code, ...)` into 0x40830000, with fill-word and overrun-guard checks. Rate codes 0–5 = 80/40/20/10/8/4 MS/s.
  - The SRAM ownership bit hands the modem the whole 128 KiB bank 0x40820000–0x40840000, so the bank is reserved from the heap and `sram_guard.ld` fails the link if static sections reach it. Static RAM currently ends at 0x4081b9c8 (about 18 KB of headroom).
  - Tuning: `phy_set_chanfreq(mhz, 1)`, whole MHz only (`radio_caps()->lo_step_khz = 1000`). PHY channel mode 1 is the wide analog path. Mode 0 (11–23 MHz) is not exposed.
  - Calibration: `recal_c5.c`, esp-sdr's C5 `rx_recalibrate` (fresh DC/IQ at the LO, then the gain table), with `--wrap=phy_chip_set_chan_ana`.
  - Filter: BBTOP 0x67 host 1 regs 6/7, applied only around a capture. In mode 1, code 0 is about 48 MHz and code 60 about 22 MHz (esp-sdr curve).
  - Gain: `force_rx_gain`; the maximum index is read from 0x600a702c (< 90).
  - `BOOTLOADER` sets `LP_AON_FORCE_DOWNLOAD_BOOT`.
- Shared code:
  - Profiles carry a `target`. The firmware uses the profile for its chip (the `PROFILE <name>` switch was later removed: each chip has only one). The `c5-race` values match the web simulator.
  - `radio_caps_t` has 6 rate slots and `lo_step_khz`. `RAWIQ` and `FFTEVAL LIVE` reject LOs that are not a multiple of it.
  - The link and command tasks were pinned to core 1; they now use the last core (core 0 on the single-core C5).
- FFT: the S3's S16R kernel is Xtensa assembly. Other targets use `dsp/fft_r4_sc16.h`, a rounded Q15 radix-4 kernel (see the DSP section below). The C5 has no FPU, so R2/R4 run in software floating point.
- `tools/flash.sh` takes `RX_ENV=esp32c5` and writes the C5 bootloader at 0x2000.

Bring-up on hardware (XIAO ESP32-C5, 2026-10-08):
- **Boot loop, fixed.** `esp_wifi_init` failed with `ESP_ERR_NO_MEM` and `app_main` aborted before `READY`, so the web UI never connected. The C5 heap is only ~125 KB once the capture bank is reserved, and the S3-sized link buffers (72 KiB queue + 16 KiB USB TX) took most of it. The C5 now uses a 36 KiB queue and 4 KiB of USB TX, and `PSD_MAX_FFT` is 1024 (`FFT` rejects 2048). Free heap while scanning was about 11 KB at that point; see the DSP section for where it went.
- Boot steps after `link_init` use `BOOT_CHECK`: on failure the board prints `ERR boot <step> <error> heap=… largest=…` every second and keeps draining USB RX, instead of reboot-looping silently.
- **Web Serial "The device has been lost" after a tool used the port:** pyserial (so esptool too) leaves the tty at VMIN=0, termios state outlives the close, and Chrome 153 reads a 0-byte non-blocking read as `DEVICE_LOST` (Chromium main now forces VMIN=1 on open). The failed session also kept the port open, so the retry said "Connection failed". Only a re-enumeration (RESET) cleared it. Our tools now leave VMIN=1 (`tools/ttyfix.py`, `flash.sh`), and the UI closes the port after a fatal read error. This happened on the S3 as well.
- `RAWIQ` was capped by the link queue: `STATUS` reported `rawiq_max` (9180 on the C5, 16380 on the S3), the command refused more (`ERR rawiq_args`), and the UI disabled larger sizes. RAWIQ frames wait up to 500 ms for queue space (`link_frame_wait`); before that, an 8192-sample capture sometimes failed with `ERR link_full`. Since 2026-10-11 RAWIQ goes out as 1024-sample chunks (each waits for queue space; the samples stay in the capture aperture meanwhile), so `rawiq_max` is the capture buffer (16380) on both targets and the C5 queue is 16 KiB instead of 36 KiB. Free heap while scanning went from ~18 KB to ~39 KB.
- **USB reset wedge on rev v1.0 silicon, fixed.** The ROM loops forever after a USB Serial/JTAG (RTS) reset if UART0's clock is gated (esp-idf#18089, IDFGH-17050). IDF 5.5.5 avoids the gating only when USB Serial/JTAG is the *console*. Ours is off (the binary protocol owns the port), so `app_main` turns the UART0 clocks back on. Before the fix, esptool auto-reset failed (`Write timeout`), and closing the port (the UI or the bridge disconnecting) left the board dead until RESET was pressed. Now `pio run -t upload` / `tools/flash.sh` work hands-free, and open/close cycles are fine.
- Capture: all six rate codes complete. A 48 MHz-crystal harmonic at 5856 MHz shows up at LO+19 MHz (80/40 MS/s) and folds to −1/−1/+3/−1 MHz at 20/10/8/4 MS/s, as expected. The lower rates are plain decimation with no anti-alias filter. That spur's sign confirms `CONJ 1` for the C5 too. Other narrow spurs seen while scanning: 5664 and 5760 MHz (also 48 MHz harmonics), plus lines near 5840 and 5920 MHz. The LO sweep below supersedes this list. 5 GHz Wi-Fi is visible around 5860–5915 MHz.
- Hop timing at bring-up, all 8 channels in pair mode: 15.3 ms/hop (15 rotations/s) against the 2.5 ms target.
  - Tune: 1.1–1.4 ms, almost all `phy_set_chanfreq`. `TUNESKIP 2` saves only ~40 µs. `TUNESKIP 4` raises the floor by 21 dB: as on the S3, channel setup resets the gain, so the re-apply stays.
  - Capture: 1.88 ms. About 1.73 ms of that is per-capture latency inside `adctrig` before the done bit (bit 18 of 0x600a9004) sets. The sample count adds only ~10 ns/sample, and fill, filter and restore add < 0.1 ms. A second capture right after the first (no retune) takes the same 1.73 ms, so it is not a post-tune settle. The loop around the done bit is a plain busy-wait; the delay is in the dump engine's setup (0x600a9018 fields?). Driving the capture registers directly, as the S3 backend does, is the likely fix.
  - DSP: 12.2 ms (PSD 8.7: prep 1.1, FFT 3.1, power 3.4; metrics 3.0). Soft float everywhere.

**DSP on the C5 (2026-10-08): 12.2 → 3.6 ms per hop; 15 → 36–39 rotations/s; heap 11 → 30 KB.**
- The PSD is integer: `psd_result_t.p` is `uint32_t` power per bin, with one float `scale` for the result. S16/S16R accumulate `|X|²` without floats. Each segment's block-floating-point shift sets an exponent, and the accumulator is rescaled once if a later segment needs a coarser unit, so nothing overflows (bound: `|X/N|² ≤ 2^29`; headroom bits beyond 4 segments). R2/R4 still accumulate floats, then quantise once with the strongest bin at 4e9: 96 dB of range, and the 4e9 (not 2^32) keeps the peak from rounding past `UINT32_MAX`.
- Metrics run on the integer bins: precomputed bin ranges, `uint64` sums, an integer quickselect for the floor and integer centroid/OBW. Floats appear only once per channel. `metrics_db` is a table-driven log2 (~1e-4 dB) instead of soft-float `log10f`, which cost thousands of cycles and ran ~20 times per hop. The EMA uses the linear RSSI directly, and the centroid threshold ratio is precomputed per configuration. A host test against the float metrics on 20 000 random PSDs: with a signal (SNR ≥ 6 dB), RSSI within 0.0001 dB, floor within 0.002 dB, centroid within 52 Hz, flags identical.
- Spectrum codes come from an integer log2 (98.4 % identical to the float codes, the rest ±1 code = 0.5 dB).
- S16R on the C5 is `dsp/fft_r4_sc16.h`, a rounded radix-4 DIT kernel: half the stages, a multiply-free first stage, Q15 twiddles in flash (`tools/gen_twiddle.py`), digit-reversed input written directly by the windowing pass, and natural-order output. Stage 1 rounds ties to even, and twiddle products keep 6 fractional bits (5 in the radix-2 stage used for 2·4^k sizes), so the rounding bias stays at ~0.005–0.03 LSB per bin at the block-scaled input levels (up to ~0.1 LSB on tiny inputs, where twiddle-free butterflies still tie). It is overflow-free for any int16 input. Host test against a double DFT: 0.33 LSB rms error (esp-sdr's radix-2: 0.42). `FFTEVAL` against R4: floor bias +0.002 dB, 0.080 dB rms per bin (the old radix-2 kernel: −0.012 dB and 0.106 dB). `pio test -e native` (test/test_dsp) checks the kernel against a DFT, `metrics_db`, and the integer metrics on a synthetic tone. The kernel runs from IRAM (1 KB of RAM for 7 % of FFT time); the twiddles stay in flash, since moving them to DRAM saved only 60 µs for 3 KB.
- The S16 pass 1 uses one's-complement magnitudes, so one compare finds both ADC rails. Pass 2 is specialised per output order and conjugation.
- **DSP region:** the dump only writes the upper half of the reserved bank (0x40830000). Its lower half (0x40820000, 64 KiB) is reserved only because the modem owns the whole bank during a capture, and its contents survive captures (checked: 4000+ captures of every size and rate, `RECALMODE EVERY`, hardware AGC). `radio_dsp_region()` exposes it. On the C5 it holds the R4 table (16 KiB, previously heap for life), the float work buffer, the R2 table and `FFTEVAL`'s buffers. This is what made R2 loadable and `FFTEVAL` work on the C5.
- `FFTEVAL` crashed on the C5 (R2 couldn't load, and the eval used the result anyway). Now psd errors are reported per type (`FFTEVAL SYNTH R2 ERR …`), types load one at a time, failed esp-dsp inits are rolled back (they leaked their twiddle table), and the float reference is R4 for power-of-4 sizes.
- Timings above 65.5 ms (R2/R4 on the C5) no longer saturate: all µs fields in the metrics and telemetry frames are u32 (metrics header 34 bytes, telemetry 62). The device→UI stream makes no promise of compatibility with older firmware or recordings.
- Then (S16R, all 8 channels, pair mode): hop ≈ 6.6 ms = tune 1.1–1.3 + capture 1.9 + DSP 3.6 (prep 0.8, FFT 2.0, power 0.3, metrics 0.4–0.5, spectrum 0.3 every 4th hop). R2/R4: ~68 ms/hop (FFT 50 ms in soft float). S16: 8.1 ms/hop.

**Capture latency on the C5 (2026-10-08): 1.79 → 0.13 ms per capture; hop 6.6 → 4.7 ms, 36 → 52 rotations/s.**
- `adctrig` (librftest `mac_common.o`) disassembled: for selector 0 it sets the length, rate and lane selectors (0x600a9004/08/18), takes the SRAM bank (0x60095004), selects raw source 15, pulses the trigger (0x600a9004 bit 19) and busy-waits for done (bit 18). Its closing `phy_printf` is compiled out at log level NONE.
- Timing the unrolled sequence: trigger to done is 52 µs for 4096 samples (the dump itself). The other 1.65 ms was one read-modify-write: `0x600a20b4 &= ~1`. Each access to that register stalls the bus ~840 µs and reads 0, so it sits in a block that isn't clocked here (adctrig only treats it specially for its BLE selector).
- `radio_capture` now drives the dump registers directly and leaves 0x600a20b4 alone. Averaged per-LO spectra match `adctrig` within run-to-run variation (alternated three times), all six rate codes give complete captures, and there were no capture errors.
- Hop now (S16R, 8 channels, pair mode, spectrum every 4th hop): ≈ 4.7 ms = tune 1.0–1.1 + capture 0.13 + DSP 3.5. Tune (`phy_set_chanfreq`) and DSP are what remain against the 2.5 ms target.

**Calibration on the C5 (2026-10-08): `RECALMODE CACHE`, the new default.**
- `ONCE` (calibrate at 5787 MHz, then only tune) vs `EVERY` (calibrate at each LO, ~41 ms), raw I/Q at 12 LOs, gain 60, dummy load:
  - DC offset grows with distance from 5787 MHz under `ONCE`: ~5 counts at 5798, 19–25 at 5751–5835, 39–58 at 5724/5687/5872–5909, 73 at 5650 (noise rms ~5–6 counts, 10-bit ADC). Under `EVERY` it stays ≤ 10 counts everywhere.
  - Noise floor away from DC: the same in both modes.
  - Bursts of raised floor (gain 40, single mode): under `ONCE`, some calibrations leave one or two LOs with the whole capture raised 10–30 dB (filter-shaped, steady over the 920 µs capture, lasting a few tens of ms) in ~50–60 % of their frames; which LOs depends on the calibration (~5 of the first 20 calibrations of a session, then none in the next 30). Separately, 1–5 % of frames at 5724 and 5872 MHz after every calibration. `EVERY`: 4 events in 8596 frames. Settle time (100 µs to 20 ms) made no consistent difference. External bursts would not depend on the calibration mode, so these come from the receiver; the exact mechanism (likely the gain state) is not pinned down.
- What a 5 GHz calibration changes in `phy_param` (diffed across calibrations): the RX IQ correction (+222, 14 bytes) and the two DC tables (+384, 272 bytes; +912, 56 bytes). `phy_set_rx_gain_table` rebuilds the RX gain memory from the DC tables in ~160 µs. A few other bytes (+288, +714, +1024) change too but are not needed.
- `RECALMODE CACHE` (`recal_c5.c`): the first visit to an LO calibrates and saves those three areas (16 LRU slots × 342 bytes, 5.5 KB static, so ~24 KB heap free while scanning); later visits restore them and rebuild the gain table. `RECAL` clears the cache. 2.4 GHz on the C5 has other areas and falls back to calibrating every LO; the S3 keeps `ONCE` as its default.
- Results: DC offset ≤ 9 counts at every LO on restore-only passes, the same as `EVERY`; no stuck LOs over 20 fresh calibrations; the remaining ≤ 1 % events are at 5724/5761, whose windows include the 5736–5752 MHz bump seen with the dummy load (probably real RF; it matches a 20 MHz Wi-Fi channel 149). Cost: tune 1.04 → 1.22 ms, hop 4.73 → 4.92 ms (pair mode).
- Not covered: image rejection (needs a test signal). Temperature drift: see "PHY tracking on the C5" below.
- Rechecked 2026-10-10 (scanner metrics, 8 calibrations per mode, GROUP and SINGLE, run twice): hop time the same (4263 vs 4270 µs); under `ONCE` R1/R2/R7/R8 have RSSI sd 0.9–3.5 dB with 1.5–7 % of frames > 3 dB over the median (99th percentile +7.5–9 dB, below the 6 dB SIG threshold so no flags), vs 0.3–0.45 dB and ≤ 0.2 % under `CACHE`. R3–R6 match. R3's ~1.5 dB sd in both modes is Wi-Fi channel 149.

**PHY tracking on the C5 (2026-10-10, after esp-sdr 5ede49b/77ffc12).**
- `CONFIG_ESP_PHY_PLL_TRACK_PERIOD_MS=1000`: an esp_timer callback runs `phy_param_track_tot` every second under the PHY lock (~50 µs). Its `phy_cal_param_track` compares the temperature (`phy_param` +0) with the one at the last calibration (+1024); from a difference of 30 (or `phy_param[1068]` when bit 1 of [1067] is set) it redoes the RX calibration at its reference channels (~45 ms): 2.4 GHz DC, gain table at 2432 MHz, 5 GHz DC (+384/+912) and IQ at 5520 MHz (+222), the areas `CACHE` saves. Idle at room temperature the readings were 51–55 against 52, so it had not fired.
- Forced every second (threshold 0): our calibration interleaved with it hung the scanner task (commands still answered) after 2.4 s in `EVERY`, and after ~20 s in a run with repeated `RECAL`. With a single LO (one channel enabled), its reference-channel tables stayed in place: DC −44 → −28 dB, RSSI sd 0.3 → 4–7 dB in `CACHE` and `ONCE`. With four LOs `CACHE` restored them on the next hop.
- Now `radio_tune` runs under `phy_get_lock()`, and a `--wrap` of `phy_set_rx_gain_cal_dc` counts DC measurements outside `c5_recalibrate`; after one, the next tune clears the cache and recalibrates. Forced every second: no hang in 3 min (2471 `EVERY` calibrations against 120 tracker runs), single-LO RSSI sd 0.3–0.5 dB. This also refreshes the cache after a 30-unit temperature change. A tracker run during a capture can still spoil that capture (as in esp-sdr, the lock is not held across acquisition).
- The S3 has the same tracker and no lock yet.

**RotorHazard mode (2026-10-10).** The board is a RotorHazard timer node: one
serial multi-node processor with 8 seats (node API 36), so a stock RotorHazard
server drives it with only `SERIAL_PORTS` configured.
- Boot is silent in RotorHazard mode; `PROTOCOL MONITOR` switches to our
  protocol, and in monitor mode a lone 0x22 after ≥100 ms of input silence (and
  nothing for 5 ms after it) switches back and answers it. Our command lines in
  RotorHazard mode start with `A`–`P`, which RotorHazard does not use.
- Seats feed `lap.c` (255 ms running median, EnterAt/ExitAt crossing, pass at the middle
  of the highest plateau, peak/nadir history with 2-code hysteresis) from the
  `METRICS RSSI|SNR` byte codes of each hop. RotorHazard-mode defaults: `FFT
  128`, `CAPN 512`, `METRICS RSSI`, no spectrum.
- Verified with RotorHazard's own interface code (`tools/rh_check.py`, their
  main branch at 1e8bbb4): discovery in 2.0 s (count 8, API 36), seat
  frequencies (1111 = off) and thresholds set and read back, 0 comm errors in
  ~600–2000 reads, passes reach its pass callback with timestamps and peaks
  (with thresholds just above the noise). Taking over a board left streaming in
  monitor mode worked first time, without retries. R1–R6 (3 LOs): ~3.5 ms
  between updates of a seat, ~285 /s.
- **Level filter, 2026-10-11:** our levels jumped around more than a
  RotorHazard node's. Their node firmware (Arduino and S32_BPill alike,
  `RssiNode.h` `FILTER_MEDIAN`) takes a running median of 255 single ADC
  reads at ~1 kHz per node, so ~255 ms, reported with the middle sample's
  time; their server does no filtering. Our median of 3 covered ~10 ms.
  `lap.c` now keeps a running median over 255 ms (at most 255 updates):
  a ring of level codes, a 256-bin histogram (one byte per bin), and one
  timestamp per 8 updates, interpolated; ~950 B per seat. It reports
  nothing until the window first spans 255 ms or holds 255 updates.
  The 8 seats did not fit in the C5's static RAM below the RF dump bank
  (~5.1 KB left), so `rh_init` allocates them from the heap (`BOOT_CHECK`, before
  `scanner_init`): free heap while scanning 39.3 → 34.1 KB. On the C5 at
  manual gain 40 (RSSI, no VTX), each seat's node peak and nadir over 8 s
  stayed within 2–6 codes (1–3 dB). With `--enter 140 --exit 139`,
  `rh_check.py` saw 15 passes and 35 crossing changes in 30 s, with 0 comm
  errors.
- **Level codes in 0.25 dB, 2026-10-11:** with 0.5 dB codes (dBFS = code / 2
  − 127.5) codes 0–129 never occurred: the ADC noise in the ±5 MHz window is
  about one count per I/Q component at every gain up to ~40–50 (gain 0 is no
  quieter), −59 to −62 dBFS. Checked against raw I/Q: the board's RSSI codes
  match a numpy calculation on 16380-sample captures to within 0.5–1 dB at
  gains 0–83. RSSI and SNR codes are now 0.25 dB (dBFS = code / 4 − 63.75, SNR
  dB = code / 4), computed from the 1/256-code integer log before rounding, so
  this is real resolution. The noise floor reads ~13–21 (gain ≤ 50), ~65 at
  gain 60, ~150 at gain 83. The `LEVELS` per-bin floor stays a 0.5 dB
  spectrum code. Defaults EnterAt/ExitAt 190/180 became 125/105 (the same
  dBFS), peak/nadir hysteresis 2 → 4 codes (still 1 dB). The web UI's saved
  thresholds use a new storage key (old ones are dropped: they don't record
  their metric). With the 255 ms median, filtered noise at gain 60 moves 3–7
  codes (1–2 dB) per seat.
- The stray first-command `ERR command` seen with `widscan58cli.py` right after
  flashing came from the tty: after re-enumeration it starts with echo on, and
  whatever the device streamed in the instant before pyserial set raw mode was
  echoed back into the device's command line. With the silent boot there is
  nothing to echo (first command on a fresh tty answered normally).
- Web UI: a menu (Monitor / RotorHazard mode, dark mode) and a RotorHazard page
  that polls like the server (seat levels, EnterAt/ExitAt, level graph, passes
  with lap times). Through the bridge on the C5: 8 seats polled in a ~40 ms
  cycle, 0 failed reads in ~1000.
- Saved defaults (2026-10-11): `CONFIG SAVE` (RotorHazard mode only; it starts
  with `C` because `S` is a RotorHazard write command) keeps gain, FFT size,
  capture length, metric and LO plan in NVS (namespace `widscan58`, blob `rh`,
  versioned and range-checked on load). Boot and every `PROTOCOL RH` (or probe
  takeover) apply them; a metric change resets the seats' levels. Every
  `PROTOCOL MONITOR` applies `METRICS FULL`. The web UI no longer sends
  `METRICS` on entering RotorHazard mode; it reads the metric from `CONFIG?`.
  Checked on the C5: save, monitor round trip and reboot all bring back the
  saved values; an unsaved change is dropped on the next `PROTOCOL RH`.
- FFT size only (2026-10-13): in RotorHazard mode the capture is 4 × the FFT
  size, set by the firmware with `FFT` (`CAPN` answers `ERR rh_mode`), and the
  saved defaults no longer hold a capture length (blob version 2; a version 1
  blob is read without it). `FFT 64` (1.25 MHz bins, 256 samples) is allowed
  in both modes. Model of the ±5 MHz window: noise bandwidth 10.6 → 11.3 MHz
  from 128 to 64 points, a 10 MHz-wide signal 20 MHz away −88 → −72 dB, so
  still far below the image and ADC limits. Measured on the C5 (R1–R8,
  `GROUP`, `METRICS RSSI`): `FFT 128` 208 /s, `FFT 64` 240 /s (DSP 554 →
  367 µs). With 8 × 64 over 512 samples it was slower than 4 × 128 (184 /s:
  FFT −23 µs, prep +134 µs): each segment cost ~33 µs of soft-float setup.
- Per-segment setup in `psd_compute` (2026-10-13): pass 1 now runs over all
  segments first and keeps integer sums per segment; the IQ moments update
  the per-LO state once per capture (weight 1 − (1 − 1/16)^segments, what
  the per-segment updates added up to) and `iq_coefficients` runs once; the
  S16 mean, shift bound and Q14 coefficients are integers. `FFTEVAL SYNTH`
  and `LIVE` match the float reference as before (S16R dmean 0.000 dB, drms
  0.08 / 0.04 dB). Prep and rate on the C5 (R1–R8, `GROUP`, `METRICS RSSI`):

  | Setup | Prep µs | Updates per channel |
  |---|---:|---:|
  | 4 × 128, 512 samples | 276 → 171 | 207 → 227 /s |
  | 4 × 64, 256 samples | 209 → 106 | 236 → 272 /s |
  | 8 × 64, 512 samples | 405 → 180 | 183 → 227 /s |
  | 4 × 1024 (`METRICS FULL`) | 1256 → 1106 | 50.9 → 51.1 /s |

  `IQCORR` now costs ~45 µs per hop at 4 × 128 (it was ~190 µs at 8 × 64).
  Levels with `IQCORR 1` vs `0` differed by up to 1.4 dB on R1/R2 right
  after a boot and 0–0.3 dB after a `RECAL`: the first-calibration problem
  in "Next steps" 8, not this change.
- Settle overlapped with the DSP (2026-10-13): `radio_tune` no longer takes
  or waits a settle time; it records when it moved the LO, and
  `radio_capture(…, settle_us, …)` waits until `now − tune end ≥ settle_us`
  before triggering. `do_hop` captures, then tunes the next
  hop's LO, then runs the DSP, so the settle passes during the DSP (a skipped
  tune, same LO, sets no wait). Checked first that a tune leaves the capture
  buffer alone while the DSP reads it: a checksum before and after the
  pre-tune changed in 0 of ~9000 hops, ~400 of them with a full
  recalibration (`RECALMODE EVERY`). C5, R1–R8, `GROUP`, `METRICS RSSI`:
  `FFT 64` hop 857 → 775 µs (272 → 296 /s), `FFT 128` 227 → 239 /s,
  `METRICS FULL` at 4 × 1024 51.1 → 51.5 /s. `SETTLE 2000` makes the capture
  wait ~1650 µs, so up to ~350 µs of settle is free at `FFT 64`. In
  RotorHazard mode (`rh_check.py`, 8 seats) 3.38–3.41 → 3.18–3.22 ms, a
  smaller gain (~50 µs per hop) than in monitor mode (~80 µs); not explained.
- PLL-only retune on the C5 (2026-10-13, `TUNESKIP 1`; the C5 default is now 3):
  between full tunes (every 2 s, and after a fresh calibration) a hop runs
  only the frequency-dependent parts of `phy_chip_set_chan` (`radio_c5.c`,
  `pll_retune`): `phy_rfpll_set_adc_rate` (ADC rate and RX filter mode; this
  is the 5830/5831 MHz switch above: without it R7 read ~1 dB high, with the
  filter of whichever LO had the last full tune), `phy_set_channel_rfpll_freq_new`
  (band, SDM, 5 GHz ckgen and PLL calibration; given the kHz correction at
  `phy_param+30` that channel setup passes, which `phy_chip_set_chan_ana`
  leaves at 0), the pbus memory (~14 µs) and the crystal duty (~13 µs), plus
  the RX gain table. Left out: AGC off/on, TX power and gain tables, DC
  memory clear, packet-detect DC cal, bandwidth registers.
  - The gain table (`phy_set_rx_gain_table`, ~170–200 µs) is most of what
    is left. It writes 84 entries through `phy_write_gain_mem` (~35 µs of
    it), now wrapped: the first build per LO is recorded and later visits
    replay it. A replay matched a fresh build bit for bit (0 differing
    entries in ~2600 comparisons, `ONCE` and `CACHE`). The table build also
    sets the AGC's maximum gain (`phy_param+292..294`,
    `phy_agc_max_gain_set`), which differs between LOs: replaying the
    entries alone read 2–5 dB high on R1/R2 with `RECALMODE ONCE`, so it is
    replayed too. Slots (8, ~6 KB heap) follow the calibration.
  - Keeping the tables fresh: at first each slot expired after 1 s and was
    rebuilt. All of a plan's tables were built together, so they expired in
    the same rotation: 4 rebuild tunes of ~420 µs once a second, a ~2.8 ms
    rotation (~355 /s) in RotorHazard mode's `loop_time`, which only reports
    the latest interval. Now the full tune records the table channel setup
    builds, and takes the plan's LOs in order, waiting at most a rotation
    for the next one, so each LO is refreshed once per cycle of full tunes.
    The scanner passes the plan's LOs with `radio_set_plan` (a first version
    inferred them from recently tuned LOs and picked the oldest table).
    Hop log (then at 200 ms): full tunes 5899, 5677, 5751, 5825, 5899, …
    ~202 ms apart, no rebuild tunes; the rotation with the full tune still
    took ~2.7 ms (~370 /s), 5 times a second.
  - Full tune interval 200 ms → 2 s. The 200 ms came with the S3 backend, a
    guess "in case the Wi-Fi driver touches the RX path", never measured.
    Test: 80 s in monitor mode (`METRICS SNR`, `FFT 64`, R1–R8 `GROUP`), a
    VTX carried around on R1: 10 s normal, 60 s with no periodic full tune
    (a temporary `TUNESKIP` bit; the 2 s table expiry still rebuilt the
    tables), 10 s normal. No full tune in the 60 s. The empty channels
    (R2–R8) held their RSSI and floor within ±0.2 dB in every 5 s window of
    all three phases (R7/R8 drifted −0.3 dB over the 80 s, in all phases
    alike), and the full tune forced at the end moved them by 0.00–0.12 dB
    (2 s before vs after). R1 read −21 to −50 dBFS as the VTX moved, peak
    SNR 35–46 dB in each phase. Now each LO's table is refreshed every
    LOs × 2 s (8 s for R1–R8 `GROUP`, 16 s for 8 LOs); the expiry backstop is
    (8 + 2) × 2 s = 20 s. Hop log: full tunes 2.0 s apart in plan order
    (5677, 5751, 5825, 5899), no rebuild tunes. Not covered: temperature
    changes over minutes, Wi-Fi driver activity.
  - Timing moves with the build by up to ~100 µs per hop with no change to
    the code involved (this last build: DSP 305 → 345 µs, 8 seats in
    RotorHazard mode ~2.2 → ~2.4 ms): code placement and flash cache misses.
    Fixed by running the scan loop from IRAM (next entry).
  - Tune (median, R1–R8, `GROUP`): 607 → 299 µs. Same build, R1–R8,
    `METRICS RSSI`: `FFT 64` 216 → 337 /s, `FFT 128` 182 → 261 /s; `METRICS
    FULL` (4 × 1024) 50 → 54 /s; `MODE SINGLE` at `FFT 64` 108 → 159 /s.
    `TUNESKIP 3` (also no RX path re-apply) saves ~35 µs more (376 /s), same
    levels; now the C5 default (RotorHazard mode, 8 seats: ~2.2 ms). The
    mask is reported as `tskip` in `STATUS?`/`CONFIG?`, and the UI's Scanner
    controls have a toggle per bit. `TUNESKIP 7` reads +27 dB: the PLL-only
    tune resets the manual gain too. Checked 2026-10-14 (gain 50): both kinds
    of tune clear the force bit (23) of 0x600a702c and keep the index (bits
    24–31), so `0x32c053e2` after `force_rx_gain` reads `0x324053e2` after
    every tune (~1000 PLL-only, 14 full). A write only when the force is gone
    (FPVGateC5MK be70d8c, against post-hop spikes from rewriting a force
    that still holds) would always write here; and our write comes before
    `SETTLE`, theirs right before a capture.
  - Levels vs the full tune (`CACHE`, dummy load, gain 50): channel RSSI and
    floor within 0.1–0.3 dB at `FFT 64`, 128 and 1024, per-bin floor within
    0.1 dB at every LO, DC within its run-to-run spread, same flags. Board
    spurs at 5680/5760/5920 MHz within ±0.4 kHz in `RAWIQ` captures, as with
    the full tune. `RECALMODE ONCE` scatters ±0.5 dB between runs either way.
  - RotorHazard mode (`rh_check.py`, `loop_time` median): 4 seats 1.6 → 1.22
    ms, 6 seats 2.4 → 1.81 ms, 8 seats 3.2 → 2.45 ms; 8 seats SNR 2.59 ms,
    `FFT 128` 3.22 ms, `FFT 256` 4.80 ms, `SINGLE` 4.89 ms. 2 minutes with 8
    seats: no comm errors, no resets.
  - The full-tune numbers move with the build: HEAD measured 473 µs tune,
    304 µs DSP and 284 /s at `FFT 64`; this build with `TUNESKIP 0` 607 µs
    and 404 µs with no change on that path. Both the PHY code and our DSP run
    from flash through the cache, and the DSP time also drops with the
    shorter tune (404 → 340 µs), so code placement decides some of it.
- Scan loop in IRAM on the C5 (2026-10-13). Hop times moved by up to ~10 %
  between builds with no change to the code involved: the scan loop ran from
  flash, and its cache misses depended on where the linker put everything.
  Now in IRAM: our hot path (`do_hop`, `psd_compute`, `metrics_level`,
  `metrics_floor_raw`, `lap_feed`/`rh_feed`, frame output, `tune_locked` with
  the PLL-only retune and gain table replay, `radio_capture`, the cal cache
  lookup; `IRAM_ATTR`), and through `src/radio/c5/hot_iram.lf` the libm
  helpers it calls (`lrintf`, `ceilf`, `floorf`, `sqrtf`, `ldexpf`, …; the
  soft-float routines are already in ROM) and the PLL-only retune's PHY
  functions (~4 KB, by section name: libphy is linked as `-lphy`, so ldgen
  cannot map its functions by symbol). `metrics_channel` and the rest of
  `METRICS FULL`, the spectrum and full channel setup stay in flash. Found
  with a call-graph walk over the linked ELF from `do_hop`.
  - Room: IRAM and static data share the SRAM below the RF dump bank
    (0x40800000–0x40820000), which had ~4.2 KB free for ~20 KB of code. Made
    by moving the cal cache (5.6 KB) and the gain tables (6.3 KB, from the
    heap) into the top 24 KB of the dump bank's lower half (the DSP keeps
    40 KB of it; it uses ~37 KB), `seg_stats` (768 B) into psd's region, the
    scanner state (4.2 KB) and the command reply buffer (3 KB) to the heap,
    and IDF's heap functions to flash (`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`,
    1.5 KB). IRAM 74.7 → 92.5 KB; 264 bytes are left below the bank. Free heap
    in monitor mode 24.9 → 19.9 KB (~1 KB more than the moves account for,
    not explained).
  - Test: a block of dead code (0, 1, 3, 7 KB) in flash moves everything
    linked after it. Per build, 3 s per mode, `FFT 64`, `METRICS RSSI`, R1–R8
    `GROUP`:

    | Pad | IRAM, `TUNESKIP 3` | Flash (previous commit), `TUNESKIP 3` |
    |---:|---|---|
    | 0 | 464.1 /s, tune 225 µs, DSP 255 µs | 329.5 /s, tune 298 µs, DSP 346 µs |
    | 1 KB | 464.4 /s, 225, 254 | 320.2 /s, 309, 378 |
    | 3 KB | 464.4 /s, 225, 254 | 308.2 /s, 314, 377 |
    | 7 KB | 464.1 /s, 225, 254 | 331.9 /s, 307, 349 |

    With `TUNESKIP 0` (full channel setup every hop, still from flash):
    318.0–318.2 /s vs 227–243 /s. Levels with both tunes within 0.16 dB.
  - Now: `METRICS FULL` (4 × 1024) 60 /s (hop 4.0 ms: tune 0.23, DSP 3.65);
    `METRICS RSSI` `FFT 128` 332 /s, `SNR` 318 /s. RotorHazard mode
    (`rh_check.py`): 8 seats 1.96 ms (~510 /s), 6 seats 1.52 ms, 4 seats
    0.98 ms; 8 seats SNR 2.0 ms, `FFT 128` 2.73 ms, `FFT 256` 4.29 ms,
    `SINGLE` 3.86 ms. No comm errors.
  - PlatformIO's builder runs ldgen into `.pio/build/esp32c5/sections.ld`
    and does not redo it when a `.lf` file changes; delete it after editing.
- Not yet tested with a VTX flying through a gate: EnterAt/ExitAt calibration
  for real passes, pass-time accuracy vs RotorHazard's RX5808 nodes, and RSSI vs
  SNR as the level.
- Gain and output filter (2026-10-16), after a RotorHazard developer's report:
  at the default gain 50 the levels pinned at 254 with a VTX within ~3 m
  (antenna on), while FPVGateC5MK 1.2.1 (gain 30) peaked at 2–3 m and did not
  clip at 0.3–1 m. Both measure the power in ~10 MHz around the channel from
  256 samples at 80 MS/s and map dBFS to nearly the same 0.25 dB code (theirs:
  dB in LSB², −10…53.75 onto 0…1023, ÷4 ≈ 4 × (dBFS + 64.2)), and both force
  the same PHY gain index, so the ~20 index steps (~15–20 dB, from our floor
  vs gain at 50–83) are the difference. At 50 the receiver's own noise already
  lifts the floor ~5 dB over the ADC's; at 30 the floor is the ADC's.
  - RotorHazard mode's built-in gain became manual 30 (monitor mode kept 50);
    since the gain sweep below, 45 in both.
    A saved gain still wins.
  - `LEVELFILTER MEDIAN|EMA`: the 255 ms median (default) or FPVGateC5MK's
    filter, a median of 3 and an EMA (their alpha 0.3 at ~1030 /s, τ = 2.72
    ms), with the weight 1 − exp(−dt/τ) from each update's interval (integer
    (2,2) Padé, within 1 % up to dt = 2τ) so τ holds at any rate. Saved with
    `CONFIG SAVE` (NVS blob v3; v1/v2 load as `MEDIAN`), in the web UI's
    Receiver card. The median costs little peak height at gate distances
    (±64 ms around the closest approach: ~0.4 dB at 3 m, 15 m/s), so the
    filter is a preference, not the fix; the gain is.
  - Not yet checked with a VTX: passes with each filter. (The gain → dB
    step: see the gain sweep below. The built-in gain is now 45.)

**Gain sweep (2026-10-16).** `GAIN MANUAL 0..83` (gain_max 83), 5 s per
index, in RotorHazard mode polled by RotorHazard's own interface code
(`RHInterface.update()` every 100 ms, as its server), 8 seats R1–R8, `FFT 64`,
`METRICS RSSI`, `MODE GROUP`, `LEVELFILTER MEDIAN`, `IQCORR 1`; first 0.5 s
after each change dropped. 25 mW VTX on R1, 0.9 m from the board (XIAO
ESP32-C5, no antenna fitted). Floor = median
of R2, R6, R7, R8. Run 1 (room occupied) and run 2 (another 25 mW VTX, room
empty); the numbers are run 2's, whose R1 held within ~1 dB per step (run 1's
wandered ±10 dB with people moving). Data and scripts: `tmp/gain-sweep/`,
`tmp/gain-sweep-2/` (not tracked).
- Gain per index, from R1's step-to-step change: 0.83 dB/index over 18–44,
  1.02 over 45–72, 0.98 over 25–72. Above ~25 the index is ~1 dB.
- Floor: ADC-limited, −60…−58 dBFS (codes ~13–24), from 10 to ~44, as a
  sawtooth: +0.3–0.6 dB per index, then a drop at 10 (−4.1 dB), 17 (−1.5),
  25 (−3.9), 30 (−1.0) and 38 (−1.5); at 6, 15 and 36 only some LO pairs
  drop. Identical in both runs, so it belongs to the gain table, not the
  signal. 0–9 is worse than 10–14 (floor 4 dB higher for the same signal).
  Above ~45 the receiver's noise dominates: +0.92 dB per index.
- The VTX is within 1.4 dB of the floor up to 13; 14: 2.5 dB, 17: 3.9 dB.
- Dead signal steps: 24 (+0.0 dB) and 30 (−0.25). Then 25 gives +1.5 dB
  with the floor −3.75 (margin +5.3 dB in one index), and 38 gives +1.5 with
  the floor −1.1. Reading: total gain keeps rising ~1 dB per index while
  gain moves between stages, and the floor falls where the later (noisier)
  stage gives some up. Not a bitfield in the index: the drops are not at
  multiples of 8 (8, 16, 24, 32, 40 are ordinary steps), are 7–15 apart, and
  move between LOs (the table is built per LO, `phy_set_rx_gain_table`).
  Not yet confirmed: dumping `gain_slots` (w0/w1/w2 per index) and diffing
  neighbouring entries would show which fields change at 10/17/25/30/38.
- R1 margin over the floor: 30: 12.9 dB, 38: 20.6, 45: 25.6, 50: 27.9; flat
  at 30.4–30.9 dB from 54 to 75 (floor and signal rise together), then
  falling (23.75 at 83).
- Clipping: R1's steps shrink from 76 (+0.4, +0.4, +0.25 dB) and it pins at
  254 from 78. R2, in the same 80 MHz capture, reads below the quiet seats
  from 74 (R1 at about −2 dBFS) and 6 dB below at 83, so the clip shows on
  the LO's partner channel a few dB before R1's code reaches 254. Run 1: the
  same from 76.
- Empty seats at high gain: R5, R6, R8 sit 1–2 dB above R2, R7, steadily
  (per-channel floor, not traffic). Wi-Fi ch 149 never showed: max − median
  ≤ ~1 dB per 5 s on every seat (the 255 ms median also removes bursts).
- Race captures at gain 30 (`tmp/rh-levels/`) peaked at codes 52–76; the
  bench at 30 read 68, so 0.9 m is about a real pass's level.
- Default gain 45 (both modes; the web UI's default too, was 50; monitor
  firmware was 50, RotorHazard mode 30). Races run 25 mW, and the timer
  stands at a gate corner: 1.52 m opening, passes at ~0.3–2.15 m, +9.5 to
  −7.6 dB vs 0.9 m. At 45: ~19 dB headroom at 0.3 m, ~18 dB margin at the far
  corner before antenna and median-filter losses. 48 would give ~2 dB more
  margin and ~3 dB less headroom; past ~54 gain buys no margin.
- Open: the RotorHazard developer's report (levels pinned at 254 at gain 50
  with a VTX within ~3 m, antenna on) is ~35 dB stronger than this bench
  (25 mW at 0.9 m, no antenna: code 151, −26 dBFS at 50). The missing
  antenna likely accounts for much of that, so with an antenna fitted 45's
  headroom is smaller than measured here. Repeat the sweep with the antenna
  on before relying on it.

**Image rejection vs IQ calibration on the C5 (2026-10-10).** esp-sdr 77ffc12 stops redoing the (transmitting) loopback IQ calibration on retune and keeps the startup IQ. Tested with an analog VTX at low power on R7 and R1, antenna on the C5, gain 66, `RECALMODE EVERY`; LO stepped so the VTX sat at ±4…±36 MHz, 8 × 9180-sample captures per point, image rejection = VTX power in ±4 MHz around its offset vs the mirrored band (floor subtracted). The VTX was only ~23–28 dB over the floor in that band, so values above ~30 dB are floor-limited.
- Per-LO IQ (current): R7 27–34 dB, R1 24–33 dB.
- Startup IQ only (esp-sdr): ~20 dB at every offset at both channels (R7 18–21, R1 20–22). The startup words (+222: `f5 04 f4 04 f0 07 f0 06 ee 09 ec 0d e9 11`) are far from the per-LO ones at 5.6–5.9 GHz (`00 12 … 01 12`), which barely change across the band. In pair mode R7's image lands 1 MHz from R8 (and R1's from R2), so this would put a ~−20 dBc ghost on the partner channel.
- IQ zeroed: about as good as per-LO at R7 (not tried at R1).
- IQ measured once at 5795 MHz, then DC-only per LO: R7 28–35 dB, R1 25–31 dB, the same as per-LO within the measurement. The −20 MHz point at R7 (22 dB in every mode) has the 5920 MHz board spur in its mirror band.
- Pair-mode ghost (VTX close on R1, gain 50, R1 at about −8 dBFS): in GROUP R2 read ~20 dB below R1 (SNR 14, SIG 99 %); in SINGLE R2 was at the floor. It is R1's image (−19 → +19 MHz, inside R2's ±5 MHz window). Image rejection vs offset at fresh LOs was 40–50 dB, but 20.7 dB at the cached LO 5677. In 4 of 8 reboots the first calibration of 5677 had ~20 dB; `RECAL` gave 30–32 dB in every case (10 fresh calibrations at 5677: 37–40 dB by the raw-capture measure). Even then R2 stays ~13 dB over its floor with a VTX this strong. A moment-based digital IQ correction (E[I²], E[Q²], E[IQ] per capture) applied offline gave 55–58 dB at the same captures, the bad-calibration one included.
- Now in the DSP (`IQCORR`, default on; `psd_iq_t`): pass 1 also sums I², Q², I·Q per segment; the centered moments are averaged per plan hop (weight 1/16 per segment, reset on config change and `RECAL`), and pass 2 rebuilds Q as c1·Q + c2·I (Q14; 1, 0 is bit-exact with it off). Estimates beyond a 0.7–1.4 gain ratio or |ρ| > 0.3 are ignored. A Python model of the integer path on the bad-calibration captures: 20.7 → 49–55 dB from the first capture (that calibration's error was mostly gain, Q ≈ 1.2 × I). Cost on the C5: DSP 3.72 → 3.90 ms per hop, hop 4.54 → 4.71 ms. No signal: floors, RSSI and its sd unchanged on all channels.
- With the VTX on R1 (SNR 35–40 dB), GROUP, a bad first calibration after boot: R2 SNR 15.5–18.5 dB, SIG 100 % with it off; −0.7 dB, SIG 0 % with it on (toggled twice). With a good calibration: 0.5–2.2 dB off, −0.6 dB on. SINGLE (R2 on its own LO): −0.2 dB either way. `RAWIQ` stays uncorrected. Why the first calibration after boot is bad about half the time is still open.

**Analog filter and R8 on the C5 (2026-10-08).**
- The noise floor (dummy load) is flat to ±37 MHz at LOs up to 5830 MHz, but from 5831 MHz up it follows the filter skirt: −2.5 dB at ±22–28 MHz and −9.5 dB at ±34–40 MHz relative to the center, with the center level unchanged. The switch sits exactly between LO 5830 and 5831 MHz and is repeatable; recalibrating (`RECALMODE EVERY`) does not change it, and `FILTER 60` moves the skirt inward. So it is a PHY setting that changes with frequency (most likely inside `phy_set_chanfreq`; 5830 MHz is Wi-Fi channel 166, near the top of UNII-3), not RF roll-off. In Raceband it only affects the R7/R8 LO.
- Filter response, measured with board spurs as test tones (the LO stepped in 2 MHz steps so a spur crosses the baseband; spur peak level in 9.8 kHz bins, 3 captures per point; the 0 MHz point is lost to DC removal):
  - Above 5831 MHz (5920 MHz spur, clean): code 0 is −3 dB at ±23 MHz, −6 dB at ±26, −10 dB at ±29.5, −20 dB at ±34; code 20 is −3 dB at ±17.5, −6 dB at ±19.5.
  - Below 5831 MHz (5760 MHz spur; the spur level scatters ±3 dB from LO to LO, so only coarse edges): −6 dB below the passband plateau at about ±32 MHz for code 0, ±20 for code 20, ±14 for code 40 and ±12 for code 63. Code 0 looks somewhat wider here than above 5831, consistent with the flat floor.
  - The profile's `passband_hz` of ±24 MHz matches code 0 above 5831 MHz and is conservative below.
- R8: LOs up to 5958 MHz tune and capture normally, and the spurs appear at their expected offsets (within ±30 kHz) at every LO, so tuning above esp-sdr's validated 5895 MHz works. From the pair LO 5899, R8 (+18 MHz) sits ~1.7 dB down the code-0 skirt and its ±5 MHz RSSI window spans −1 to −2 dB; R7 (−19 MHz) is about −1.3 dB. R8 is usable; a response EQ (`CALFLAT`, §4.5) would remove the tilt. The 5920 MHz board spur is 3 MHz from R8, inside its RSSI window (masked, see below).

**Tune time on the C5 (2026-10-08): 1.22 → 0.68 ms per hop; hop 4.93 → 4.38 ms, 51 → 56 rotations/s (pair mode, `RECALMODE CACHE`).**
- Profiled `phy_set_chanfreq` (~980 µs) with temporary linker wraps: the cross-object callees timed with the cycle counter, and `ets_delay_us` per call site. Calls inside `phy_rfpll.o` cannot be wrapped.
- Largest items: a fixed 300 µs `ets_delay_us` at the end of `phy_set_rfpll_freq`; `phy_set_rx_gain_table` (~160 µs, rebuilds the RX gain memory from the DC tables); `phy_wifi_set_tx_gain_new` (~85 µs, the TX gain table); 14 `phy_i2c_writeReg_Mask` + 11 `phy_i2c_readReg` (~75 µs); `phy_freq_i2c_data_write` (~46 µs). Settle (`SETTLE`, 100 µs) is outside it.
- The 300 µs wait is redundant: right after it, `phy_wait_rfpll_cal_end_new` restarts the PLL calibration itself, waits 20 µs and checks the VCO code (retrying up to 10 times while it reads 125–130). With the wait skipped, spurs land within 4 kHz of their frequency after 50–350 MHz jumps (300 captures, none off; the stock sequence had 2 off-frequency captures in 300, both raised-floor bursts).
- Now skipped while `radio_tune` runs channel setup (`--wrap` of `ets_delay_us` and `phy_wifi_set_tx_gain_new`, gated by a flag): the 300 µs wait and the TX gain table. `c5_cal_restore` no longer rebuilds the gain table itself, since channel setup does (that was a duplicate ~160 µs per hop in `CACHE` mode). Full-spectrum comparison against the stock sequence at five LOs: floor within 0.07 dB, no new lines, same DC.
- What remains in channel setup (~560 µs) is mostly the gain-table rebuild (~170 µs) and I2C register traffic. Further cuts would mean a PLL-only retune like the S3's (`phy_set_rf_freq_offset`), which skips the per-frequency settings that channel setup applies (band, pbus memory, RX compensation, the 5831 MHz filter change), so it needs care. (Done 2026-10-13: "PLL-only retune on the C5" above.)

**Peak, center and occupied BW are time-averaged (2026-10-08).**
- With a VTX on R1 (SNR ~42 dB), the per-snapshot values scattered: peak sd 1.3 MHz, centroid sd 0.84 MHz, −10 dB width mostly 0.2–1.1 MHz. Recomputing them in Python from raw captures gave the same numbers, so the arithmetic was right; the definitions were the problem. A capture is 51 µs, less than one 64 µs line, and the carrier sat at about +1.5 MHz (picture level) with ~5 µs dips to −3.5 MHz (sync) every line, so each snapshot saw one or two of those.
- Now (see §4.5): most occupied frequency +1.42 MHz (sd 0.02), center −0.05 MHz (sd 0.07), 99 % band −4.26 … +4.73 MHz (8.98 MHz, sd 0.09). Channels without a signal report no center. Cost ~70 µs per hop (metrics 0.33 → 0.40 ms, independent of the weight) and ~4 KB static RAM. Those numbers are at weight 1/32; the default is now 1/128 (`AVG 7`).
- The frame's `obw_hz` (u32) is now `obw_lo_khz`/`obw_hi_khz` (two int16, vs channel nominal); the UI shows "OBW 99 %" and draws the band in the zoom view.

**Board spurs on the C5 (LO sweep, 2026-10-08).**
- Method: scanning stopped, dummy load on the antenna port, manual gain 40, `RECALMODE ONCE`. LO 5600–5975 MHz in 5 MHz steps; at each LO, 4 × 9180-sample `RAWIQ` captures at 80 MS/s, 2048-point Hann FFTs (39 kHz bins), averaged. A line is a bin ≥ 6 dB over the median of the surrounding ±1 MHz, with |offset| 0.4–30 MHz from the LO. A board spur is a line fixed in RF that is seen from ≥ 75 % of the LOs that cover it; LO-relative lines and IQ images move with the LO and drop out.
- In `spurs_khz` now: **5600, 5680, 5712, 5760, 5920 MHz**, each 7–10 dB over the floor (up to 15 dB at some LOs) and seen from 5/6 to 11/12 of the covering LOs. 5600/5680/5760/5920 are multiples of 80 MHz; 5712 (×119) and 5760 (×120) are 48 MHz crystal harmonics.
- Weak or intermittent, not masked: 5616, 5664 and 5808 MHz (48 MHz × 117/118/121), seen from only 3–4 of 12 covering LOs, at 6–12 dB. 5840 and 5856 MHz did not show up with the dummy load (5856 was seen at bring-up with an antenna).
- Effect on channels (`spur_guard_hz` 60 kHz; masked bins are left out of the floor, peak and centroid, and RSSI fills them with the window mean): 5920 MHz is 3 MHz from R8 (5917), inside its ±5 MHz RSSI window, and 5760 MHz is 9 MHz from R4 (5769). Both channels are always flagged `SPUR` (a spur within ±12 MHz of the channel). No other channel has one of these spurs within ±12 MHz.

Next steps, in order:
1. ~~**Capture latency**~~ (done 2026-10-08: 1.79 → 0.13 ms per capture, see "Capture latency on the C5" above).
2. ~~**DSP**: merge FFT stage 1 into the windowing pass, and special-case the twiddle-free butterflies (j = 0)~~ (tried 2026-10-08, not kept: together they saved ~50 µs of a 4.9 ms hop, not the ~0.2–0.3 ms estimated).
   - Merging stage 1 into the windowing pass (bit-exact on the host at 64–1024 points) moved its cost instead of removing it: FFT 1935 → 1576 µs, but prep 881 → 1185 µs, and the same with the merged loop in IRAM. Stage 1's cost is its arithmetic, not the extra pass over memory.
   - Exact j = 0 butterflies (no multiplies, ties to even): FFT 1935 → ~1915 µs; host bias at 1024 points 0.0042 → 0.0029 LSB; `FFTEVAL` vs R4 unchanged (+0.002 dB, 0.080 dB rms).
   - The kernel is near its limit for scalar code: ~90 cycles per radix-4 butterfly at 240 MHz (12 multiplies plus loads/stores). Real savings would come from doing less work: `FFT 512` (roughly halves the FFT, 156 kHz bins) or fewer Welch segments (`SEGMENTS`), or from the tune (`phy_set_chanfreq`; 1.2 → 0.68 ms since, see "Tune time on the C5" above).
3. ~~**Calibration.**~~ (done 2026-10-08: per-LO calibration cache, now the C5 default; see "Calibration on the C5" above). ~~Image level~~ (measured 2026-10-10 with a VTX, and now corrected digitally; see "Image rejection vs IQ calibration on the C5" above).
4. **RF characterisation** (Phase 0-C5 step 2): ~~board spurs by LO sweep~~, ~~the real width of filter code 0~~ and ~~whether R8 (5917 MHz, above esp-sdr's validated 5150–5895 MHz) is usable~~ (done 2026-10-08, see "Analog filter and R8 on the C5" above). Still open: alias rejection (R3 seen from LO 5677 at −24 MHz), which needs a strong signal 40–80 MHz from the LO (a VTX).
5. **Profile.** Finalise `c5-race` (pair vs. single mode, filter code, windows), then run the Phase 1 acceptance criteria with real VTXs.
6. ~~`BOOTLOADER` on the C5~~ (done 2026-10-08): the board enters ROM download mode, esptool connects with `--before no-reset`, and `--after watchdog-reset` returns to the app.
7. The S3 firmware picked up the shared changes (`BOOT_CHECK`, `rawiq_max`, `link_frame_wait`, the integer PSD/metrics/spectrum, the u32 frame timings). It builds but hasn't been reflashed or rerun on the S3; its S16R is still the SIMD radix-2 kernel.
   - **PHY lock and stale-calibration detection on the S3.** The S3 runs the same 1 Hz PHY tracking, and `radio_s3.c` tunes and calibrates without `phy_get_lock()`, so the hang seen on the C5 (see "PHY tracking on the C5") can happen there too. Take the lock in `radio_tune`; for stale detection, esp-sdr wraps `set_rx_gain_cal_dc` (10 word arguments) on the S3. Check first whether the S3's tracker recalibrates RX (its `phy_param_track_tot` callees), and reproduce with the threshold forced low as on the C5.
   - **Test whether `RECALMODE CACHE` would help the S3.** On the S3, `CACHE` currently behaves like `EVERY` (no cache; its calibration snapshot layout is uninvestigated, §4.4). First repeat the C5 comparison of `ONCE` vs `EVERY` there: DC offset per LO across the band, and bursts of raised floor (dummy load, single mode, several `RECAL`s). If `EVERY` is clearly better, find what the S3 calibration writes (diff its `phy_param` areas as on the C5) and port the cache; if not, keep `ONCE`.
8. **Bad first calibration after boot on the C5.** In 4 of 8 reboots the first calibration of LO 5677 left ~20 dB image rejection (mostly an I/Q gain error, Q ≈ 1.2 × I); `RECAL` always fixed it (37–40 dB). `CACHE` keeps a bad entry until `RECAL` or a tracking recalibration. `IQCORR` hides it from the scanner, but `RAWIQ` still shows it, and it may be related to the bursts of raised floor seen in the first ~20 calibrations of a session under `ONCE` ("Calibration on the C5"). Find out whether it is the first calibration after boot or the first per LO (log the IQ words at +222 per calibration), then fix it (e.g. a throw-away calibration in `radio_init`, or recalibrate when the IQ words are implausible).
9. **IQ correction cost.** `IQCORR` adds ~0.17 ms per hop at 1024 points (3 multiplies per sample for the moments in pass 1, 2 in pass 2). Once a hop's estimate has settled, the moments could be updated on every Nth capture only, leaving just the pass-2 multiplies. ~~Per-segment soft-float setup~~ (done 2026-10-13: IQ coefficients once per capture, integer means; see "Per-segment setup in `psd_compute`" above).
10. **Revisit the IQ part of the PHY calibration.** With `IQCORR`, per-LO IQ calibration (a transmitted loopback tone each time) matters less. Options measured with the VTX: IQ once at the band center then DC-only per LO matched per-LO IQ; startup IQ only (esp-sdr's policy) gave ~20 dB, which `IQCORR` should now fix up as well (untested). Either would also shrink cache entries to the DC tables.

## 7. Risks and open questions

| Risk | Mitigation |
|---|---|
| PlatformIO can't use the pinned IDF → wrong `libphy` ABI | Git-URL `platform_packages`; `idf.py` fallback; boot-time PHY version check |
| Retune/recal too slow | Per-LO calibration cache, pair mode, only scan enabled channels |
| S3 mock behaves differently from the C5 (aliasing, filter skirt, crowded band) | Documented in §2.2; alias-emulation profile; response EQ; keep the S3 for pipeline/UI/IMD logic, not for RF characterisation of the C5 |
| S3 13 MHz filter isn't actually 13 MHz at 2.4 GHz | Measure in Phase 0; choose the code that matches k × the C5 width |
| Wi-Fi/BLE ambient traffic pollutes S3 baselines | Bursty-interference flag, percentile floors, test in a quiet RF spot or with a shielded box/cable |
| Uncalibrated absolute power | dBFS is the primary unit; dBm uses the gain table plus a user offset |
| Our own receiver produces IMD and pollutes the IMD statistics | Gain-step discrimination (§4.8), saturation tracking |
| C5 R8 above the validated range | Measure; document |
| PHY background tracking recalibrates under us | Tune under `phy_get_lock()`; detect its DC measurements and recalibrate (C5 done, S3 open) |
| Pair-mode images (a strong channel shows on its partner) | Digital IQ correction (`IQCORR`); per-LO PHY IQ calibration |

**Open questions (defaults assumed):**
1. S3 board: ESP32-S3 Micro (native USB only, BOOT + EN buttons). C5: Seeed XIAO ESP32-C5 (rev v1.0, 8 MB flash; built as `esp32-c5-devkitc-1` with 4 MB).
2. ~~testgen~~: deferred by request; the S3 work uses ambient 2.4 GHz traffic.
3. Raceband only for v1? Assumed yes, with a generic channel table.
4. Lap and IMD analytics in the browser first, firmware later? Assumed yes.
