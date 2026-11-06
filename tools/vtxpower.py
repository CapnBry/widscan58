#!/usr/bin/env python3
"""Measure a VTX's output on every Raceband channel and power level.

GPL-3.0-or-later.

The VTX is set over Betaflight MSP and measured on an ImmersionRC RF Power
Meter v2, or with --widscan58 on a widscan58 ESP32-C5 board (all over USB). The
result is a table (Markdown and CSV), a graph (PNG) and the raw records (JSON
lines).

  vtxpower.py check [--attn dB] [--partial]   FC, VTX table and meter; changes nothing
  vtxpower.py sweep [--attn dB] [--partial]   start with the VTX off, power it up when asked
  vtxpower.py sweep --widscan58 --attn dB     the same, on a widscan58 instead of the meter
  vtxpower.py report <run>.jsonl              redo the table and graph from a run's records

--title replaces the table's and graph's title (a sweep keeps it for report);
--output-w sets the graph's width in pixels (default 800).

--attn is the attenuator between the VTX and the meter's input, in dB (default
0: the meter straight on the VTX, its internal 30 dB already in the reading).
Every reported value is the VTX's own output: the meter's reading plus --attn.
Measure a chain rather than trusting its rating: a "30 dB" chain has measured
~25 dB at 5.8 GHz. An --attn that is wrong makes the reported values wrong by
as much, and the table check below; the meter's reading is checked as it is.

The power limit (--max-mw, default 500) is the power at the meter's input, after
the attenuator, so a 2 W VTX can be measured through 10 dB or more:

- A table level over the limit, its label less --attn, is an error (exit code 1)
  and nothing is swept. --partial skips those levels instead. A label that
  doesn't read as a power (MAX) counts as too high at any attenuation. RCE
  counts as 25 mW.
- Betaflight's VTX table can change when a SmartAudio VTX comes up, so it is
  read after the VTX is ready, and again before every step; the sweep aborts if
  it has changed.
- Betaflight reports its settings, not the VTX's state, so the meter is watched
  too, from the moment a level is set. A reading over the limit drops the VTX to
  the lowest level at once, and that level and those above it stay off for the
  rest of the sweep.
- A reading at the meter's floor, on a level that should clear it (cable off,
  VTX not responding), aborts before any higher level is set.
- Any exit (done, abort, error, Ctrl-C, SIGTERM) leaves the VTX on the channel
  it started on, at the lowest level, out of pit mode, and the meter has to
  confirm the drop: if it still reads over the limit, the script says to power
  the VTX off. Nothing is saved to the FC's EEPROM.

Betaflight reports a change before the VTX has finished it, and after a channel
change OpenVTx ramps its output up slowly, so a step is read again until its
readings hold still: under 1 dB of spread at each calibration point and under
1.5 dB between the two. A step that doesn't settle within --stable-timeout is
kept, marked in the table and CSV, and left off the graph.

--quick cuts the script's own waiting, for a fast look: no padding to a fixed
step, a shorter meter settle and 3 readings per calibration point instead of 10.
Every step still waits for Betaflight to report the change, and the guards all
stay on. The readings are noisier, and the VTX heats on a different schedule, so
compare quick runs with quick runs. An explicit --step, --measure-at, --reads or
--settle overrides it.

--widscan58 measures as the web UI's power meter mode does, with its calibration
(R1-R8 only, about +/- 1 dB): one LO per channel, gain 0, a +/- 15 MHz window.
The board has no attenuator of its own and reads up to -5 dBm at its input, so
--attn is required, --max-mw defaults to that -5 dBm (0.316 mW) and can only go
lower. 25 mW needs 19 dB, 400 mW 31 dB, 1 W 35 dB and 2 W 38 dB; the first
attenuator takes nearly all the power, so rate it at least twice the VTX. A
capture that clips counts as over the limit. Afterwards the board goes back to
its default scan and its boot mode (lap timer). --widscan58-port overrides the
port.

Before the sweep, with the VTX off, the meter's floor is measured (~-24 dBm at
5.8 GHz; ~-28 dBm on a widscan58) and subtracted linearly from the readings. Each channel goes from low
to high power, on a fixed --step schedule; the VTX heats up during a sweep, and
later channels read up to ~0.8 dB low (PLAN.md, "Power calibration, repeat").
"""
import argparse
import csv
import json
import math
import re
import queue
import signal
import statistics
import sys
import threading
import time
from pathlib import Path

import widscan58cli as w
from powercal import METER_FREQS, RACEBAND, Meter, Msp, find_port, mean_db

CHANNEL_NAMES = {mhz: f"R{i + 1}" for i, mhz in enumerate(RACEBAND)}
# dBm at the board's input minus dBFS at gain 0: web/src/model/power.ts, CALIBRATION.
WIDSCAN58_CAL = [(5658, 27.4), (5695, 27.3), (5732, 26.8), (5769, 26.1),
                 (5806, 25.9), (5843, 25.3), (5880, 25.2), (5917, 26.1)]
