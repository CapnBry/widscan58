# The command line tool (widscan58cli.py)

`tools/widscan58cli.py` is a small Python script that talks to the board over
USB without the web page. It shows what firmware and settings the board has,
sends it commands, prints the channel readings live, and saves raw radio
samples or a spectrum to look at later.

For watching channels, the web page is nicer. The script is for getting data
into files, scripting, and machines with no web browser.

## Getting started

Install Python 3 and pyserial:

```sh
pip install pyserial
```

Plug in the board and run:

```sh
python3 tools/widscan58cli.py info
```

The output looks something like this (trimmed):

```
INFO widscan58|esp32c5|...
{
 "name": "c5-race",
 "channels": [
  { "id": 0, "name": "R1", "khz": 5658000 },
  ...
STATUS {"gain":45, ...}
```

An error about `/dev/ttyACM0` means the script couldn't open the port; see
"The serial port" below.

Three things to know up front:

- **The board ends up in monitor mode.** The board starts up in lap timer mode,
  where it stays quiet. Every run of the script (except `bootloader`) switches
  the board to monitor mode first. To put it back in lap timer mode, run
  `python3 tools/widscan58cli.py cmd "PROTOCOL RH"`, or unplug it and plug it
  back in.
- **One program at a time.** Only one program can have the serial port open.
  Click Disconnect in the web page first, and close `pio device monitor` or
  anything else using the port.
- **Settings stick around.** Anything changed with `cmd` (gain, channels and
  so on) stays until the board restarts, so one run sets things up and the
  next one captures.

### The serial port

The script uses `/dev/ttyACM0` unless told otherwise with `-p`. The `-p` goes
*before* the command name:

```sh
python3 tools/widscan58cli.py -p /dev/ttyACM1 info
```

On Windows that's something like `-p COM5`, and on a Mac
`-p /dev/cu.usbmodem1101`.

### Extra Python packages

`info`, `cmd`, `monitor` and `spectrum` only need pyserial. Saving raw samples
needs numpy, and anything with `--plot` also needs matplotlib:

```sh
pip install numpy matplotlib
```

## The commands

| Command | What it does |
|---|---|
| `info` | Prints the firmware version, the channel plan and the current settings. |
| `cmd "LINE" ...` | Sends one or more command lines to the board and prints the replies. |
| `monitor` | Shows a live table of every channel, updated twice a second. |
| `rawiq LO_KHZ N` | Grabs `N` raw I/Q samples with the radio tuned to `LO_KHZ`. |
| `spectrum` | Collects the board's spectrum for a few seconds and keeps the highest reading at each frequency. |
| `bootloader` | Asks the board to reboot into its flashing mode. This doesn't work reliably yet; holding BOOT while pressing RESET always does. |

`python3 tools/widscan58cli.py --help` prints the short version.

### info

```sh
python3 tools/widscan58cli.py info
```

Prints three things: the `INFO` line (firmware version and build), the band
profile with the channel plan, and the full `STATUS?` settings as JSON.

### cmd

```sh
python3 tools/widscan58cli.py cmd "GAIN MANUAL 50"
python3 tools/widscan58cli.py cmd "CHANNELS R1=5658 R2=5695" "STATUS?"
```

