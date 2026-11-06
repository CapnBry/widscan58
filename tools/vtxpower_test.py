#!/usr/bin/env python3
"""vtxpower.py against a simulated FC and meter, speaking the real wire protocols over ptys.

GPL-3.0-or-later.

  python3 tools/vtxpower_test.py        (no hardware needed)

The simulated VTX's power follows what the FC is told and the meter's reading
follows the VTX, so each test checks what the VTX was actually commanded to do.
"""
import csv
import math
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
METER_FREQS = [35, 72, 433, 868, 900, 1200, 2400] + list(range(5600, 6001, 50))
STARTUP = [("25", 14), ("100", 20)]
FULL = [("25", 14), ("100", 20), ("400", 26)]
WITH_MAX = FULL + [("MAX", 32)]
RACEBAND = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]


class Pty:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)


class Sim:
    """FC + VTX + meter. true_mw(index, mhz) is what the VTX really puts out."""

    def __init__(self, on_table=FULL, off_table=STARTUP, power_up_after=0.3, true_mw=None, atten=0.0,
                 floor=-24.0, start_power=1, band=5, channel=8, freq=5917, pit=0):
        self.off_table, self.on_table = off_table, on_table
        self.power_up_after = power_up_after
        self.true_mw = true_mw or (lambda index, mhz: float(self.table()[index - 1][0]) * 0.8 if
                                   self.table()[index - 1][0].isdigit() else 1600.0)
        self.atten, self.floor = atten, floor
        self.power, self.band, self.channel, self.freq, self.pit = start_power, band, channel, freq, pit
        self.actual = start_power  # what the VTX does; power is what the FC reports
        self.stuck_at = None  # the VTX stops taking levels once told this one
        self.t0 = time.time()
        self.fc, self.meter = Pty(), Pty()
        self.sets = []  # (freq, power index) as commanded
        self.peak_mw = 0.0
        self.cable_off = False
        self.meter_dead_after_reads = None
        self.dropout_reads = 0  # the first step's first readings come in 30 dB low
        self.flaky_level = None  # this level's readings alternate, forever
        self.meter_reads = 0
        self.table_override = None
        self.stop = False
        self.fc_thread = threading.Thread(target=self.run_fc, daemon=True)
        self.meter_thread = threading.Thread(target=self.run_meter, daemon=True)
        self.fc_thread.start()
        self.meter_thread.start()

    def vtx_on(self):
        return self.power_up_after is not None and time.time() - self.t0 >= self.power_up_after

    def table(self):
        if self.table_override:
            return self.table_override
        return self.on_table if self.vtx_on() else self.off_table

    def output_mw(self):
        return self.true_mw(self.actual, self.freq) if self.vtx_on() else 0.0

    def run_fc(self):
        buf = bytearray()
        while not self.stop:
            r, _, _ = select.select([self.fc.master], [], [], 0.05)
            if not r:
                continue
            buf += os.read(self.fc.master, 256)
            while True:
                i = buf.find(b"$M<")
                if i < 0 or len(buf) < i + 6 or len(buf) < i + 6 + buf[i + 3]:
                    break
                size, cmd = buf[i + 3], buf[i + 4]
                payload = bytes(buf[i + 5:i + 5 + size])
                del buf[:i + 6 + size]
                self.reply(cmd, self.handle(cmd, payload))

    def handle(self, cmd, p):
        if cmd == 88:  # MSP_VTX_CONFIG: settings, as Betaflight reports them
            t = self.table()
            return struct.pack("<BBBBBHBB", 3, self.band, self.channel, self.power, self.pit, self.freq, int(self.vtx_on()), 0) + \
                struct.pack("<HBBBB", 0, 1, 6, 8, len(t))
        if cmd == 89:  # MSP_SET_VTX_CONFIG
            freq, power, pit, _lpd = struct.unpack("<HBBB", p)
            if freq <= 63:  # band and channel; only Raceband here
                self.band, self.channel = freq // 8 + 1, freq % 8 + 1
                assert self.band == 5, self.band
                freq = RACEBAND[self.channel - 1]
            else:
                self.band = 0
            self.freq, self.power, self.pit = freq, power, pit
            if self.actual != self.stuck_at:
                self.actual = power
            self.sets.append((freq, power))
            self.peak_mw = max(self.peak_mw, self.output_mw())
            return b""
        if cmd == 138:  # MSP_VTXTABLE_POWERLEVEL
            label, value = self.table()[p[0] - 1]
            return struct.pack("<BHB", p[0], value, len(label)) + label.encode()
        raise AssertionError(f"unexpected MSP {cmd}")

    def reply(self, cmd, data):
        body = bytes([len(data), cmd]) + data
        ck = 0
        for b in body:
            ck ^= b
        os.write(self.fc.master, b"$M>" + body + bytes([ck]))

    def run_meter(self):
        buf = b""
        while not self.stop:
            r, _, _ = select.select([self.meter.master], [], [], 0.05)
            if not r:
                continue
            buf += os.read(self.meter.master, 256)
            while b"\r\n" in buf:
                line, buf = buf.split(b"\r\n", 1)
                self.meter_cmd(line.decode())

    def meter_cmd(self, c):
        if c == "V":
            out = "RFPowerMeterv2\r\n1.0.9\r\n"
        elif c.startswith("F"):
            out = ""
        elif c == "D":
            self.meter_reads += 1
            if self.meter_dead_after_reads is not None and self.meter_reads > self.meter_dead_after_reads:
                return  # no answer
            mw = self.output_mw()
            if self.dropout_reads and len(self.sets) >= 3:
                self.dropout_reads -= 1
                mw *= 0.001
            if self.power == self.flaky_level and self.meter_reads % 2:
                mw *= 0.001
            sig = 0.0 if self.cable_off or mw <= 0 else 10 ** ((10 * math.log10(mw) - self.atten) / 10)
            out = f"{10 * math.log10(sig + 10 ** (self.floor / 10)):.6f}\r\n"
        else:
            raise AssertionError(f"unexpected meter command {c!r}")
        os.write(self.meter.master, (out + "OK\r\n").encode())

    def close(self):
        self.stop = True