WIDSCAN58_MAX_DBM = -5.0  # web/src/model/power.ts, INPUT_MAX_DBM
WIDSCAN58_CAL_REACH_MHZ = 10  # beyond the calibrated channels, as the web UI: uncalibrated
WIDSCAN58_FRAMES = 8  # metrics frames per reading
WIDSCAN58_LO_REACH_KHZ = 20000  # a frame's LO this close to the channel is on it (MODE SINGLE: 8 MHz below)
NO_SIGNAL_DB = 3.0  # a reading this close to the floor is no signal
NEAR_FLOOR_DB = 6.0  # flagged in the report; the floor correction is large here
VISIBLE_MARGIN_DB = 6.0  # a level's label this far over the floor should show on the meter; VTXs read low
TIMING = dict(step=8.0, measure_at=2.0, reads=10, settle=0.7)
QUICK_TIMING = dict(step=0.0, measure_at=0.5, reads=3, settle=0.3)
METER_READ_S = 0.06  # one meter reading, for the time estimate
STABLE_SPREAD_DB = 1.0  # within a calibration point; settled readings stay under ~0.4
STABLE_GAP_DB = 1.5  # between the two calibration points; settled readings stay under ~0.6
DEFAULT_TITLE = "VTX output by channel and power level"
DEFAULT_OUTPUT_W = 800
GRAPH_W_IN = 9.5
DROP_CONFIRM_S = 5.0  # for the meter to see a drop: a busy SmartAudio VTX can take a few seconds


class SafetyAbort(Exception):
    """The sweep can't go on safely."""


class OverLimit(Exception):
    def __init__(self, dbm):
        super().__init__(f"{dbm:.1f} dBm")
        self.dbm = dbm


def dbm_to_mw(dbm):
    return 10 ** (dbm / 10)


def mw_to_dbm(mw):
    return 10 * math.log10(mw)


def label_mw(label):
    """A table label as mW: '25', '400mW', '1W', '1.6 W', 'RCE'. None for anything else."""
    if label.strip().upper() == "RCE":
        return 25.0
    m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*(mw|w)?\s*", label, re.IGNORECASE)
    if not m:
        return None
    mw = float(m[1]) * (1000 if (m[2] or "").lower() == "w" else 1)
    return mw if mw > 0 else None


def level_name(nominal, label):
    return "RCE" if label.strip().upper() == "RCE" else f"{nominal:g} mW"


def at_meter_mw(mw, attn_db):
    return mw / 10 ** (attn_db / 10)


def classify(table, max_mw, attn_db):
    """(levels within the limit, lowest first; levels too high or of unknown power)."""
    allowed, rejected = [], []
    for lv in table:
        mw = label_mw(lv["label"])
        if mw is not None and at_meter_mw(mw, attn_db) <= max_mw:
            allowed.append(dict(lv, mw=mw))
        else:
            rejected.append(lv)
    return sorted(allowed, key=lambda lv: (lv["mw"], lv["index"])), rejected


def describe_level(lv, max_mw, attn_db):
    mw = label_mw(lv["label"])
    head = f"  {lv['index']}: {lv['label'].strip()!r:8}"
    if mw is None:
        return f"{head} power unknown: too high"
    meter = at_meter_mw(mw, attn_db)
    if meter <= max_mw:
        return f"{head} {meter:7.3g} mW at the meter: OK"
    return f"{head} {meter:7.3g} mW at the meter: too high, needs {mw_to_dbm(mw / max_mw):.1f} dB"


def describe_table(table, max_mw, attn_db):
    return "\n".join(describe_level(lv, max_mw, attn_db) for lv in table)


def check_table(table, args):
    """The levels to sweep. Raises SafetyAbort if the table doesn't allow a sweep."""
    allowed, rejected = classify(table, args.max_mw, args.attn)
    where = f"{args.attn:g} dB, limit {args.max_mw:.3g} mW at the meter"
    if rejected and not args.partial:
        raise SafetyAbort(f"the VTX table has levels too high for {where}:\n{describe_table(table, args.max_mw, args.attn)}\n"
                          "Use a larger attenuator, or --partial to skip those levels.")
    if not allowed:
        raise SafetyAbort(f"no level in the VTX table is within {where}:\n{describe_table(table, args.max_mw, args.attn)}")
    return allowed


class Vtx:
    """The only way this script changes the VTX: every level the sweep sends goes through the allow list."""

    def __init__(self, msp, args, start):
        self.msp = msp
        self.args = args
        self.start = start  # MSP_VTX_CONFIG when the script started
        self.table = None
        self.allowed = []
        self.freq = None

    def adopt_table(self, table):
        self.allowed = check_table(table, self.args)
        self.table = table

    def send(self, mhz, index, wait=True):
        if index not in {lv["index"] for lv in self.allowed}:
            raise SafetyAbort(f"refusing power index {index}: not in the allowed levels")
        now = self.msp.power_levels()
        if now != self.table:
            raise SafetyAbort(f"VTX table changed during the sweep: {self.table} -> {now}")
        self.msp.set_vtx(mhz, index)
        self.freq = mhz
        if wait:
            return self.msp.wait_vtx(mhz, index, False)

    def lowest(self):
        """The table's lowest level, as of now, within the limit or not: the way down from anything
        higher. The first entry if no label reads as a power."""
        table = self.msp.power_levels()
        known = [(label_mw(lv["label"]), lv["index"], lv) for lv in table if label_mw(lv["label"]) is not None]
        if known:
            return min(known, key=lambda k: k[:2])[2]
        if not table:
            raise SafetyAbort("the VTX table is empty")
        return table[0]

    def drop(self):
        """To the lowest level on the current channel, whatever else has gone wrong."""
        lv = self.lowest()
        mhz = self.freq or self.start["freq"]
        self.msp.set_vtx(mhz, lv["index"])
        return mhz, lv["label"].strip()

    def leave(self, timeout=8.0):
        """The channel the script started on, the lowest level, pit mode off. Band and channel go back
        as they were if it was set that way: a frequency alone would leave Betaflight on band 0."""
        lv = self.lowest()
        band, channel, freq = self.start["band"], self.start["channel"], self.start["freq"]
        target = (band - 1) * 8 + channel - 1 if band >= 1 and channel >= 1 else freq
        self.msp.set_vtx(target, lv["index"])
        self.freq = freq
        deadline = time.time() + timeout
        sent = time.time()
        while True:
            c = self.msp.vtx_config()
            if c["freq"] == freq and c["power"] == lv["index"] and not c["pit"]:
                return freq, lv["label"].strip()
            if time.time() > deadline:
                raise SafetyAbort(f"the FC did not take {freq} MHz, power {lv['index']}, pit off: {c}")
            if time.time() - sent > 1.0:
                self.msp.set_vtx(target, lv["index"])
                sent = time.time()
            time.sleep(0.2)


