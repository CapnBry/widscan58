# Widscan58 display: developer notes

Technical reference for `display/`: a stand-alone display for a widscan58 on
an ESP32 "Cheap Yellow Display" (ESP32-2432S028R). The user-facing
description is in [README.md](README.md); the widscan58 itself (firmware,
protocol, web UI) is in [../CLAUDE.md](../CLAUDE.md).

| Path | What |
|---|---|
| `src/` | The firmware: screens (`ui*.c`), host protocol (`host.c`, `link.c`), panel and touch (`lcd.c`, `touch.c`, `lvport.c`), simulator (`sim.c`), RemoteID receiver (`rid*.c`, `ui_rid.c`). |
| `RAM.md` | RAM map: static RAM by object, the heap by startup step and with the RemoteID radios up (measured 2026-11-12). |
| `test/` | Host tests (`pio test -e native`): RemoteID parsing, decoding, the aircraft list and the radio time slices. |
| `tools/cydshot.py` | Screenshots and injected touch over the CYD's USB port (see below). |
| `tools/gen_voice.py` | Makes the lap callout clips with a TTS model on OpenRouter (`OPENROUTER_API_KEY`); see "Lap callouts". |
| `../tools/displayflash.py` | Builds and flashes the C5 through the display's USB port (`src/passthru.c`); see "Flashing the C5 through the display". |
| `voice/` | The callout clips (`voice.bin`, tracked) and the paid API's cached replies (`cache/`, not tracked). |

`display/` is its own PlatformIO + ESP-IDF 5.5.5 project (the same pioarduino
platform as `firmware/`) for an ESP32-2432S028R: 320x240 in landscape,
XPT2046 touch. It drives the widscan58 over the C5's host UART as
the web UI does over USB (`src/host.c`: `INFO` until it answers, then the
mode's setup: `PROTOCOL MONITOR|RH`, the stored `CHANNELS`, `MODE`, `GAIN`,
and power meter mode's scan from `../web/src/model/power.ts`). The only shared
file is `../firmware/src/proto/frames.h`, on the include path, not copied;
`src/power.c` repeats the web UI's `CALIBRATION` (`../web/src/model/power.ts`; keep them in step).

```sh
cd display
pio run -e cyd -t upload            # the real thing (UART to a C5)
pio run -e cyd-sim -t upload        # built-in simulated widscan58 (src/sim.c), no C5 needed
pio run -e cyd-lcdtest -t upload    # panel bring-up: write speed and readback per SPI clock
tools/cydshot.py shot out.png       # screenshot over USB; also swipe l|r|u|d, tap X Y, drag X0 Y0 X1 Y1, bench, peek X Y, reopen, log [s], fakelaps, fakerid N
pio test -e native                  # host tests (test/test_rid)
```

- **Wiring** (CYD connector CN1: GND, GPIO22, GPIO27, 3.3V): CYD GPIO22 (TX)
  to the C5's RX (GPIO12, D7 on the XIAO), CYD GPIO27 (RX, pull-up on) to the
  C5's TX (GPIO11, D6), GND to GND, 921600 baud. GPIO21 is the backlight and
  GPIO1/3 are the CH340's, so CN1 is what is left. The CYD's 3.3V pin can
  power a XIAO through its 3V3 pin only while the XIAO's USB is unplugged
  (two regulators in parallel otherwise). Pins: `LINK_TX_GPIO`/`LINK_RX_GPIO`
  (`src/link.h`).
- **UI:** swipe left/right cycles monitor, lap timer, power meter,
  RemoteID (only while its receiver is on) and Global (a mode overlay
  shows 0.7 s; swipes work while it shows), swipe down opens the
  mode's settings (gain for monitor and lap timer, attenuator for power
  meter, Edit channels: the web UI's band x channel grid plus 2.4 GHz;
  the gain also has -/+ step buttons, as exact drags are hard on this screen),
  swipe up or Back closes them. Under the gain, the plan's channels as
  buttons: on/off (`ENABLE`) in monitor and power meter mode, the seat's
  EnterAt/ExitAt screen in lap timer mode (level codes only, 0-255 as the
  C5 and RotorHazard use them; −/+ one code, Now: the last second's
  average; live level, peak, passes and a level graph with the thresholds;
  also opened by tapping the pilot's channel on the lap timer screen, and
  then Back returns there). Swipes are detected in `src/lvport.c`, not by
  LVGL (whose gesture restarts on any read that moves under its minimum
  speed and ends at a release, so most swipes on this resistive panel
  failed): 40 px along one axis and twice the other, from any point of the
  last 600 ms; a press survives 3 reads with no or disagreeing touch
  readings (`touch.c`, `TOUCH_BAD`); one swipe per press, sent to the
  active screen as `LV_EVENT_GESTURE` (handlers read `lvport_gesture_dir()`),
  not when the press is on a slider or scrolling. Monitor mode's panorama
  also draws the web UI's average line (blue, under the trace): per column, 0.05 per
  spectrum in linear power. Settings are in NVS (`src/settings.c`: blob `set`, a
  version byte (0) and `settings_t`; any other version or size is ignored,
  the defaults instead, so a change to `settings_t` bumps the version;
  thresholds `lapthr`,
  `src/lap.c`); the lap timer gain is the device's own (`GAIN MANUAL`,
  `CONFIG SAVE`). Lap timer mode has no seats on 2.4 GHz and says so.