Each line is sent in order, and the script prints everything the board says
for about a second after each one. Quote each command so the shell keeps it
together. The full list of commands is in the
[Useful commands](../CLAUDE.md#useful-commands) table in CLAUDE.md.

### monitor

```sh
python3 tools/widscan58cli.py monitor
```

Clears the screen and redraws a table of all channels until Ctrl+C. `-s 10`
stops after 10 seconds, and `--plain` skips the screen clearing (for saving
the output to a file).

The columns:

| Column | Meaning |
|---|---|
| `ch`, `MHz` | Channel name and frequency. |
| `LO` | Where the radio was tuned to read this channel. Two neighbouring channels often share one. |
| `RSSI` | Power in the channel, in dBFS (0 is the loudest the receiver can take). |
| `EMA` | RSSI smoothed over about 100 ms. |
| `floor` | The noise level in the channel. |
| `SNR` | How far the signal is above the noise, in dB. Around 0 means nothing's there. |
| `clear` | The channel's power averaged only while nobody's on it. |
| `peak` | The strongest single frequency bin in the channel. |
| `occ kHz` | The frequency the signal sits on most, as an offset from the channel's frequency. |
| `cen kHz` | The middle of the signal's power, as an offset. A VTX that's a bit off frequency shows up here. |
| `OBW99 kHz` | How wide the signal is (the band holding 99 % of its power). |
| `flags` | `SIG` signal present, `SAT` the receiver clipped (too loud), `DC` the tuning point is inside the channel, `EDGE` the channel runs past the edge of the receiver's filter, `NOC` no signal to find a center for, `SPUR` a known noise spike from the board was cut out. |

The two lines under the table show the overall noise, gain and clipping, and
how fast the board is scanning (`rot` is full passes over all channels per
second).

### rawiq

```sh
python3 tools/widscan58cli.py rawiq 5658000 16380 -o r1.npy
```

Tunes the radio to the given frequency (in kHz) and grabs that many I/Q
samples, then prints a one-line summary (average, RMS level, biggest I and Q
value). Options:

- `-o FILE.npy` saves the samples as a numpy array of complex numbers.
- `--plot FILE.png` saves a picture: the spectrum on top and the first couple
  thousand samples of I and Q below.
- `-r CODE` picks the sample rate. On the ESP32-C5: 0 = 80, 1 = 40, 2 = 20,
  3 = 10, 4 = 8, 5 = 4 MS/s. On the ESP32-S3: 0 = 80, 1 = 40, 2 = 16 MS/s.
  Without it, the capture uses the scanner's current rate.

Limits: at most 16380 samples per grab. On the ESP32-C5 the frequency has to
be a whole number of MHz, so it ends in `000`. `--plot` needs at least 2048
samples (the spectrum is worked out in 2048-sample pieces).

The saved values are raw ADC counts (the ADC is 10-bit, so roughly −512 to
511), not volts or dBm.

### spectrum

```sh
python3 tools/widscan58cli.py spectrum -s 5 --plot band.png
```

Listens to the spectrum frames the board sends while it scans, for 3 seconds
(or whatever `-s` says), and keeps the highest level seen at each frequency.
Without `--plot` it only prints how many frames and points it got. The plot marks each channel with a red line
and its name.

Keeping the highest reading makes it good at catching things that come and
go, like a VTX being switched on, or Wi-Fi bursts.

## Capture examples

### Save a raw capture and load it in Python

Grab the most samples possible at R1:

```sh
python3 tools/widscan58cli.py rawiq 5658000 16380 -o r1.npy
```

Then load it:

```python
import numpy as np

x = np.load("r1.npy")        # complex numbers, ADC counts
print(len(x), x.dtype)        # 16380 complex64
power = np.mean(abs(x) ** 2)
print(10 * np.log10(power), "dB (ADC counts²)")
```

At 80 MS/s, 16380 samples is about 0.2 ms of radio.

### A quick picture of one frequency

```sh
python3 tools/widscan58cli.py rawiq 5740000 8192 --plot r4.png
```

`r4.png` shows the spectrum around 5740 MHz and a slice of the raw waveform.
I and Q traces that flatten at the top and bottom mean the signal is too
strong: lower the gain (next example).

### Change the gain, then capture

Settings stay until the board restarts, so set them first and capture after:

```sh
python3 tools/widscan58cli.py cmd "GAIN MANUAL 30"
python3 tools/widscan58cli.py rawiq 5740000 16380 -o r4-gain30.npy
```

Lower numbers mean less gain. The default is 45.

### Watch the whole band for a while

```sh
python3 tools/widscan58cli.py spectrum -s 30 --plot band-30s.png
```

Power up a quad or two while it runs, and every VTX that came on in those 30
seconds shows up in the plot.

### Log the channel readings to a file

```sh
python3 tools/widscan58cli.py monitor --plain -s 60 > readings.txt
```

This writes a new table every half second for a minute. It's plain text, so
it's easy to read through or pull apart with a script later.

### Scan only the channels that matter

```sh
python3 tools/widscan58cli.py cmd "CHANNELS R1=5658 R8=5917"
python3 tools/widscan58cli.py monitor
```

Fewer channels means each one gets read more often. `CHANNELS DEFAULT` puts the
usual plan back.

## When things go wrong

- **`could not open port /dev/ttyACM0`**: the board isn't plugged in, it's on
  a different port (try `ls /dev/ttyACM*`), or another program has it open.
- **`no reply to PROTOCOL MONITOR`**: the board isn't answering. Unplug it and
  plug it back in, or press RESET, and try again. If it still doesn't answer,
  it probably doesn't have this firmware on it; the web page's Flash device
  button puts it on.
- **`ERR ...` from `rawiq`**: usually the frequency or sample count is out of
  range. Check the limits under `rawiq` above.
- **The web page says "The device has been lost" right after using the
  script**: the script leaves the port the way the web browser expects, so
  this shouldn't happen. If it does, press RESET on the board, or run
  `stty -F /dev/ttyACM0 min 1`.
