# lapcapture: flight captures replayed through lap.c

Notes for the agent running lap timing tests. Records one channel's raw
level codes while a drone flies past the board, then replays them through the
firmware's own `firmware/src/scan/lap.c` (built on the host) at plateau N=0
and N, and plots each pass. Offline replay compares lap.c rules and
thresholds on the same flight, which a live lap timer mode run cannot.

| File | What |
|---|---|
| `lapcapture.sh` | Entry point: `capture usb\|net`, `sweep`, `analyze` |
| `capture.py` | Monitor mode with lap timer mode's scan settings (`FFT 64`, `METRICS RSSI`, gain 45, `THROTTLE 1050`, one channel, no spectrum); writes `laps.csv` (`seq,t_ms,rssi,saturated`) |
| `replay.c` | Feeds `laps.csv` through `lap_feed` at each N given; prints passes (`pass_ms(pk…,peak-run first..last,plat first..last)`), writes the filtered trace |
| `plot.py` | `timeline.png` (whole capture, passes numbered) and `pass_detail.png` (±600 ms per pass: reported pass, N span shaded green, N=0 span orange, peak and peak − N lines, optional reference time in black), and a pass table (`passes.txt` from `analyze`; times in ms) |

## Workflow

```sh
cd tools/lapcapture
OUT=<scratchpad>/flight1
./lapcapture.sh capture usb 180 $OUT          # or: capture net 180 $OUT
./lapcapture.sh sweep $OUT                    # passes found per EnterAt/ExitAt pair
./lapcapture.sh analyze $OUT 120 100          # replay at N=0 and N=2 (the default), plot
```

Put the output in the scratchpad, not the repo. `capture` passes extra options
to `capture.py`: `--chan R1=5658` (default), `--gain 45`, `--fft 64`,
`-p /dev/ttyACM0`.

- Run a 3–5 s capture first to check the setup; with the VTX off the level
  sits at ~15–35 codes at gain 45.
- 180 s at 1050 updates/s is ~189,000 rows (~3.7 MB). `seq gaps 0` and
  `crc errors 0` at the end mean nothing was lost.
- Pick thresholds with `sweep`: the pass count should be stable over a range
  of pairs. Use the middle of that range. Peaks vary by day and flight path
  (2026-10-26: 177–189, EnterAt 145 / ExitAt 120; 2026-10-27: 128–157,
  145/120 found 7 of 19 passes, 125/105 … 105/85 all found 19, used 120/100;
  later the same day: 93–137, background up to 60–90 between passes, 90/75).
- A landing or takeoff near the board can add passes at the end or start;
  check the timeline and leave them out of the analysis.
- Without a reference the replay shows how far N moves each pass against
  N=0 and why (span, shoulders, bridged humps), not which is correct. A
  camera reference (below) shows which.
- Show both PNGs to the user (Read them first to check the layout).

## Reference times (camera)

`plot.py --ref FILE --ref-offset S` draws reference pass times, e.g. from a
camera, on `pass_detail.png`. `analyze` does not pass them; rerun `plot.py`
after it:

```sh
python3 plot.py $OUT --enter 90 --exit 75 --n 2 --ref $OUT/cam.txt --ref-offset 1.196 > $OUT/passes.txt
```

- `FILE`: one time per line in seconds on the reference's clock, optionally
  followed by `, note`. Capture time = reference − `S`. A camera file with
  several flights (`Data-1`, `Data-2` sections) is split into one file per
  capture first.
- Each reference within ±600 ms of a pass is a black line labelled with its
  time; the panel title adds `ref <time> s: N=<N> <±ms> ms, N=0 <±ms> ms`
  (reference − pass). A line with a note is dotted and its title time has a
  `?`: notes mark passes the camera timed badly (off camera, a crash). Remove
  notes that don't mean that (2026-10-27: "beep" entries were clean).
- The offset is fixed per capture: the median of reference − N pass over the
  matched passes without a note, matched as the nearest reference within
  0.5 s. Check the drift (slope of the error over the capture: 2026-10-27,
  10–45 ms over 180 s, small against the ±50 ms scatter) and passes the
  reference has and the replay doesn't (missed passes) or the other way round
  (false passes).
- Compare N and N=0 over all clean passes and over the passes where they
  differ by ≥ 20 ms: most of the error is common to both (2026-10-27: the
  two errors correlated 0.88; the camera often fired on the rising edge, up to
  −110 ms). On the passes where N=0 and N=2 differed, N=2 was closer on 7 of 10
  (median 19.5 ms against 55 ms), the broad flat tops most clearly; over all
  48, median 29 ms against 35 ms.

## USB mode

The board is on this machine (`/dev/ttyACM0`). `capture.py` imports
`../widscan58cli.py`, which leaves the tty at VMIN=1. Nothing else may hold the
port: disconnect the web UI tab, stop `serial_bridge.py`.

## Network mode (RotorHazard host)

The board is plugged into the RotorHazard server: `rotorhaz.lan` (`RH_HOST`),
ssh as the user without a password, web UI on port 5000. `capture net`:

1. stops `rotorhazard.service` (`sudo -n systemctl stop rotorhazard`, works
   without a password), which holds the port;
2. copies `capture.py`, `widscan58cli.py` and `ttyfix.py` to `~/widscan58cap/`;
3. runs the capture there with the service's venv (`~/.venv/bin/python`,
   `RH_PY`; it has pyserial, the system Python does not), detached with
   `nohup … </dev/null`, so the ssh session returns and a dropped connection
   doesn't stop it;
4. polls `capture.log` until `done`, then copies `laps.csv` and `capture.log`
   back to OUTDIR.

`capture net` blocks for the whole capture. Run it with `run_in_background`
(Bash) and tell the user it has started, so they can fly. The capture starts
a few seconds after the command does.

Leave the service stopped afterwards unless the user asks for it back
(`sudo systemctl start rotorhazard`; opening the port resets the board into
lap timer mode). The capture leaves the board in monitor mode on R1 only.