def cal_points(mhz):
    """The meter's two calibration frequencies around a channel."""
    lo = max(f for f in METER_FREQS if f <= mhz)
    return sorted({lo, min(lo + 50, 6000)})


def interpolate(mhz, by_freq):
    pts = sorted(by_freq.items())
    if len(pts) == 1:
        return pts[0][1]
    (f0, a), (f1, b) = pts
    return a + (b - a) * (mhz - f0) / (f1 - f0)


class ImmersionRC(Meter):
    """The RF power meter: read at the two calibration frequencies around a channel."""
    name = "immersionrc"
    default_floor_dbm = -24.0  # at 5.8 GHz; used when the VTX is already on at the start

    def points(self, mhz):
        return cal_points(mhz)

    def read_dbm(self):
        return float(self.cmd("D")[0])

    def close(self):
        pass


class Widscan58:
    """A widscan58 board as power meter mode runs it: one channel at a time, reading its RSSI window."""
    name = "widscan58"
    default_floor_dbm = -28.0  # the +/- 15 MHz window's noise at gain 0, measured -27 to -29

    def __init__(self, port, channels):
        for mhz in channels:
            if not WIDSCAN58_CAL[0][0] - WIDSCAN58_CAL_REACH_MHZ <= mhz <= WIDSCAN58_CAL[-1][0] + WIDSCAN58_CAL_REACH_MHZ:
                raise SafetyAbort(f"{mhz} MHz is outside the widscan58's power calibration (R1-R8)")
        self.link = w.Link(port, timeout=0.01)
        self.text = queue.Queue()
        self.cond = threading.Condition()
        self.collecting = None  # the frames a reading is waiting for
        self.error = None
        self.stopping = False
        self.mhz = None
        # Always drained: frames left waiting on the device would make readings
        # old, and the limit check has to see the power as it is now.
        self.reader = threading.Thread(target=self._read_stream, daemon=True)
        self.reader.start()
        self._to_monitor()
        profile = json.loads(self.command("PROFILE?", prefix="PROFILE ")[len("PROFILE "):])
        if profile.get("name") != "c5-race":
            raise SafetyAbort(f"no power calibration for band profile {profile.get('name')!r}: needs an ESP32-C5 (c5-race)")

    def _read_stream(self):
        try:
            while not self.stopping:
                for item in self.link.poll():
                    if item[0] == "text":
                        self.text.put(item[1])
                    elif item[1] == w.FRAME_METRICS:
                        with self.cond:
                            if self.collecting is not None:
                                self.collecting.append(w.parse_metrics(item[2]))
                                self.cond.notify_all()
        except Exception as e:  # noqa: BLE001: reported by the next reading
            self.error = e
            with self.cond:
                self.cond.notify_all()

    def _to_monitor(self):
        # Opening the port can restart the board, and it boots silent in lap timer mode.
        for _ in range(5):
            try:
                self.command("PROTOCOL MONITOR", timeout=1.0)
                return
            except TimeoutError:
                pass
        raise TimeoutError("the widscan58 did not answer PROTOCOL MONITOR")

    def command(self, line, prefix=None, timeout=2.0):
        """The first reply line (with prefix, or OK/ERR)."""
        while not self.text.empty():
            self.text.get_nowait()
        self.link.send(line)
        deadline = time.time() + timeout
        while True:
            try:
                reply = self.text.get(timeout=max(0.0, deadline - time.time()))
            except queue.Empty:
                raise TimeoutError(f"widscan58: no reply to {line!r}") from None
            if reply.startswith(prefix if prefix else ("OK", "ERR")):
                return reply

    def _send(self, *lines):
        for line in lines:
            reply = self.command(line)
            if reply.startswith("ERR"):
                raise SafetyAbort(f"widscan58: {line!r} -> {reply}")

    def version(self):
        return self.command("INFO", prefix="INFO")

    def points(self, mhz):
        return [mhz]

    def set_freq(self, mhz):
        if mhz == self.mhz:
            return
        # The scan the calibration was made with (web/src/model/power.ts, powerCommands).
        self._send(f"CHANNELS X={mhz}", "MODE SINGLE", "ENABLE 1", "GAIN MANUAL 0", "WIN 15000000 15000000",
                   "METRICS FULL", "SPEC 0 256 40000000 MAX", "SCAN START")
        self.mhz = mhz

    def _frames(self, n, timeout=3.0):
        """The next n frames that are on this channel at gain 0."""
        deadline = time.time() + timeout
        on_channel = []
        with self.cond:
            self.collecting = []
            try:
                while len(on_channel) < n:
                    if self.error:
                        raise RuntimeError(f"widscan58: {self.error}")
                    if time.time() > deadline:
                        raise TimeoutError(f"widscan58: no readings on {self.mhz} MHz")
                    self.cond.wait(0.1)
                    on_channel += [m for m in self.collecting if m["gain"] == 0 and m["nch"]
                                   and abs(m["lo_khz"] - self.mhz * 1000) <= WIDSCAN58_LO_REACH_KHZ]
                    self.collecting.clear()
            finally:
                self.collecting = None
        return on_channel

    def read_dbm(self):
        """dBm at the board's input. The median: Wi-Fi bursts in the window raise a mean."""
        frames = self._frames(WIDSCAN58_FRAMES)
        dbfs = statistics.median(m["channels"][0]["rssi"] / 100 for m in frames)
        dbm = dbfs + interpolate_cal(self.mhz)
        # A clipped capture reads low, so its level says nothing: it counts as over the range.
        if sum(1 for m in frames if m["saturated"]) * 2 > len(frames):
            dbm = max(dbm, WIDSCAN58_MAX_DBM + 0.1)
        return dbm

    def read(self, n, settle):
        time.sleep(settle)
        v = [self.read_dbm() for _ in range(n)]
        return mean_db(v), max(v) - min(v)

    def close(self):
        """Back to the board's default scan and its boot mode."""
        try:
            self._send("CHANNELS DEFAULT", "MODE GROUP", "GAIN MANUAL 45", "WIN 5000000 12000000",
                       "SPEC 4 256 40000000 MAX", "PROTOCOL RH")
        finally:
            self.stopping = True
            self.reader.join(timeout=1.0)