- **Global screen** (`src/ui_global.c`; "Global settings" in its title
  and the overlay): in the swipe cycle between power meter and monitor;
  it leaves the widscan58 in the mode it was in. The display boots into
  the screen last shown: Global, RemoteID (while its receiver is on) or a
  mode (`g_set.screen`: `SCREEN_GLOBAL`, `SCREEN_RID` or
  `SCREEN_OF_MODE`).
  The widscan58 is set up for `g_set.mode` whichever it is. Backlight 5-100 %
  (LEDC PWM on GPIO21, `lcd_brightness`; `g_set.backlight`) and GPIO config: the web UI's GPIO dialog, 5 pins a page, on a
  copy of `GPIO?` (`M.gpio`). OK sends `GPIO <n> …` for each changed pin, then
  `GPIO SAVE` and `GPIO?` (`host_request_gpio`); Cancel and Back drop the
  copy; Revert and Defaults only change the copy (Defaults: no outputs, no
  LEDs). TX and RX are shown, not offered: they carry the display's link,
  and Defaults never sends `GPIO RESET`, which would move them. The
  simulator answers `GPIO` as a C5. Below it, the RemoteID receiver
  checkbox (`g_set.rid_on`).
- **RemoteID receiver** (`src/rid.c`, `src/rid_sched.c`, `src/rid_radio.c`,
  `src/ui_rid.c`; `g_set.rid_on`, `rid_ble_pct`, `rid_lock`): ASTM F3411 / EN 4709-002 broadcasts from the CYD's own
  ESP32, independent of the widscan58. Wi-Fi beacons and NAN action frames
  on channel 6 (promiscuous, management frames) and Bluetooth 4 legacy
  advertisements (service data 0xFFFA; ESP32 has no BT5 long range), one
  radio at a time. Bluetooth goes straight to the controller over VHCI (HCI
  reset, event mask with LE meta, passive scan 50 ms / 50 ms, scan enable
  per slice, no duplicate filter): no host stack. Off, both are stopped and
  deinitialized (`esp_wifi_deinit`, `esp_bt_controller_deinit`); Classic
  Bluetooth's controller memory goes back to the heap at boot
  (`esp_bt_controller_mem_release`). The callbacks parse and queue
  (`rid_raw_t`); `rid_poll()` in every UI pass (whatever screen shows)
  feeds the list, `RID_MAX` 5 aircraft (what the screen shows), by MAC, the
  one heard longest ago replaced. Time slices (`rid_sched.c`, host-tested):
  a 1 s cycle (`RID_PERIOD_MS`) split by the settings' slider (Wi-Fi left,
  Bluetooth right, 10 % steps, 50/50 default; the end icons step it), and
  Lock: None, Double (a radio that hears RemoteID gets twice its slice
  until a whole cycle, at the doubled length, passes with nothing), Full
  (it listens all the time until 1 s passes with nothing, then doubled for
  a cycle, then the split; anything heard goes back to full). Top left of
  the list, on a block as wide as the lap timer's channel chip: the radio
  listening now. The block is grey while scanning, dark blue with
  Bluetooth RemoteID heard in the last 10 s, dark green with Wi-Fi's (both:
  the radio listening now), dark red when the radios did not start
  (retried every 5 s). Beside it a yellow padlock while a radio is boosted:
  II doubled, X full (drawn, `lock_create`: the symbol font has no lock).
  The overlay calls it "RemoteID receiver", the title "RemoteID". Rows: the operator ID, else the CAA registration, else another
  UAS ID, else the MAC; under it the serial number; RSSI, height (else
  GNSS altitude), speed, age; grey after 10 s. A tap opens every field of
  every message type (`lv_table`, Back only: vertical drags scroll it),
  with RSSI, broadcasts and rate, seen duration, the ranges of RSSI, GNSS
  altitude, height and speed seen, and the distance to the operator.
  Settings (swipe down): the split, Lock, Clear list, frames heard per
  transport. `cydshot.py fakerid N` injects a frame from each of N made-up
  aircraft (`rid_fake`, the tests use it too), counted as heard by its
  radio, so it drives the locks and the block's colour too. Where all of
  the RAM goes: [RAM.md](RAM.md). Memory: the radios take ~35 KB
  (Wi-Fi with its event loop ~21 KB, the BT controller ~14 KB), more than
  the ~30 KB left after boot, so while the receiver is on LVGL's draw
  buffers are 20 rows (`LV_BUF_ROWS_LOW`, one block for both) instead of
  40: ~29.5 KB free with both radios up (min ~28.5 KB). The Kconfig cuts
  (`sdkconfig.defaults`, 2026-11-12) gave that from ~18 KB: 2 static Wi-Fi
  RX buffers and 2 management ones, an 8-entry event queue, and the BT
  controller without HLI (its level 4 interrupt stack, 4.4 KB of `.data`),
  channel assessment, LE ping or modem sleep. BLE adverts and Wi-Fi
  management frames came in at the same rates before and after (~19/s and
  ~4/s at the 50/50 split). The Wi-Fi feature switches (enterprise, GMAC,
  ESP-NOW keys, Wi-Fi NVS, disconnected power save, management short
  buffers) saved only ~2 KB of flash and nothing measurable in RAM: the
  bulk is the prebuilt net80211, pp and PHY libraries, and
  `esp_wifi_init` calls `esp_supplicant_init` whatever the mode, so the
  supplicant and its crypto (~55 KB of flash) stay. Turned off, they grow
  back only if a 51 KB block is free, which after the radios ran it is not
  (~19 KB largest): then at the next boot. The radios start from the UI
  task after that shrink, never at boot before the UI has its memory (they
  took it all once). IRAM: the BT controller did not fit with Wi-Fi's IRAM
  options on, so those are off, and the ring buffers are in flash
  (`sdkconfig.defaults`). Checked on hardware (2026-11-12): a real BT4
  transmitter (78 frames in 22 s, all message types it sends), the Full
  lock holding Bluetooth, Wi-Fi slices receiving channel 6's management
  frames, on/off cycles, monitor mode unchanged (55 fps, ~600 link
  frames/s) and lap timer polling unchanged with the radios running. No
  Wi-Fi RemoteID transmitter was at hand: beacon and NAN parsing are
  host-tested only.
