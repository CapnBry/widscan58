# Widscan58 display: developer notes

Technical reference for `display/`: a stand-alone display for a widscan58 on
an ESP32 "Cheap Yellow Display" (ESP32-2432S028R) or, in progress, an
LCDwiki 3.5" ESP32-S3 display (ES3C35P). The user-facing
description is in [README.md](README.md); the widscan58 itself (firmware,
protocol, web UI) is in [../CLAUDE.md](../CLAUDE.md).

| Path | What |
|---|---|
| `src/` | The firmware: screens (`ui*.c`), host protocol (`host.c`, `link.c`), boards (`board*.h`), panel and touch drivers (`lcd_*.c`, `touch_*.c`, `backlight.c`, `lvport.c`), simulator (`sim.c`), RemoteID receiver (`rid*.c`, `ui_rid.c`). |
| `test/` | Host tests (`pio test -e native`): RemoteID parsing, decoding, the aircraft list and the radio time slices. |
| `tools/cydshot.py` | Screenshots and injected touch over the CYD's USB port (see below). |
| `tools/gen_voice.py` | Makes the lap callout clips with a TTS model on OpenRouter (`OPENROUTER_API_KEY`); see "Lap callouts". |
| `../tools/displayflash.py` | Builds and flashes the C5 through the display's USB port (`src/passthru.c`); see "Flashing the C5 through the display". |
| `voice/` | The callout clips (`voice.bin`, tracked) and the paid API's cached replies (`cache/`, not tracked). |