def interpolate_cal(mhz):
    pts = WIDSCAN58_CAL
    if mhz <= pts[0][0]:
        return pts[0][1]
    for (f0, a), (f1, b) in zip(pts, pts[1:]):
        if mhz <= f1:
            return a + (b - a) * (mhz - f0) / (f1 - f0)
    return pts[-1][1]


def open_meter(args):
    if args.widscan58:
        return Widscan58(args.widscan58_port, getattr(args, "channels", RACEBAND))
    return ImmersionRC(args.meter)


class Guard:
    """Meter readings, each one checked against the limit: the power at the meter's input."""

    def __init__(self, meter, max_mw):
        self.meter = meter
        self.limit_dbm = mw_to_dbm(max_mw)

    def read(self):
        raw = self.meter.read_dbm()
        if raw > self.limit_dbm:
            raise OverLimit(raw)
        return raw

    def watch(self, until):
        """Keep reading until the given time."""
        while time.time() < until:
            self.read()
            time.sleep(min(0.05, max(0.0, until - time.time())))

    def confirm_low(self, timeout=DROP_CONFIRM_S):
        """Raise SafetyAbort if the meter still reads over the limit after timeout."""
        deadline = time.time() + timeout
        while True:
            raw = self.meter.read_dbm()
            if raw <= self.limit_dbm:
                return
            if time.time() > deadline:
                raise SafetyAbort(f"the meter still reads {raw:.1f} dBm ({dbm_to_mw(raw):.0f} mW) after lowering the VTX")
            time.sleep(0.2)

    def measure(self, freq, reads, settle):
        self.meter.set_freq(freq)
        self.watch(time.time() + settle)
        v = [self.read() for _ in range(reads)]
        return mean_db(v), max(v) - min(v)


def measure_settled(guard, mhz, args):
    """A step's readings at its calibration points, and whether they held still."""
    deadline = time.time() + args.stable_timeout
    while True:
        meter = {f: guard.measure(f, args.reads, args.settle) for f in guard.meter.points(mhz)}
        levels = [v[0] for v in meter.values()]
        if max(v[1] for v in meter.values()) <= STABLE_SPREAD_DB and max(levels) - min(levels) <= STABLE_GAP_DB:
            return meter, True
        if time.time() > deadline:
            return meter, False


def measure_floor(meter, channels, reads, settle):
    floors = {}
    for f in sorted({p for mhz in channels for p in meter.points(mhz)}):
        meter.set_freq(f)
        floors[f] = meter.read(reads, settle)[0]
        print(f"  {f} MHz: {floors[f]:.2f} dBm", file=sys.stderr)
    return floors


def at_vtx_dbm(raw, floor, attn_db):
    """Output at the VTX: the reading less the meter's floor, plus the attenuator."""
    if raw - floor >= NO_SIGNAL_DB:
        raw = 10 * math.log10(10 ** (raw / 10) - 10 ** (floor / 10))
    return raw + attn_db


def wait_ready(args, msp, guard):
    print("\n>>> Power up the VTX now. Waiting for Betaflight to see it; Ctrl-C quits.\n", file=sys.stderr)
    deadline = time.time() + args.ready_timeout
    while not msp.vtx_config()["ready"]:
        if time.time() > deadline:
            raise SafetyAbort(f"VTX not ready after {args.ready_timeout:g} s")
        guard.watch(time.time() + 0.5)
    # A SmartAudio VTX's table comes in after it answers.
    guard.watch(time.time() + args.table_wait)