- **Lap timer mode** (`src/host.c`, `src/lap.c`, `src/ui_lap.c`): the display
  is the RotorHazard server: `PROTOCOL RH`, then the link goes raw
  (`link_set_raw`: RotorHazard's replies have no framing; our text commands
  still work), the seats get the plan's frequencies in order and the
  thresholds, and every 50 ms (`LAP_POLL_MS`) each seat is selected and read
  with `READ_LAP_STATS` (0x05: pass stats and extremums in one reply).
  Laps as the web UI's `laps.ts`: minimum lap and fastest consecutive
  laps from the settings' Lap settings screen (0-99 s, default 5; 2-9,
  default 3; `g_set`). A new
  run length finds the fastest run again in the kept 64 laps (breaks are
  flagged in `laps_ms`, `LAP_BREAK`); a new minimum applies from the next
  pass. A run of more than 3 shows its average beside the total. The
  first counted pass picks the pilot's seat; the main screen shows its last
  lap big, the running lap, the fastest 3 and the laps before, over a graph
  of every seat's level; Clear drops the laps and the pick. Tapping the lap
  grid opens every lap since the clear (the newest 50, 5 x 10, the fastest
  run in green). Laps of a minute or more read 1:02.35 (no "s" after the big
  time) and switch the Best 3 laps and the lap list to a smaller font; 10
  minutes or more may overflow. `cydshot.py fakelaps SEAT N [LO_MS HI_MS]`
  adds sample laps for screenshots. A device
  restart (`READ_TIME_MILLIS` going back) sets the seats up again. The
  simulator answers RotorHazard too: a pilot on R4 every 12-15 s. Over the
  C5's UART a poll of 8 seats took 180 ms (~20 ms a request) until the
  firmware's UART read returned at once (../CLAUDE.md, Host UART); the display's
  own UART task had the same fault. The host task takes UI requests as a
  mask and its own semaphore, not task notifications: the raw stream
  buffer blocks the same task on its notification, took a request's
  wake-up and left the bits, which every timed-out wait then returned (power
  meter mode set up again 3 times a second after lap timer mode).
  LVGL 8.3 (component manager); screens must not have
  `LV_OBJ_FLAG_GESTURE_BUBBLE`, or LVGL sends no gesture at all.
