# esp-rx5808 — ESP32 FPV Spectrum / RSSI Scanner (C5 @ 5.8 GHz, S3 @ 2.4 GHz)

**License:** GPL-3.0-or-later. The project builds on code from
[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr) (GPL-3.0-or-later). Every
file copied or derived from it keeps its notice and gets a header naming the source.

The project has two parts:

1. **Firmware** (PlatformIO + ESP-IDF). It reads raw I/Q samples through the ADC/DAC
   debug bypass that esp-sdr found, steps through a set of FPV channels one after
   another (round robin), and reports per channel: RSSI, noise floor, SNR, and the
   estimated centre of the video signal. It also streams a reduced spectrum for
   display. Two targets share one codebase:
   - **ESP32-C5:** the real target. Raceband R1–R8 at 5.8 GHz.
   - **ESP32-S3:** a **scaled mock-up** of the C5 system in the 2.4 GHz ISM band.
     We have S3 hardware now, so the S3 is where the pipeline, protocol and UI get
     debugged while we wait for the C5 devkits.
2. **Web UI** (Web Serial): live per-channel signal strength, channel enable/disable,
   and debug/spectrum views (band panorama, waterfall, per-channel zoom, timing and
   health telemetry, and later intermodulation (IMD) statistics).

**Main design rule:** everything band-specific (channel centres, window widths,
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
| Centre search window | ±12 MHz |
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
- Centre of channel *i* (*i* = 0…7) = 2400 + 5.21875 × (2*i* + 1) MHz

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
     tones, at the scaled channel centres (esp-sdr's PHY cert-test TX path; exact
     API to verify).
  2. A signal generator or HackRF, if available.
  3. Ambient Wi-Fi/BLE.

  `testgen` can also turn several "VTXs" on and off on a schedule, which drives the
  IMD statistics work.

## 3. Repository layout

```
esp-rx5808/
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
    ├── rx5808cli.py             # pyserial client: dump frames, record, benchmarks
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
framed `LOG` messages.

### 4.3 LO planner (profile-driven)

**Pair mode (default):** the LO sits halfway between neighbouring channels. The DC
spike lands in the gap between channels, and 8 channels take 4 hops.
**Single mode:** LO = channel centre + profile `single_offset_hz`.

The planner takes the enabled-channel mask and greedily pairs neighbours whose
spacing is ≤ the profile's `pair_max_spacing_hz`. It outputs LOs with
`(channel_id, offset_hz)` lists. A panorama mode sweeps the whole profile band in
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

Target: ≤ 2.5 ms per hop, so ≥ 100 Hz per channel with all 8 enabled.

### 4.5 Per-hop DSP (`psd.c`, `metrics.c`). All sizes come from the profile.

1. Snapshot capture: C5 4096 @ 80 MS/s, S3 8192 @ 40 MS/s.
2. Saturation count (|I| or |Q| ≥ 510).
3. Welch PSD. Hann window, Q15 FFT (S3: SIMD kernel from esp-sdr; C5: portable),
   linear float accumulation. Optional halfband ÷2 first (S3 alias-emulation
   profile).
4. Per-FFT DC removal. Bins [−1, 0, +1] are excluded from metrics.
5. **Response EQ:** subtract the per-LO median noise baseline stored by `CALFLAT`.
6. Per channel:
   - **RSSI**: integrated power over ±rssi_window, in dBFS, plus approximate dBm
     via the gain table and a user offset.
   - **Peak**: highest bin in ±search_window, refined with parabolic interpolation.
   - **Centroid / video centre**: power-weighted mean of bins more than 6 dB above
     the noise floor.
   - **Occupied BW**: −10 dB width.
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

Host → device: ASCII lines; each replies `OK …` or `ERR <reason>`.

| Command | Purpose |
|---|---|
| `INFO` | `RX5808 <target> <proto_ver> <fw_ver> <phy_ver>` |
| `PROFILE?` / `PROFILE <name>` | Dump the active band profile as JSON (channels, windows, LOs, rate, filter, span), or switch profile (e.g. `s3-race24`, `s3-race24-alias`, `c5-race`) |
| `CHANNELS?` / `CHANNELS <id:kHz>,…` | Read or override the channel table |
| `ENABLE <mask>` / `ENABLE?` | Enabled-channel bitmask |
| `MODE PAIR\|SINGLE\|AUTO` | Hop strategy |
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

| Frame | Contents |
|---|---|
| `METRICS` (per hop) | seq, t_us, LO kHz, gain idx, sat count, noise floor (cdB), hop timing (tune / capture / dsp µs), then per channel: `{id, rssi, rssi_ema, snr, clear_floor (cdB), peak_off, centroid_off (Hz/10), obw (kHz), flags}` |
| `SPECTRUM` / `PANO` | seq, LO kHz, start kHz, bin Hz, n, detector, u8 codes[] |
| `TELEMETRY` (≈ 4 Hz) | rotation Hz, hop timing min/avg/max, CPU ‰, heap, TX drops, temperature, recal age per LO |
| `EVENT` (Phase 4) | channel on/off transition plus the before/after delta vector |
| `RAWIQ` | n, rate, LO, IQ8 payload |
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
profile, so the same UI shows "S1–S8 @ 2.4 GHz" for the S3 and "R1–R8 @ 5.8 GHz"
for the C5. A banner marks the S3 as a scaled mock-up and offers a "show equivalent
5.8 GHz frequencies" toggle (axes multiplied back by 1/k).

**Transport:** `requestPort` filtered to `303A:1001`. Stream parser splits text
lines from frames, checks CRC, counts resyncs. One outstanding command at a time
while streaming. Automatic reconnect after a board reset.

**Mock device** (`web/src/mock/`): generates frames for either profile (moving
carriers, drifting VTX, Wi-Fi bursts, saturation, scheduled VTX power-ups with IMD3
products), so UI and IMD analytics can be developed without hardware. Recorded
sessions replay through the same parser.

**Views:**
1. **Connection bar:** versions, target, profile, link statistics, start/stop.
2. **Channel grid:** per-channel enable toggle, RSSI bar, SNR, noise floor, centre
   offset, OBW, flags, 10 s sparkline, update rate (Hz). Also shows the current LO
   plan.
3. **Band panorama:** stitched spectrum with channel overlays, live trace,
   max-hold, average, persistence, and segment-age shading.
4. **Waterfall.**
5. **Channel zoom:** markers for nominal centre, peak, centroid, OBW edges, noise
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
   into `radio/s3/`, behind `radio.h`. Add `RAWIQ`. `tools/rx5808cli.py` plots
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
4. `testgen` env: tone transmitter at the S-channel centres (verify the cert-test
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
  - Profiles carry a `target`. The firmware boots into the first profile for its chip, and `PROFILE` rejects the other chip's (`ERR profile_target`). The `c5-race` values match the web simulator.
  - `radio_caps_t` has 6 rate slots and `lo_step_khz`. `RAWIQ` and `FFTEVAL LIVE` reject LOs that are not a multiple of it.
  - The link and command tasks were pinned to core 1; they now use the last core (core 0 on the single-core C5).
- FFT: the S3's S16R kernel is Xtensa assembly. Other targets get a C port of esp-sdr's rounded Q15 radix-2 kernel (`spectrum_fft.h`), checked on the host with a test tone. That arithmetic is identical to esp-dsp's ANSI `dsps_fft2r_sc16`, which is what S16 maps to on the C5. So on the C5, S16 and S16R should give the same result; the truncation bias only exists in the S3 SIMD kernel. The C5 has no FPU, so R2/R4 run in software floating point.
- `tools/flash.sh` takes `RX_ENV=esp32c5` and writes the C5 bootloader at 0x2000.

Bring-up on hardware (XIAO ESP32-C5, 2026-10-08):
- **Boot loop, fixed.** `esp_wifi_init` failed with `ESP_ERR_NO_MEM` and `app_main` aborted before `READY`, so the web UI never connected. The C5 heap is only ~125 KB once the capture bank is reserved, and the S3-sized link buffers (72 KiB queue + 16 KiB USB TX) took most of it. The C5 now uses a 36 KiB queue and 4 KiB of USB TX, and `PSD_MAX_FFT` is 1024 (`FFT` rejects 2048). Free heap while scanning is about 11 KB, so watch it when adding anything.
- Boot steps after `link_init` use `BOOT_CHECK`: on failure the board prints `ERR boot <step> <error> heap=… largest=…` every second and keeps draining USB RX, instead of reboot-looping silently.
- `RAWIQ` is capped by the link queue: `STATUS` reports `rawiq_max` (9180 on the C5, 16380 on the S3), the command refuses more (`ERR rawiq_args`), and the UI disables larger sizes. RAWIQ frames wait up to 500 ms for queue space (`link_frame_wait`); before that, an 8192-sample capture sometimes failed with `ERR link_full`.
- **USB reset wedge on rev v1.0 silicon, fixed.** The ROM loops forever after a USB Serial/JTAG (RTS) reset if UART0's clock is gated (esp-idf#18089, IDFGH-17050). IDF 5.5.5 avoids the gating only when USB Serial/JTAG is the *console*. Ours is off (the binary protocol owns the port), so `app_main` turns the UART0 clocks back on. Before the fix, esptool auto-reset failed (`Write timeout`), and closing the port (the UI or the bridge disconnecting) left the board dead until RESET was pressed. Now `pio run -t upload` / `tools/flash.sh` work hands-free, and open/close cycles are fine.
- Capture: all six rate codes complete. A 48 MHz-crystal harmonic at 5856 MHz shows up at LO+19 MHz (80/40 MS/s) and folds to −1/−1/+3/−1 MHz at 20/10/8/4 MS/s, as expected. The lower rates are plain decimation with no anti-alias filter. That spur's sign confirms `CONJ 1` for the C5 too. Other narrow spurs seen while scanning: 5664 and 5760 MHz (also 48 MHz harmonics), plus lines near 5840 and 5920 MHz. They aren't in `spurs_khz` yet. 5 GHz Wi-Fi is visible around 5860–5915 MHz.
- Hop timing, all 8 channels in pair mode: **15.3 ms/hop** (15 rotations/s) against the 2.5 ms target.
  - Tune: 1.23 ms, almost all `phy_set_chanfreq`. `TUNESKIP 2` saves only ~40 µs. `TUNESKIP 4` raises the floor by 21 dB: as on the S3, channel setup resets the gain, so the re-apply stays.
  - Capture: 1.88 ms. About 1.78 ms of that is fixed overhead inside `adctrig` (1024 samples 1.80 ms, 8192 samples 1.99 ms, filter on or off). Its 10 ms delay is on the TX-tone path, not ours, so the cost is elsewhere in the cert-test routine. Driving the capture registers directly, as the S3 backend does, is the likely fix.
  - DSP: 12.2 ms. `TIMING`: PSD 8.7 ms (prep 1.1, FFT 3.1, power 3.4), metrics 3.0 ms. The C5 has no FPU, and window/power/metrics are float.

Next steps, in order:
1. **DSP cost.** The biggest item (12 of 15 ms). Move window, power and metrics to fixed point, or at least drop the float work done per bin. `FFTEVAL SYNTH`/`LIVE` for all FFT types: S16 and S16R should match on the C5, and R4 shows the soft-float cost. Step-on/IMD relies on R4, so it needs an affordable path.
2. **Capture overhead.** Find where `adctrig`'s 1.78 ms goes. Either replace it with direct register writes, or keep only the parts that are needed per capture.
3. **Calibration.** Compare `RECALMODE ONCE` (calibrate at 5787 MHz only) with `EVERY` across R1–R8: DC spike size, noise floor and image level. Decide the default. If `EVERY` is needed and slow, this is where the per-LO calibration cache (§4.4) comes back.
4. **RF characterisation** (Phase 0-C5 step 2): board spurs by LO sweep (add them to `spurs_khz`), the real width of filter code 0, alias rejection (R3 seen from LO 5677 at −24 MHz), and whether R8 (5917 MHz, above esp-sdr's validated 5150–5895 MHz) is usable.
5. **Profile.** Finalise `c5-race` (pair vs. single mode, filter code, windows), then run the Phase 1 acceptance criteria with real VTXs.
6. `BOOTLOADER` is still untested on the C5.
7. The S3 firmware picked up the shared changes (`BOOT_CHECK`, `rawiq_max`, `link_frame_wait`). It builds but hasn't been reflashed or rerun on the S3.

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

**Open questions (defaults assumed):**
1. S3 board: ESP32-S3 Micro (native USB only, BOOT + EN buttons). C5: ESP32-C5-DevKitC-1 assumed.
2. ~~testgen~~: deferred by request; the S3 work uses ambient 2.4 GHz traffic.
3. Raceband only for v1? Assumed yes, with a generic channel table.
4. Lap and IMD analytics in the browser first, firmware later? Assumed yes.
