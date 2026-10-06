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
| `tools/check_phy_abi.sh` | Compares the private PHY functions we call between two ESP-IDF versions. |
| `ref/esp-sdr/` | Reference checkout of esp-sdr (not tracked here). Restore it with `git clone https://github.com/ESPARGOS/esp-sdr ref/esp-sdr`. |
| `backup/` | Full flash dump of the S3 as it arrived (esp-sdr WebSDR firmware). Local only, not tracked. |

## Firmware

```sh
cd firmware
pio run -e esp32s3 -t upload      # no buttons needed: esptool resets via USB (usb-reset)
```

Use the PlatformIO Core that pioarduino 55.03.312-1 expects (6.2.x). The
platform pins ESP-IDF 5.5.5. Before changing the platform version, run
`tools/check_phy_abi.sh esp32s3 25fe69f946311abdaf9ad56591f25fedbc20ac98 <new-idf-tag>`.
The firmware depends on private PHY functions, and they must stay identical.

Ways back into the bootloader, from least to most manual: esptool's `usb-reset`
(automatic on upload), the `BOOTLOADER` command (`tools/rx5808cli.py bootloader`
or the button in the UI), or hold BOOT while pressing EN/RESET.

To restore the original WebSDR firmware:
`esptool --chip esp32s3 -p /dev/ttyACM0 write-flash 0 backup/websdr-s3-flash-4MB.bin`.

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