- **Lap callouts** (`src/audio.c`): each lap of the pilot's seat is said
  to 0.1 s ("twenty three point nine", up to 999.9 s) through the speaker
  connector: GPIO26 is DAC channel 2, into the board's amplifier;
  `dac_continuous` (I2S0's DMA) at 16 kHz from the APLL (the default clock
  goes no lower than 19.6 kHz). A new lap replaces one still waiting. The
  words are 30 clips (zero to nineteen, twenty to ninety, hundred, point;
  `voice/voice.bin`, 276 KB of 8-bit PCM, `EMBED_FILES`, tracked, so a
  build needs no API key; table in `src/voice_clips.h`), made by
  `tools/gen_voice.py` with Gemini TTS on OpenRouter (voice Leda, each
  clip trimmed to the speech at −32 dB, no padding, no gap between
  words). `--instructions` (delivery text, cached separately) and
  `--only` (fetch a few words, leave `voice.bin`) are for trials; a
  "quick, crisp announcer" style sounded worse than none (2026-11-11). Every API call costs money: the raw replies are
  kept in `voice/cache/` (not tracked), and reruns only fetch missing clips,
  so trim and level changes cost nothing. Don't delete the cache. Volume
  in Lap settings: Off, 5, 10, then steps of 10 to 100 % (default 50);
  Test says 23.9.
  Every DAC write is a whole 256 samples, carried across words: a clip's
  odd-sized last chunk stopped the DMA after the first word. Between
  callouts the DAC holds 0 V, ramped to mid-scale and back over 50 ms each
  side: held at mid-scale the PCB's digital noise was loud in the speaker,
  at 0 V it is quieter (not silent), with the channel off no quieter. Heard on hardware
  (2026-11-13). The amplifier (SC8002B) came with a gain of ~29 (Rf 68 kΩ
  across pins 4-5, Ri the 4.7 kΩ from GPIO26, then a capacitor to GND and
  the coupling capacitor to pin 4): 50 % overdrove the speaker. With Rf
  changed to 6.8 kΩ (gain ~2.9) the old 10 % is about 100 %. The capacitor
  to GND is no low-pass: the coupling capacitor goes to pin 4's virtual
  ground, so the two split the current in a fixed ratio. The DAC has no
  gain of its own.