def confirm_plan(args, vtx, table):
    print("VTX table:", file=sys.stderr)
    print(describe_table(table, args.max_mw, args.attn), file=sys.stderr)
    skipped = len(table) - len(vtx.allowed)
    if skipped:
        print(f"--partial: skipping {skipped} level{'s' if skipped > 1 else ''}.", file=sys.stderr)
    labels = ", ".join(lv["label"].strip() for lv in vtx.allowed)
    n = len(args.channels) * len(vtx.allowed)
    per_step = max(args.step, args.measure_at + 2 * (args.settle + args.reads * METER_READ_S))
    print(f"Sweep: {len(args.channels)} channels x levels [{labels}], limit {args.max_mw:.3g} mW at the meter, "
          f"{args.attn:g} dB attenuator: {n} steps, at least {n * per_step / 60:.1f} min", file=sys.stderr)
    if args.yes:
        return
    try:
        input("Check the attenuator and the cable. Enter starts; Ctrl-C quits. ")
    except EOFError:
        raise SafetyAbort("no confirmation (stdin closed); --yes skips it") from None


def run_steps(args, vtx, guard, floors, out):
    levels = vtx.allowed
    blocked_mw = None  # this level and above stay off once one has read over the limit
    first = True
    total = len(args.channels) * len(levels)
    n = 0
    for mhz in args.channels:
        for lv in levels:
            n += 1
            if blocked_mw is not None and lv["mw"] >= blocked_mw:
                continue
            t0 = time.time()
            rec = dict(rec="step", t=t0, mhz=mhz, index=lv["index"], label=lv["label"].strip(), nominal_mw=lv["mw"])
            try:
                # The meter is on the channel's frequency before the level changes.
                guard.meter.set_freq(guard.meter.points(mhz)[0])
                vtx.send(mhz, lv["index"])
                guard.watch(t0 + args.measure_at)
                meter, settled = measure_settled(guard, mhz, args)
            except OverLimit as e:
                mhz_low, label = vtx.drop()
                guard.confirm_low()
                print(f"[{n}/{total}] {mhz} {lv['label']:>5}: the meter read {e.dbm:.1f} dBm ({dbm_to_mw(e.dbm):.0f} mW), over "
                      f"the limit. Dropped to {label} on {mhz_low} MHz; this level and above stay off.", file=sys.stderr)
                if lv["index"] == levels[0]["index"]:
                    raise SafetyAbort("the lowest level reads over the limit at the meter: use a larger attenuator") from None
                blocked_mw = lv["mw"]
                rec.update(over=True, meter_dbm=e.dbm, read_dbm=e.dbm + args.attn)
                out.write(json.dumps(rec) + "\n")
                out.flush()
                continue
            raw = interpolate(mhz, {f: v[0] for f, v in meter.items()})
            floor = interpolate(mhz, {f: floors.get(f, guard.meter.default_floor_dbm) for f in meter})
            # R8 at the lowest level can sit near the floor through a pad: only
            # the first step and the higher levels can tell a lost signal, and
            # only a level that should clear the floor through this attenuator.
            visible = mw_to_dbm(lv["mw"]) - args.attn - VISIBLE_MARGIN_DB >= floor + NO_SIGNAL_DB
            if raw - floor < NO_SIGNAL_DB and visible and (first or lv["index"] != levels[0]["index"]):
                raise SafetyAbort(f"no signal on {mhz} MHz at {lv['label'].strip()}: the meter reads {raw:.1f} dBm, its "
                                  f"floor is {floor:.1f}. Check the cable and that the VTX takes the setting.")
            first = False
            dbm = at_vtx_dbm(raw, floor, args.attn)
            rec.update(raw_dbm=raw, floor_dbm=floor, dbm=dbm, mw=dbm_to_mw(dbm), near_floor=raw - floor < NEAR_FLOOR_DB, settled=settled,
                       meter={str(f): dict(dbm=v[0], spread=v[1]) for f, v in meter.items()})
            out.write(json.dumps(rec) + "\n")
            out.flush()
            print(f"[{n}/{total}] {mhz} {lv['label']:>5}: {dbm:6.2f} dBm {rec['mw']:7.1f} mW"
                  f"{'' if settled else '  NOT SETTLED'}", file=sys.stderr)
            time.sleep(max(0.0, t0 + args.step - time.time()))


def cmd_check(args):
    msp = Msp(args.fc)
    cfg = msp.vtx_config()
    print("FC:", json.dumps(cfg))
    table = msp.power_levels()
    print(f"VTX table, {args.attn:g} dB attenuator, limit {args.max_mw:.3g} mW at the meter:")
    print(describe_table(table, args.max_mw, args.attn))
    status = 0
    try:
        allowed = check_table(table, args)
        print("Would sweep:", ", ".join(lv["label"].strip() for lv in allowed))
    except SafetyAbort as e:
        print(f"ERROR: {str(e).splitlines()[0]}", file=sys.stderr)
        print(e.args[0].splitlines()[-1], file=sys.stderr)
        status = 1
    if not cfg["ready"]:
        print("VTX not ready (off?). A SmartAudio VTX's table can change when it comes up; "
              "the sweep reads it again then.")
    meter = open_meter(args)
    try:
        print("Meter:", meter.version())
        for f in (meter.points(mhz)[0] for mhz in (RACEBAND[0], RACEBAND[4], RACEBAND[7])):
            meter.set_freq(f)
            print(f"  {f} MHz: {meter.read(5, args.settle)[0]:.2f} dBm")
    finally:
        meter.close()
    return status