FAST = ["--step", "0.02", "--measure-at", "0.01", "--reads", "2", "--ready-timeout", "3", "--table-wait", "0.2"]


class SweepTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.out = Path(self.dir.name) / "run"

    def start(self, sim, *extra, attn="0", settle="0.02"):
        self.addCleanup(sim.close)
        cmd = [sys.executable, str(HERE / "vtxpower.py"), "--fc", sim.fc.path, "--meter", sim.meter.path,
               "--settle", settle, "sweep", "--attn", attn, "-o", str(self.out), "--yes", *FAST, *extra]
        return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, text=True)

    def sweep(self, sim, *extra, **kw):
        p = self.start(sim, *extra, **kw)
        stdout, stderr = p.communicate(timeout=120)
        return p.returncode, stdout, stderr

    def wait_for(self, p, condition, timeout=20.0):
        deadline = time.time() + timeout
        while not condition():
            if p.poll() is not None or time.time() > deadline:
                p.kill()
                self.fail(f"condition not reached: {p.communicate()[1]}")
            time.sleep(0.005)

    def assert_left(self, sim, band=5, channel=8, freq=5917, power=1):
        """Where the VTX has to end up on any exit."""
        self.assertEqual((sim.band, sim.channel, sim.freq, sim.power, sim.pit), (band, channel, freq, power, 0))

    def assert_only_levels(self, sim, allowed):
        self.assertTrue(sim.sets, "nothing was commanded")
        used = {power for _, power in sim.sets}
        self.assertLessEqual(used, allowed, f"commanded {sorted(used)}, allowed {sorted(allowed)}")

    def rows(self):
        with open(self.out.with_suffix(".csv")) as f:
            return list(csv.DictReader(f))

    def test_full_sweep(self):
        """The table grows at power-up; every output is written; back on the starting channel."""
        sim = Sim()
        code, stdout, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3})
        self.assert_left(sim)
        self.assertLessEqual(sim.peak_mw, 500)
        for ch in RACEBAND:
            self.assertEqual({p for f, p in sim.sets if f == ch}, {1, 2, 3}, ch)
        rows = self.rows()
        self.assertEqual(len(rows), 24)
        self.assertEqual({r["channel"] for r in rows}, {f"R{i}" for i in range(1, 9)})
        self.assertEqual({r["level"] for r in rows}, {"25", "100", "400"})
        for r in rows:
            nominal = float(r["nominal_mw"]) * 0.8
            self.assertAlmostEqual(float(r["mw"]), nominal, delta=nominal * 0.05, msg=str(r))
        self.assertGreater(self.out.with_suffix(".png").stat().st_size, 10000)
        self.assertIn("| R8 | 5917 |", self.out.with_suffix(".md").read_text())
        self.assertIn("| R8 | 5917 |", stdout)

    def test_too_high_is_an_error(self):
        """The table grows past the limit at power-up: error, nothing swept, VTX left low."""
        sim = Sim(on_table=WITH_MAX)
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1, stderr)
        self.assertIn("too high", stderr)
        self.assertIn("'MAX'", stderr)
        self.assertIn("--partial", stderr)
        self.assert_only_levels(sim, {1})
        self.assert_left(sim)
        self.assertFalse(self.out.with_suffix(".jsonl").exists())

    def test_too_high_before_power_up(self):
        """Already too high with the VTX off: error, and the FC isn't touched."""
        table = [("25", 14), ("800", 29)]
        sim = Sim(off_table=table, on_table=table, power_up_after=None)
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1, stderr)
        self.assertEqual(sim.sets, [])
        self.assertIn("needs 2.0 dB", stderr)

    def test_partial_skips(self):
        """--partial: the same table sweeps, without the level that's too high."""
        sim = Sim(on_table=WITH_MAX)
        code, _, stderr = self.sweep(sim, "--partial")
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3})
        self.assertLessEqual(sim.peak_mw, 500)
        self.assertEqual(len(self.rows()), 24)
        self.assertIn("skipping 1 level", stderr)
        self.assert_left(sim)

    def test_partial_labels(self):
        """--partial: over the limit, or anything that doesn't read as a power, is never sent. RCE is."""
        table = [("25", 14), ("RCE", 0), ("200mW", 23), ("0.4W", 26), ("600", 28), ("1W", 30), ("1.6W", 32), ("MAX", 32)]
        sim = Sim(on_table=table, true_mw=lambda i, f: {1: 20, 2: 20, 3: 150, 4: 300}.get(i, 3000.0))
        code, _, stderr = self.sweep(sim, "--partial")
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3, 4})
        self.assertLessEqual(sim.peak_mw, 500)

    def test_default_limit(self):
        """The default limit is 500 mW at the meter: a 500 label is in, 600 is out."""
        sim = Sim(on_table=[("25", 14), ("500", 27), ("600", 28)])
        code, _, stderr = self.sweep(sim, "--partial")
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2})

    def test_quick(self):
        """--quick needs none of the timing options, and still measures every level."""
        sim = Sim()
        self.addCleanup(sim.close)
        cmd = [sys.executable, str(HERE / "vtxpower.py"), "--fc", sim.fc.path, "--meter", sim.meter.path, "sweep", "--quick",
               "--channels", "5658", "--table-wait", "0.2", "-o", str(self.out), "--yes"]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual({r["level"] for r in self.rows()}, {"25", "100", "400"})
        self.assert_left(sim)

    def test_dropout_is_read_again(self):
        """Readings still ramping at the first step are read again, not averaged into the table."""
        sim = Sim()
        sim.dropout_reads = 6
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        self.assertEqual(sim.dropout_reads, 0)
        row = self.rows()[0]
        self.assertEqual((row["channel"], row["level"], row["unsettled"]), ("R1", "25", "0"))
        self.assertAlmostEqual(float(row["mw"]), 20, delta=1)

    def test_never_settles(self):
        """A level that won't hold still: kept and marked, off the graph, and the sweep goes on."""
        sim = Sim()
        sim.flaky_level = 2
        code, stdout, stderr = self.sweep(sim, "--channels", "5658,5695", "--stable-timeout", "0.1")
        self.assertEqual(code, 0, stderr)
        self.assertEqual({r["level"]: r["unsettled"] for r in self.rows()}, {"25": "0", "100": "1", "400": "0"})
        self.assertIn("NOT SETTLED", stderr)
        self.assertIn("†", stdout)
        self.assertGreater(self.out.with_suffix(".png").stat().st_size, 10000)

    def test_lower_limit(self):
        sim = Sim(on_table=FULL)
        self.addCleanup(sim.close)
        cmd = [sys.executable, str(HERE / "vtxpower.py"), "--fc", sim.fc.path, "--meter", sim.meter.path, "--settle", "0.02",
               "--max-mw", "100", "sweep", "--partial", "-o", str(self.out), "--yes", *FAST]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assert_only_levels(sim, {1, 2})

    def test_no_allowed_level(self):
        """Nothing usable in the table: no level is ever commanded, with or without --partial."""
        for extra in ([], ["--partial"]):
            with self.subTest(extra=extra):
                sim = Sim(off_table=[("MAX", 32)], on_table=[("MAX", 32)])
                out = Path(self.dir.name) / f"n{len(extra)}"
                code, _, stderr = self.sweep(sim, "-o", str(out), *extra)
                self.assertEqual(code, 1)
                self.assertEqual(sim.sets, [], stderr)
                self.assertIn("'MAX'", stderr)

    def test_attenuator_opens_the_table(self):
        """A 2 W table through 10 dB: all levels run, values reported at the VTX."""
        table = [("25", 14), ("400", 26), ("1W", 30), ("2W", 33)]
        watts = {1: 25, 2: 400, 3: 1000, 4: 2000}
        sim = Sim(on_table=table, atten=10, true_mw=lambda i, f: watts[i] * 0.8)
        code, stdout, stderr = self.sweep(sim, attn="10")
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3, 4})
        for r in self.rows():
            want = float(r["nominal_mw"]) * 0.8
            self.assertAlmostEqual(float(r["mw"]), want, delta=want * 0.05, msg=str(r))
            self.assertAlmostEqual(float(r["meter_dbm"]), float(r["dbm"]) - 10, delta=0.05, msg=str(r))
        self.assert_left(sim)
        self.assertIn("1600 mW", stdout)

    def test_attenuator_too_small(self):
        """The same table through 5 dB: 2 W is 632 mW at the meter. Error; --partial skips it alone."""
        table = [("25", 14), ("400", 26), ("1W", 30), ("2W", 33)]
        sim = Sim(on_table=table, atten=5, true_mw=lambda i, f: {1: 20, 2: 300, 3: 800, 4: 1600}[i])
        code, _, stderr = self.sweep(sim, attn="5")
        self.assertEqual(code, 1, stderr)
        self.assertIn("needs 6.0 dB", stderr)
        self.assert_only_levels(sim, {1})
        out = Path(self.dir.name) / "partial"
        sim = Sim(on_table=table, atten=5, true_mw=lambda i, f: {1: 20, 2: 300, 3: 800, 4: 1600}[i])
        code, _, stderr = self.sweep(sim, "-o", str(out), "--partial", attn="5")
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3})

    def test_limit_is_at_the_meter(self):
        """The meter's reading decides, whatever --attn says: 3 W out through 10 dB is fine, and
        a real 0 dB under a claimed 30 dB still trips at 500 mW."""
        sim = Sim(on_table=[("25", 14), ("1W", 30)], atten=10, true_mw=lambda i, f: {1: 20, 2: 3000}[i])
        code, _, stderr = self.sweep(sim, attn="10")
        self.assertEqual(code, 0, stderr)
        self.assertEqual(self.rows()[0]["over_limit"], "0")
        self.assertNotIn("over the limit", stderr)
        out = Path(self.dir.name) / "claimed"
        sim = Sim(on_table=[("25", 14), ("1W", 30)], atten=0, true_mw=lambda i, f: {1: 20, 2: 550}[i])
        code, _, stderr = self.sweep(sim, "-o", str(out), attn="30")
        self.assertEqual(code, 0, stderr)
        self.assertIn("over the limit", stderr)
        self.assertEqual(sum(1 for _, p in sim.sets if p == 2), 1)

    def test_weak_level_through_big_attenuator(self):
        """A low level below the meter's floor through 40 dB isn't a lost signal."""
        table = [("25", 14), ("2W", 33)]
        sim = Sim(on_table=table, atten=40, true_mw=lambda i, f: {1: 20, 2: 1600}[i])
        code, _, stderr = self.sweep(sim, attn="40")
        self.assertEqual(code, 0, stderr)
        self.assertNotIn("no signal", stderr)

    def test_rce(self):
        """RCE counts as 25 mW: no error, swept first, and the lowest level to leave the VTX on."""
        sim = Sim(off_table=[("RCE", 0), ("100", 20)], on_table=[("RCE", 0), ("100", 20), ("400", 26)],
                  true_mw=lambda i, f: {1: 20, 2: 80, 3: 320}[i])
        code, stdout, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3})
        self.assert_left(sim)
        self.assertIn("| Channel | MHz | RCE | 100 mW | 400 mW |", stdout)

    def test_leaves_start_channel(self):
        """Back where it started, set by band and channel or by frequency alone, out of pit mode."""
        for kw in (dict(band=5, channel=3, freq=5732), dict(band=5, channel=8, freq=5917, pit=1),
                   dict(band=0, channel=8, freq=5800)):
            with self.subTest(**kw):
                sim = Sim(start_power=3, **kw)
                out = Path(self.dir.name) / f"s{kw['freq']}"
                code, _, stderr = self.sweep(sim, "-o", str(out))
                self.assertEqual(code, 0, stderr)
                self.assert_left(sim, kw["band"], kw["channel"], kw["freq"])

    def test_over_limit_reading(self):
        """A mislabelled level: dropped within the step and never sent again; lower ones finish."""
        sim = Sim(true_mw=lambda i, f: {1: 20, 2: 80, 3: 550}.get(i, 3000.0))
        code, stdout, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2, 3})
        self.assertEqual(sum(1 for _, p in sim.sets if p == 3), 1, "the 400 level must only be tried once")
        i = sim.sets.index((RACEBAND[0], 3))
        self.assertEqual(sim.sets[i + 1], (RACEBAND[0], 1), "must drop straight to the lowest level")
        for ch in RACEBAND:
            self.assertIn((ch, 2), sim.sets)
        self.assertEqual(len([r for r in self.rows() if r["over_limit"] == "1"]), 1)
        self.assertIn("over limit", stdout)

    def test_higher_level_also_blocked(self):
        """After a level reads over, levels above it stay off too."""
        table = [("25", 14), ("200", 23), ("300", 25), ("400", 26)]
        sim = Sim(on_table=table, true_mw=lambda i, f: {1: 20, 2: 550, 3: 600, 4: 700}[i])
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        self.assert_only_levels(sim, {1, 2})

    def test_lowest_over_limit(self):
        """The lowest level already over the limit at the meter: abort."""
        sim = Sim(true_mw=lambda i, f: 550.0)
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1)
        self.assert_left(sim)
        self.assert_only_levels(sim, {1})
        self.assertIn("ABORT", stderr)

    def test_drop_ignored(self):
        """The FC reports the drop but the VTX stays high: the meter catches it, and the script says so."""
        sim = Sim(true_mw=lambda i, f: {1: 20, 2: 80, 3: 550}.get(i, 3000.0))
        sim.stuck_at = 3
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1, stderr)
        self.assertEqual(sim.power, 1)
        self.assertIn("POWER THE VTX OFF", stderr)
        self.assertNotIn("VTX left at", stderr)

    def test_power_up_during_floor(self):
        """A VTX switched on during the floor measurement doesn't become the floor."""
        sim = Sim(power_up_after=0.05)
        sim.true_mw = lambda i, f: {1: 20, 2: 80, 3: 320}.get(i, 3000.0)
        code, _, stderr = self.sweep(sim, settle="0.1")
        self.assertEqual(code, 0, stderr)
        self.assertIn("came up during the floor", stderr)
        for r in self.rows():
            self.assertEqual(float(r["floor_dbm"]), -24.0)

    def test_second_ctrl_c(self):
        """Ctrl-C twice, the second during the final drop: still ends low."""
        sim = Sim(true_mw=lambda i, f: {1: 20, 2: 80, 3: 550}.get(i, 3000.0))
        sim.stuck_at = 3  # holds the script in the final confirmation
        p = self.start(sim, "--step", "0.2")
        self.wait_for(p, lambda: (RACEBAND[0], 3) in sim.sets and sim.power == 1)
        p.send_signal(signal.SIGINT)
        time.sleep(0.1)
        p.send_signal(signal.SIGINT)
        _, stderr = p.communicate(timeout=30)
        self.assertEqual(p.returncode, 1, stderr)
        self.assertEqual(sim.power, 1)
        self.assertIn("POWER THE VTX OFF", stderr)

    def test_no_signal(self):
        """Cable off: abort at the first step, nothing above the lowest level."""
        sim = Sim()
        sim.cable_off = True
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1)
        self.assert_only_levels(sim, {1})
        self.assertIn("no signal", stderr)

    def test_cable_falls_off(self):
        """Cable off mid-sweep: abort, at the lowest level, no further climbing."""
        sim = Sim()
        p = self.start(sim, "--step", "0.15", settle="0.02")
        self.wait_for(p, lambda: len(sim.sets) >= 6)
        sim.cable_off = True
        _, stderr = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 1, stderr)
        self.assert_left(sim)
        self.assertIn("no signal", stderr)
        self.assertLess(len(sim.sets), 24)

    def test_table_change(self):
        """The table changes mid-sweep (an index could now mean more): abort before sending it."""
        sim = Sim()
        p = self.start(sim, "--step", "0.15")
        self.wait_for(p, lambda: len(sim.sets) >= 6)
        n = len(sim.sets)
        sim.table_override = [("25", 14), ("100", 20), ("1W", 30), ("MAX", 32)]
        _, stderr = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 1, stderr)
        self.assertIn("table changed", stderr)
        self.assert_left(sim)
        self.assertLessEqual(len(sim.sets), n + 2)

    def test_ctrl_c(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(sig=sig.name):
                sim = Sim()
                out = Path(self.dir.name) / sig.name
                self.addCleanup(sim.close)
                cmd = [sys.executable, str(HERE / "vtxpower.py"), "--fc", sim.fc.path, "--meter", sim.meter.path,
                       "--settle", "0.02", "sweep", "-o", str(out), "--yes", *FAST, "--step", "0.2"]
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, text=True)
                self.wait_for(p, lambda: any(pw == 3 for _, pw in sim.sets))
                p.send_signal(sig)
                _, stderr = p.communicate(timeout=30)
                self.assertEqual(p.returncode, 130, stderr)
                self.assert_left(sim)
                self.assertIn("VTX left at", stderr)
                self.assertTrue(out.with_suffix(".md").exists() or out.with_suffix(".jsonl").exists())

    def test_meter_dies(self):
        """The meter stops answering mid-sweep (auto shutdown): abort, at the lowest level."""
        sim = Sim()
        sim.meter_dead_after_reads = 80
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1, stderr)
        self.assert_left(sim)
        self.assertIn("meter", stderr)

    def test_vtx_never_ready(self):
        """No VTX: gives up without raising the level, after the floor measurement."""
        sim = Sim(power_up_after=None)
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 1, stderr)
        self.assert_only_levels(sim, {1})
        self.assertIn("not ready", stderr)
        self.assertFalse(self.out.with_suffix(".jsonl").exists(), "an empty run leaves no files")

    def test_starts_at_high_setting(self):
        """The FC was left at a high index: lowered before the VTX comes up."""
        sim = Sim(start_power=2)
        p = self.start(sim)
        p.communicate(timeout=120)
        self.assertEqual(sim.sets[0], (5917, 1))

    def test_refuses_existing_output(self):
        self.out.with_suffix(".jsonl").write_text("{}\n")
        sim = Sim()
        code, _, stderr = self.sweep(sim)
        self.assertNotEqual(code, 0)
        self.assertEqual(sim.sets, [])
        self.assertIn("exists", stderr)

    def test_report_again(self):
        sim = Sim()
        code, _, stderr = self.sweep(sim)
        self.assertEqual(code, 0, stderr)
        before = self.out.with_suffix(".md").read_text()
        self.out.with_suffix(".md").unlink()
        self.out.with_suffix(".png").unlink()
        p = subprocess.run([sys.executable, str(HERE / "vtxpower.py"), "report", str(self.out.with_suffix(".jsonl"))],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.out.with_suffix(".md").read_text(), before)
        self.assertTrue(self.out.with_suffix(".png").exists())