- **Display speed:** SPI2 on its IOMUX pins, DMA, two 320x40 LVGL buffers
  (320x20 while the RemoteID receiver is on: monitor full redraw 25.8 ms
  instead of 21.8, live updates unchanged).
  The panel setup runs at 40 MHz (TFT_eSPI's rate here), then the bus at
  80 MHz (`LCD_PCLK_HZ`, `src/lvport.c`; `-DLCD_PCLK_HZ=40000000` for 40).
  Full-screen redraws (`cydshot.py bench`), 2026-11-11: lap timer
  placeholder 16.1 ms (62 fps) at 80 MHz, 31.4 ms at 40; power meter 21.7 /
  32.1 ms; monitor 24.9 / 33.9 ms (below 40 MHz's ~31 ms, the CPU's drawing
  is the limit, not the bus). Pixel data reads back right at 80 MHz
  (`cyd-lcdtest` and `cydshot.py peek X Y`: G and B; R reads back wrong at
  every clock, a read quirk). The panorama and waterfall (`src/pano.c`) write straight into
  LVGL's draw buffer and invalidate only the columns whose trace moved,
  merged into rectangles by a pixel cost per rectangle (`MERGE_PX`). With a
  C5 on the UART (R1-R8, 4 LOs, 2026-11-11; the simulator measures the
  same): every spectrum shown (~49 updates/s), ~1.5 rectangles and ~3700 px
  each, ~850 KB/s to the panel (mostly the 320x50 waterfall, ~12 rows/s),
  ~2.4 ms of refresh per 8 ms loop; the link carries ~300 frames/s, 37 KB/s,
  with no warnings or reconnects in a 15 s soak. LVGL
  redraws an object on every style or flag call, changed or not: set those
  only when the value differs (`power_update`), and set an object's fixed
  look when it shows, not on the first update: hidden cards skipped once
  kept LVGL's default white stripe and no name (it looked like an 80 MHz
  fault). The console (921600 baud) prints these figures every 2 s.
- **Panel setup** (`src/lcd.c`, `panel_init`): byte for byte what TFT_eSPI
  sends with the ASCII-Aquarium sketch's flags (ILI9341_2_DRIVER,
  TFT_INVERSION_ON, `setRotation(1)`), known to work on this board. Its ID
  registers read like an ST7789 (RDID3 0x52), but ST7789 sequences and
  hand-picked MADCTL/inversion values gave mirrored, inverted or shifted
  pictures: copy TFT_eSPI rather than reason from the IDs. On an ST7789
  some ILI9341 commands are analog settings (0xC5: VCOM offset, 0xE0/0xE1:
  gamma in another format; 0xC0 0x10 is LCMCTRL's inversion flip, why INVON
  shows normal colours), and the blacks came out medium grey: after the
  TFT_eSPI sequence come the ST7789 analog and gamma values from TFT_eSPI's
  ST7789_Init.h, nothing that changes orientation or colours. Touch is the
  XPT2046 library's rotation-1 mapping, as in that sketch.
- **Flashing the C5 through the display** (`src/passthru.c`,
  `../tools/displayflash.py`, run with `~/.platformio/penv/bin/python`): the console's `passthru` sets a flag in RTC
  no-init memory and restarts the CYD (the CH340 stays enumerated, so the
  PC's port stays open) into a bridge: it sends the C5 `PROTOCOL MONITOR`
  and `BOOTLOADER`, checks the ROM's banner for `DOWNLOAD` at 115200, says
  `passthru: ready download|no-banner` at 921600, then copies bytes between
  UART0 and the link with the log off. It watches esptool's SLIP for
  `CHANGE_BAUDRATE` (0x0F) and moves both UARTs to the new rate once the
  chip's reply has gone through. It restarts into the normal display after
  8 s without traffic (esptool's `--after watchdog-reset` has started the
  new firmware), 60 s if esptool never starts, or on a tap.
  `displayflash.py` keeps one port open from `passthru` to the end, since
  opening the port resets the CYD, and hands it to esptool's Python API
  (`esptool.main(..., esp=...)`). It needs our firmware running on the C5:
  CN1 has no BOOT or EN line, so a C5 left unbootable by a failed flash
  needs its own USB port. The 691 KB image, 2026-11-11: 6.2 s at 921600 (the
  default), 4.7 s at 1.5 Mbaud, 4.1 s at 2 Mbaud (`--baud`), all verified.
- `cydshot.py` opening the port resets the board; it waits for the
  firmware's "main: up" before its steps (separated by `--`).