def leave_low(vtx, guard):
    """The last thing a sweep does. Returns an exit status: 1 if the VTX may still be high."""
    try:
        mhz, label = vtx.leave()
    except Exception as e:  # noqa: BLE001
        print(f"WARNING: could not lower the VTX ({e}). IT MAY STILL BE AT HIGH POWER: power it off.", file=sys.stderr)
        return 1
    try:
        guard.confirm_low()
    except SafetyAbort as e:
        print(f"WARNING: {e}. POWER THE VTX OFF.", file=sys.stderr)
        return 1
    except Exception as e:  # noqa: BLE001
        print(f"VTX told to go to {mhz} MHz, {label}, pit off (not saved to the FC); not confirmed, the meter failed: {e}",
              file=sys.stderr)
        return 1
    print(f"VTX left at {mhz} MHz, {label}, pit off (not saved to the FC).", file=sys.stderr)
    return 0


def default_output():
    return f"tmp/vtxpower/{time.strftime('%Y%m%d-%H%M%S')}"


def cmd_sweep(args):
    base = Path(args.output or default_output())
    base.parent.mkdir(parents=True, exist_ok=True)
    jsonl = base.with_suffix(".jsonl")
    if jsonl.exists():
        raise SystemExit(f"{jsonl} exists")

    msp = Msp(args.fc)
    try:
        meter = open_meter(args)
        print("Meter:", meter.version(), file=sys.stderr)
    except (SafetyAbort, OSError, TimeoutError) as e:
        print(f"ABORT: {e}", file=sys.stderr)
        return 1
    guard = Guard(meter, args.max_mw)
    cfg = msp.vtx_config()
    vtx = Vtx(msp, args, cfg)
    print(f"Start: {cfg['freq']} MHz, power {cfg['power']}, pit {cfg['pit']}; goes back to {cfg['freq']} MHz.", file=sys.stderr)
    status = 0
    started = False
    with open(jsonl, "w") as out:
        try:
            # A table already too high stops here with nothing changed. Otherwise the
            # FC comes down to the lowest level before there is a VTX to obey it.
            vtx.adopt_table(msp.power_levels())
            started = True
            vtx.leave()
            floors = {}
            if cfg["ready"]:
                print(f"The VTX is already on: no floor measurement, using {meter.default_floor_dbm:g} dBm.", file=sys.stderr)
            else:
                print("Meter floor, VTX off:", file=sys.stderr)
                floors = measure_floor(meter, args.channels, args.reads, args.settle)
                if msp.vtx_config()["ready"]:
                    print(f"The VTX came up during the floor measurement: discarded, using {meter.default_floor_dbm:g} dBm.",
                          file=sys.stderr)
                    floors = {}
                wait_ready(args, msp, guard)

            table = msp.power_levels()
            vtx.adopt_table(table)
            vtx.leave()  # the table may have a new lowest level
            confirm_plan(args, vtx, table)
            header = dict(rec="header", t=time.time(), attn_db=args.attn, max_mw=args.max_mw, table=table,
                          floors={str(f): v for f, v in floors.items()}, meter=meter.version(), meter_name=meter.name,
                          title=args.title)
            out.write(json.dumps(header) + "\n")
            out.flush()
            run_steps(args, vtx, guard, floors, out)
        except KeyboardInterrupt:
            print("Interrupted.", file=sys.stderr)
            status = 130
        except OverLimit as e:
            print(f"ABORT: the meter reads {e.dbm:.1f} dBm ({dbm_to_mw(e.dbm):.0f} mW) before the sweep, over the limit.",
                  file=sys.stderr)
            status = 1
        except Exception as e:  # noqa: BLE001: any failure still has to reach the drop below
            print(f"ABORT: {e}", file=sys.stderr)
            status = 1
        finally:
            if started:
                # A second Ctrl-C must not stop this.
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                    signal.signal(sig, signal.SIG_IGN)
                status = leave_low(vtx, guard) or status
            try:
                meter.close()
            except Exception as e:  # noqa: BLE001: the VTX comes first, and the report still gets written
                print(f"WARNING: closing the meter: {e}", file=sys.stderr)
    if jsonl.stat().st_size:
        report(jsonl, args.title, args.output_w)
    else:
        jsonl.unlink()
    return status


def load(jsonl):
    header, steps = None, []
    for line in open(jsonl):
        r = json.loads(line)
        if r["rec"] == "header":
            header = r
        else:
            steps.append(r)
    return header, steps


def report(jsonl, title=None, width=DEFAULT_OUTPUT_W):
    jsonl = Path(jsonl)
    header, steps = load(jsonl)
    title = title or (header or {}).get("title") or DEFAULT_TITLE
    levels = sorted({(s["nominal_mw"], s["label"]) for s in steps})
    channels = sorted({s["mhz"] for s in steps})
    if not steps:
        print("No steps measured: no table or graph.", file=sys.stderr)
        return
    write_csv(jsonl.with_suffix(".csv"), steps)
    md = table_markdown(header, steps, levels, channels, title)
    jsonl.with_suffix(".md").write_text(md)
    write_graph(jsonl.with_suffix(".png"), header, steps, levels, channels, title, width)
    print(md)
    print(f"Wrote {jsonl.with_suffix('.md')}, {jsonl.with_suffix('.csv')}, {jsonl.with_suffix('.png')}", file=sys.stderr)


def channel_name(mhz):
    return CHANNEL_NAMES.get(mhz, str(mhz))


def write_csv(path, steps):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow("channel mhz level nominal_mw dbm mw meter_dbm floor_dbm near_floor over_limit unsettled".split())
        for s in steps:
            over = s.get("over", False)
            w.writerow([channel_name(s["mhz"]), s["mhz"], s["label"], f"{s['nominal_mw']:g}",
                        "" if over else f"{s['dbm']:.2f}", "" if over else f"{s['mw']:.1f}",
                        "" if over else f"{s['raw_dbm']:.2f}", "" if over else f"{s['floor_dbm']:.2f}",
                        int(s.get("near_floor", False)), int(over), int(not over and not s.get("settled", True))])