class UnitTest(unittest.TestCase):
    def test_send_refuses_a_level_off_the_list(self):
        """The last check before the FC, whatever a bug upstream asks for."""
        from argparse import Namespace

        from vtxpower import SafetyAbort, Vtx

        class Msp:
            sent = []
            table = [dict(index=1, value=14, label="25"), dict(index=2, value=32, label="MAX")]

            def power_levels(self):
                return self.table

            def set_vtx(self, *a):
                self.sent.append(a)

        msp = Msp()
        vtx = Vtx(msp, Namespace(max_mw=400, attn=0, partial=True), None)
        vtx.adopt_table(msp.table)
        with self.assertRaises(SafetyAbort):
            vtx.send(5658, 2, wait=False)
        self.assertEqual(msp.sent, [])
        vtx.send(5658, 1, wait=False)
        self.assertEqual(msp.sent, [(5658, 1)])

    def test_label_mw(self):
        from vtxpower import label_mw
        for label, mw in [("25", 25), ("400 ", 400), (" 25mW", 25), ("1W", 1000), ("1.6 w", 1600), ("0.4W", 400), ("RCE", 25), ("rce ", 25)]:
            self.assertEqual(label_mw(label), mw, label)
        for label in ["MAX", "", "PIT", "26dBm", "0", "-1", "400mW+"]:
            self.assertIsNone(label_mw(label), label)


if __name__ == "__main__":
    unittest.main(verbosity=2)