`display/` is its own PlatformIO + ESP-IDF 5.5.5 project (the same pioarduino
platform as `firmware/`) for an ESP32-2432S028R (`cyd`: 320x240 in landscape,
XPT2046 touch) and an ES3C35P (`cyds3`: 480x320, see "Boards"). It drives the widscan58 over the C5's host UART as
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
pio run -e cyds3-sim -t upload      # ES3C35P with the simulator (cyds3: the UART, untested)
tools/cydshot.py shot out.png       # screenshot over USB; also swipe l|r|u|d, tap X Y, drag X0 Y0 X1 Y1, bench, peek X Y, reopen, log [s], fakelaps, fakerid N
tools/cydshot.py --port /dev/ttyACM0 shot out.png   # the same on the ES3C35P (its USB Serial/JTAG)
pio test -e native                  # host tests (test/test_rid)
```

- **Boards** (`src/board.h`): each board is a header, `board_<name>.h`,
  picked by its `-DBOARD_<NAME>` in `platformio.ini` (one section per board,
  `[cyd]`, `[cyds3]`, with its envs). It sets the screen size (`LCD_W`,
  `LCD_H`, landscape), which panel and touch driver to build
  (`LCD_DRIVER_<name>` → `lcd_<name>.c`, `TOUCH_DRIVER_<name>` →
  `touch_<name>.c`; every driver file is compiled, empty unless picked), the
  pins, the draw window alignment (`LCD_ALIGN`, a rounder in `lvport.c`),
  LVGL's buffer rows, the link's UART and pins, where the debug console is
  (`CONSOLE_USB_JTAG`), and the features it has (`BOARD_HAS_AUDIO`, `_RID`,
  `_PASSTHRU`, `_LCD_TEST`; without one, `audio.c`, `rid_radio.c` and
  `passthru.c` build as stubs, the Global screen has no RemoteID checkbox and
  `rid_on` loads as 0). `board.h` lists every define. The interfaces are
  `lcd.h` (`lcd_init`, `lcd_draw`, `lcd_reopen`, `lcd_peek`; the backlight in
  `backlight.c`, LEDC on `BL_GPIO`) and `touch.h` (`touch_read` in screen
  coordinates). Components: `src/CMakeLists.txt` adds each board's by
  `IDF_TARGET` (requirements are resolved before Kconfig is read, so they
  can't follow `CONFIG_` values); Kconfig in `sdkconfig.defaults` (every
  board) and `sdkconfig.defaults.<target>` (ESP-IDF loads both; the CYD's
  generated sdkconfig came out identical after the split, 2026-11-14), the
  component lock per target (`dependencies.lock.<target>`). A new board:
  a header, a section in `platformio.ini`, a driver file for a new
  controller, its target's components and defaults. Screens use `LCD_W`
  and `LCD_H`: monitor mode (`pano.c`, `WF_H` a fraction of `LCD_H`, tag
  sizes and fonts by `LCD_H`) fills the screen; the other screens still have
  the CYD's 320x240 geometry and sit at the top left of a larger screen.
- **ES3C35P** (`cyds3`, `board_cyds3.h`;
  https://www.lcdwiki.com/3.5inch_ESP32-S3_Display): ESP32-S3 N16R8 (16 MB
  flash, 8 MB octal PSRAM, off for now), native USB Serial/JTAG (the console
  and `cydshot.py`, `--port /dev/ttyACM0`), 320x480 IPS ST77922 on QSPI
  (`lcd_st77922.c`: CS 10, CLK 12, D0-D3 11/13/14/9; instruction 0x02 then
  00 <cmd> 00 for commands, 0x32 for pixels on four lines, at 40 MHz, the
  fastest 80 MHz / n under the controller's 62.5 MHz). Landscape is
  software: the panel stays in its native portrait (MADCTL 0x00; USB at the
  bottom, column 0 left, row 0 top) and `lcd_draw` transposes each area into
  one of two DMA buffers (`LCD_ROTATION` 90: USB on the right; 270: on the
  left), handing LVGL its buffer back at once. MADCTL 0xA0 (MV | MY,
  reported to give landscape on this board) flipped the rows but did not
  exchange the axes here (2026-11-14): the ST77922's MADCTL has no MV bit
  (datasheet v0.1, p. 211; the frame RAM is compressed row by row), so
  landscape is always a software turn. Turning through two 6 KB chunks
  (RAMWR, then Write Memory Continue 3Ch) saved ~33 KB of heap but made a
  full redraw 59 ms (93 ms with the sending on its own task); the
  full-area buffers stay, for speed. The vendor init sequence
  is the factory firmware's (decoded by github.com/jlmeredith/ES3C35P,
  docs/HARDWARE.md, which also has the board's pin and touch findings).
  Windows on 4 px (dual gate). The panel's reset is the chip's EN: a USB
  reset leaves the panel as it was, so after flashing over other firmware
  unplug the board once. Backlight GPIO41 (25 kHz PWM). Touch: the panel's
  own controller on I2C (SDA 38, SCL 39, reset 48, address 0x55, the
  ST7123's registers; `touch_st7123.c`), points in portrait, turned to
  landscape there. Link: UART1 on the 4-pin serial socket's GPIO43 (TX) and
  44 (RX); `cyds3` builds but is untested with a C5. Not ported yet: the lap
  callouts (ES8311 codec over I2S, the amplifier's enable on GPIO1, low
  on), the RemoteID receiver (the S3's Bluetooth is not the ESP32's
  controller), passthru flashing, `lcd_peek` (reads need instruction 0x0B
  with a dummy phase). Measured 2026-11-14 with the simulator: full-screen
  redraw 40.2 ms, sd 0.5 (24.7 fps; 3 boots x 10 `bench` runs, the same
  pattern every boot); a build differing only in the touch code's size ran
  39.3 ms, the same read with the new code 40.25: code placement (flash
  cache), not the work done, monitor mode every spectrum (50 updates/s,
  ~1.8-2.9 ms of refresh per 8 ms loop), ~72 KB of heap free after boot
  (LVGL's 2 x 32 rows and the rotation buffers: 4 x 30 KB).
- **Wiring** (CYD connector CN1: GND, GPIO22, GPIO27, 3.3V): CYD GPIO22 (TX)
  to the C5's RX (GPIO12, D7 on the XIAO), CYD GPIO27 (RX, pull-up on) to the
  C5's TX (GPIO11, D6), GND to GND, 921600 baud. GPIO21 is the backlight and
  GPIO1/3 are the CH340's, so CN1 is what is left. The CYD's 3.3V pin can
  power a XIAO through its 3V3 pin only while the XIAO's USB is unplugged
  (two regulators in parallel otherwise). Pins: `LINK_TX_GPIO`/`LINK_RX_GPIO`
  (`src/board_cyd.h`).
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
  readings (`touch_xpt2046.c`, `TOUCH_BAD`); one swipe per press, sent to the
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
  The widscan58 is set up for `g_set.mode` whichever it is, but on
  Global and RemoteID (and the screens they open) it idles
  (`host_set_idle`, from `show` in `ui.c`; a mode's own screens, settings
  and channel editor included, keep it scanning): `SCAN STOP`, so the C5
  powers its radio down (../CLAUDE.md, "Idle"). In monitor protocol its
  telemetry (4/s) still shows the link is up; in lap timer mode the
  polls stop (they would restart the scan) and `INFO` every 1 s
  (`IDLE_PING_MS`) does that. Showing a mode runs its setup again, which
  ends with `SCAN START` (the C5 keeps a stopped scan across protocol
  switches; lap timer mode's next poll restarts its own). Swiping always
  passes monitor or power meter mode on the way to Global or RemoteID, so
  lap timer mode idles only if it was set up for there (checked once with
  a build booting so, 2026-11-14). The simulator stops too and sends
  telemetry only. Backlight 5-100 %
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
  radio, so it drives the locks and the block's colour too. Memory:
  the radios take ~35 KB
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
  of every seat's level; Clear drops the laps and the pick. Until a pass
  picks a seat ("Fly through the gate"), the channel chip shows the seat
  with the highest level, its level and crossing dot (`lead_seat`,
  `ui_lap.c`), so a tap opens the thresholds of a pilot whose level stays
  below EnterAt; that is not a pick. A change holds at least 750 ms
  (`LEAD_HOLD_MS`), unless the seat shown loses its level. Tapping the lap
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
  **Open: crackle on one word per callout**, a different word each time
  (Test: sometimes "point", sometimes "nine" or "three"). Tried on
  2026-11-14, none made a difference: 128 ms of DMA buffering instead of
  32 ms (the driver splits each 256-sample write into two 128-sample
  descriptors, so `desc_num 4` held 32 ms) and the audio task at priority 7
  instead of 3; a sound-only firmware (only `settings_load` and the audio,
  23.9 every 3 s: no panel, backlight, touch, link or radios); and the
  driver's async mode (a ring of DMA buffers refilled from `on_convert_done`,
  never stopping, no audio task). The clips are not clipped (none reach 0 or
  255). The blocking write's DMA stops once per callout, always 8 ms in
  (after the first 128 samples, with 2048 queued) and restarted by the next
  write: during the ramp-up, before the first word, and async mode, without
  it, crackled the same. Untested: the analog side (the DAC's 8-bit steps
  at a low level, the amplifier), e.g. a steady tone from the DAC's cosine
  generator (no DMA).
- **Display speed:** SPI2 on its IOMUX pins, DMA, two 320x40 LVGL buffers
  (320x20 while the RemoteID receiver is on: monitor full redraw 25.8 ms
  instead of 21.8, live updates unchanged).
  The panel setup runs at 40 MHz (TFT_eSPI's rate here), then the bus at
  80 MHz (`LCD_PCLK_HZ`, `src/board_cyd.h`; `-DLCD_PCLK_HZ=40000000` for 40).
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
- **Panel setup** (CYD: `src/lcd_ili9341.c`, `panel_init`): byte for byte what TFT_eSPI
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