def table_markdown(header, steps, levels, channels, title):
    cells = {(s["mhz"], s["label"]): s for s in steps}
    lines = [f"# {title}", ""]
    if header:
        lines += [f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(header['t']))}. "
                  f"{header['attn_db']:g} dB attenuator. Values are the VTX's output (the meter's reading plus the attenuator). "
                  f"Limit {header['max_mw']:.3g} mW at the meter."
                  + (" Measured on a widscan58: about +/- 1 dB." if header.get("meter_name") == "widscan58" else ""), ""]
    lines += ["| Channel | MHz | " + " | ".join(level_name(*lv) for lv in levels) + " |",
              "|---|---:|" + "---:|" * len(levels)]
    near = over = unsettled = False
    for mhz in channels:
        row = []
        for _, label in levels:
            s = cells.get((mhz, label))
            if s is None:
                row.append("–")
            elif s.get("over"):
                row.append("over limit")
                over = True
            else:
                settled = s.get("settled", True)
                mark = ("*" if s["near_floor"] else "") + ("†" if not settled else "")
                near = near or s["near_floor"]
                unsettled = unsettled or not settled
                row.append(f"{s['dbm']:.1f} dBm, {s['mw']:.0f} mW{mark}")
        lines.append(f"| {channel_name(mhz)} | {mhz} | " + " | ".join(row) + " |")
    lines.append("")
    if near:
        lines += ["\\* Within 6 dB of the meter's floor: less accurate.", ""]
    if unsettled:
        lines += ["† Never held still within the timeout: don't trust this value.", ""]
    if over:
        lines += ["Over limit: the meter read above the limit, the VTX dropped to its lowest level at once, "
                  "and that level and those above it were left out from there on. – is not measured.", ""]
    return "\n".join(lines)


# Categorical slots from the dataviz reference palette, light mode, in order; surface and inks from the same.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#7f7f7f"]
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
Y_TICKS = [1, 2, 5, 10, 25, 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800]


def write_graph(path, header, steps, levels, channels, title, width):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # The layout stays the same at any width; a hair over, as the pixel count is rounded down.
    fig, ax = plt.subplots(figsize=(GRAPH_W_IN, 5.4), dpi=(width + 0.01) / GRAPH_W_IN, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)
    x = {mhz: i for i, mhz in enumerate(channels)}
    measured = [s for s in steps if not s.get("over") and s.get("settled", True)]
    left_out = sum(1 for s in steps if not s.get("over") and not s.get("settled", True))
    # The limit is at the meter; the graph is at the VTX. A limit far above the data isn't drawn.
    limit = header["max_mw"] * 10 ** (header["attn_db"] / 10) if header else None

    top = len(channels) - 1 if channels else 0
    for i, (nominal, label) in enumerate(levels):
        color = SERIES[i % len(SERIES)]
        pts = [s for s in measured if s["label"] == label]
        pts.sort(key=lambda s: s["mhz"])
        if limit is None or not math.isclose(nominal, limit, rel_tol=0.02):
            ax.axhline(nominal, color=color, lw=0.9, ls=(0, (4, 4)), alpha=0.7, zorder=1)
        xs = [x[s["mhz"]] for s in pts]
        ys = [s["mw"] for s in pts]
        ax.plot(xs, ys, color=color, lw=1.6, zorder=3, label=f"{level_name(nominal, label)} setting")
        ax.plot(xs, ys, ls="none", marker="o", ms=6.5, mfc=color, mec=SURFACE, mew=1.2, zorder=4)
        low = [s for s in pts if s["near_floor"]]
        ax.plot([x[s["mhz"]] for s in low], [s["mw"] for s in low], ls="none", marker="o", ms=6.5,
                mfc=SURFACE, mec=color, mew=1.4, zorder=5)
        if pts:
            ax.annotate(level_name(nominal, label), (xs[-1], ys[-1]), xytext=(10, 0), textcoords="offset points",
                        va="center", fontsize=9, color=INK2)
    ax.set_yscale("log")
    over = [s for s in steps if s.get("over")]
    for s in over:
        color = SERIES[levels.index((s["nominal_mw"], s["label"])) % len(SERIES)]
        ax.plot(x[s["mhz"]], dbm_to_mw(s["read_dbm"]), ls="none", marker="x", ms=8, mec=color, mew=2, zorder=5)
        ax.annotate("over limit, dropped", (x[s["mhz"]], dbm_to_mw(s["read_dbm"])), xytext=(9, 0),
                    textcoords="offset points", va="center", fontsize=8.5, color=INK2)
    values = [s["mw"] for s in measured] + [dbm_to_mw(s["read_dbm"]) for s in over] + [nominal for nominal, _ in levels]
    if limit is not None and limit <= max(values) * 2:
        values.append(limit)
        ax.axhline(limit, color=INK, lw=1.0, ls=":", zorder=2)
        ax.annotate(f"Limit: {limit:.3g} mW at the VTX, {header['max_mw']:.3g} mW at the meter", (0, limit),
                    xycoords=("axes fraction", "data"), xytext=(4, 3), textcoords="offset points", fontsize=8.5, color=INK2)
    lo, hi = min(values) / 1.25, max(values) * 1.25
    ax.set_ylim(lo, hi)
    ticks = [t for t in Y_TICKS if lo <= t <= hi]
    ax.set_yticks(ticks)
    ax.set_yticklabels([f"{t:g} mW / {mw_to_dbm(t):.0f} dBm" for t in ticks], fontsize=9, color=INK2)
    ax.minorticks_off()
    ax.set_xticks(list(x.values()))
    ax.set_xticklabels([f"{channel_name(m)}\n{m}" for m in channels], fontsize=9, color=INK2)
    ax.set_xlim(-0.3, top + 0.75)
    ax.set_xlabel("Channel, MHz", fontsize=9, color=INK2)
    ax.grid(axis="y", color=GRID, lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color("#b9b8b2")
    ax.tick_params(length=0)

    note = "Solid: the VTX's output, measured. Dashed: the level's label. Hollow: within 6 dB of the meter's floor."
    if header:
        note = f"{header['attn_db']:g} dB attenuator, added back. " + note
    if left_out:
        note += f" {left_out} unsettled left out."
    fig.suptitle(title, x=0.02, y=0.975, ha="left", fontsize=13, color=INK)
    fig.text(0.02, 0.905, note, fontsize=8.5, color=INK2)
    if len(levels) >= 2:
        ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.17), ncol=len(levels), frameon=False,
                  fontsize=9, labelcolor=INK2)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(path, facecolor=SURFACE)
    plt.close(fig)


def on_signal(_num, _frame):
    raise KeyboardInterrupt


def channels_arg(v):
    return [int(x) for x in v.split(",")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fc", default=find_port("Betaflight", "STM32"), help="flight controller (MSP)")
    ap.add_argument("--meter", default=find_port("ImmersionRC"))
    ap.add_argument("--max-mw", type=float, help="power limit at the meter's input, after the attenuator "
                    "(default 500; with --widscan58 0.316, -5 dBm, and no higher)")
    ap.add_argument("--settle", type=float, help="seconds after a meter frequency change (default 0.7; --quick 0.3)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--attn", type=float, help="dB of attenuator before the meter (default 0; required with "
                        "--widscan58); reported values have it added back")
    common.add_argument("--partial", action="store_true", help="skip table levels too high for the attenuator, instead of an error")
    common.add_argument("--widscan58", action="store_true", help="measure on a widscan58 board instead of the RF power meter")
    common.add_argument("--widscan58-port", default=find_port("Espressif"), help="the widscan58's port")
    output = argparse.ArgumentParser(add_help=False)
    output.add_argument("--title", help=f'table and graph title (default "{DEFAULT_TITLE}")')
    output.add_argument("--output-w", type=int, default=DEFAULT_OUTPUT_W, help=f"graph width in pixels (default {DEFAULT_OUTPUT_W})")
    sub.add_parser("check", parents=[common])
    s = sub.add_parser("sweep", parents=[common, output])
    s.add_argument("-o", "--output", help="output path without extension (default tmp/vtxpower/<date>-<time>)")
    s.add_argument("--channels", type=channels_arg, default=RACEBAND, help="MHz, comma separated (default Raceband)")
    s.add_argument("--quick", action="store_true", help="cut the script's own waiting; noisier readings")
    s.add_argument("--step", type=float, help="seconds per step, at least (default 8; --quick 0)")
    s.add_argument("--measure-at", type=float, help="seconds after the VTX change (default 2; --quick 0.5)")
    s.add_argument("--reads", type=int, help="meter readings per calibration point (default 10; --quick 3)")
    s.add_argument("--stable-timeout", type=float, default=15.0, help="seconds to keep re-reading a step that won't hold still")
    s.add_argument("--ready-timeout", type=float, default=240.0, help="seconds to wait for the VTX to come up")
    s.add_argument("--table-wait", type=float, default=2.0, help="seconds from VTX ready to reading its table")
    s.add_argument("--yes", action="store_true", help="start without asking, once the plan is printed")
    r = sub.add_parser("report", parents=[output])
    r.add_argument("jsonl")
    args = ap.parse_args()
    for name, value in (QUICK_TIMING if getattr(args, "quick", False) else TIMING).items():
        if getattr(args, name, value) is None:  # an option this command has, not given
            setattr(args, name, value)

    if getattr(args, "output_w", 1) <= 0:
        ap.error("--output-w must be above 0")
    if args.cmd == "report":
        report(args.jsonl, args.title, args.output_w)
        return 0
    if args.widscan58:
        if args.attn is None or args.attn <= 0:
            ap.error("--widscan58 has no attenuator of its own: --attn with the attenuator's dB is required")
        input_max = dbm_to_mw(WIDSCAN58_MAX_DBM)
        if args.max_mw is None:
            args.max_mw = input_max
        elif args.max_mw > input_max:
            ap.error(f"--widscan58 reads up to {WIDSCAN58_MAX_DBM:g} dBm: --max-mw can't be over {input_max:.3g}")
    else:
        args.attn = 0.0 if args.attn is None else args.attn
        args.max_mw = 500.0 if args.max_mw is None else args.max_mw
    if args.attn < 0:
        ap.error("--attn can't be negative")
    meter_port = args.widscan58_port if args.widscan58 else args.meter
    if not args.fc or not meter_port:
        ap.error(f"port not found (--fc {args.fc}, meter {meter_port})")
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)
    if args.cmd == "check":
        return cmd_check(args)
    return cmd_sweep(args)


if __name__ == "__main__":
    sys.exit(main())
