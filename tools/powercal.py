#!/usr/bin/env python3
"""Calibrate the C5's channel power (dBFS) against an RF power meter (dBm).

GPL-3.0-or-later.

A VTX, set over Betaflight MSP, is the source; one attenuator chain on the VTX
stays put and only the cable's far end moves between the meter and the C5, so
the meter reads what the C5 gets. Three runs, two connector changes:

  powercal.py vtx                          VTX table and current VTX config (FC check)
  powercal.py meter -o run1.jsonl          sweep channels x power levels on the meter
  powercal.py c5 -o run2.jsonl             the same sweep on the C5, at each gain index
  powercal.py meter -o run3.jsonl          the meter again (VTX drift check)
  powercal.py fit run1.jsonl run2.jsonl run3.jsonl

Each step of a sweep sets the VTX, then measures from --measure-at seconds
after the change, and pads to --step seconds, so the VTX goes through the same
thermal states in every run. Pit mode is measured first on the meter as its
floor (subtracted in the fit).

Meter: ImmersionRC RF Power Meter v2 over USB CDC (V, D, E, F<idx>; lines end
in CR LF, replies end in OK). It is calibrated every 50 MHz from 5600 to 6000;
each step reads the two calibration points around the channel and
interpolates. The F query always answers 35 (firmware 1.0.9), so the
frequency is set before each reading.

C5: MODE SINGLE (LO 8 MHz below the channel, as for any single channel),
WIN <rssi_hz> <search_hz> for a window that holds the whole VTX signal,
METRICS FULL; per gain index, the mean linear RSSI over --frames frames,
the window's noise power and the ADC saturation count.
"""
import argparse
import json
import math
import struct
import sys
import time

import serial

from ttyfix import keep_vmin1
import widscan58cli as w

METER_FREQS = [35, 72, 433, 868, 900, 1200, 2400] + list(range(5600, 6001, 50))  # F<idx>, as the meter answers
RACEBAND = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]


def find_port(*names):
    """The /dev/serial/by-id link whose name holds one of names."""
    import glob
    for p in sorted(glob.glob("/dev/serial/by-id/*")):
        if any(n.lower() in p.lower() for n in names):
            return p
    return None


class Meter:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.5)
        keep_vmin1(self.ser)
        self.ser.reset_input_buffer()

    def cmd(self, c):
        self.ser.write(c.encode() + b"\r\n")
        r = self.ser.read_until(b"OK\r\n").decode(errors="replace").split()
        if not r or r[-1] != "OK":
            raise RuntimeError(f"meter: {c!r} -> {r!r}")
        return r[:-1]

    def version(self):
        return " ".join(self.cmd("V"))

    def set_freq(self, mhz):
        self.cmd(f"F{METER_FREQS.index(mhz)}")

    def read(self, n, settle):
        time.sleep(settle)
        v = [float(self.cmd("D")[0]) for _ in range(n)]
        return mean_db(v), max(v) - min(v)


class Msp:
    """Betaflight MSP v1 over the FC's USB VCP."""
    VTX_CONFIG, SET_VTX_CONFIG, VTXTABLE_POWERLEVEL = 88, 89, 138

    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=1.0)
        keep_vmin1(self.ser)
        self.ser.reset_input_buffer()

    def request(self, cmd, payload=b""):
        body = bytes([len(payload), cmd]) + payload
        ck = 0
        for b in body:
            ck ^= b
        self.ser.write(b"$M<" + body + bytes([ck]))
        while True:
            if self.ser.read_until(b"$M")[-2:] != b"$M":
                raise TimeoutError(f"MSP {cmd}: no reply")
            d, size, rcmd = self.ser.read(3)
            data = self.ser.read(size + 1)
            if rcmd != cmd:
                continue
            if d == ord("!"):
                raise RuntimeError(f"MSP {cmd}: error reply")
            return data[:size]

    def vtx_config(self):
        p = self.request(self.VTX_CONFIG)
        keys = "type band channel power pit freq ready low_power_disarm".split()
        c = dict(zip(keys, struct.unpack_from("<BBBBBHBB", p)))
        if len(p) >= 15:  # BF 4.1+: the VTX table
            c.update(zip("pit_freq table bands channels power_levels".split(), struct.unpack_from("<HBBBB", p, 9)))
        return c

    def power_levels(self):
        out = []
        for i in range(1, self.vtx_config().get("power_levels", 0) + 1):
            p = self.request(self.VTXTABLE_POWERLEVEL, bytes([i]))
            _idx, value, n = struct.unpack_from("<BHB", p)
            out.append(dict(index=i, value=value, label=p[4:4 + n].decode(errors="replace").strip()))
        return out

    def set_vtx(self, mhz, power, pit=False):
        # A frequency above 63 is taken as MHz (band 0); low power disarm off,
        # or a disarmed FC holds the lowest power. Not saved to EEPROM.
        self.request(self.SET_VTX_CONFIG, struct.pack("<HBBB", mhz, power, int(pit), 0))

    def wait_vtx(self, mhz, power, pit, timeout=8.0):
        """Resend until the VTX reports the setting: a SmartAudio VTX still
        busy with a change loses the next one (pit mode right after a
        frequency change)."""
        deadline = time.time() + timeout
        sent = time.time()
        while time.time() < deadline:
            c = self.vtx_config()
            if c["freq"] == mhz and c["power"] == power and bool(c["pit"]) == pit and c["ready"]:
                return c
            if time.time() - sent > 1.0:
                self.set_vtx(mhz, power, pit)
                sent = time.time()
            time.sleep(0.2)
        raise RuntimeError(f"VTX did not take {mhz} MHz power {power} pit {pit}: {c}")


def mean_db(v):
    return 10 * math.log10(sum(10 ** (x / 10) for x in v) / len(v))


def parse_range(s):
    """'0-60:3,70' -> [0, 3, ..., 60, 70]"""
    out = []
    for part in s.split(","):
        step = 1
        if ":" in part:
            part, step = part.split(":")
            step = int(step)
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1, step))
    return out


def steps(args, levels):
    """(channel MHz, power index, pit) for each step; pit mode first on each channel."""
    out = []
    for mhz in args.channels:
        if args.pit:
            out.append((mhz, levels[0]["index"], True))
        out += [(mhz, lv["index"], False) for lv in levels if lv["index"] in args.powers]
    return out


def sweep(args, measure):
    msp = Msp(args.fc)
    levels = msp.power_levels()
    if not args.powers:
        # MAX measured ~32 dBm (1.6 W): +2 dBm through the 30 dB pad.
        args.powers = [lv["index"] for lv in levels if lv["label"] != "MAX"]
    label = {lv["index"]: lv for lv in levels}
    plan = steps(args, levels)
    print(f"{len(plan)} steps, {len(plan) * args.step / 60:.1f} min", file=sys.stderr)
    with open(args.output, "a") as f:
        for i, (mhz, power, pit) in enumerate(plan):
            t0 = time.time()
            # Betaflight drops pit mode that comes with a frequency change.
            msp.set_vtx(mhz, power)
            cfg = msp.wait_vtx(mhz, power, False)
            if pit:
                msp.set_vtx(mhz, power, True)
                cfg = msp.wait_vtx(mhz, power, True)
            time.sleep(max(0.0, t0 + args.measure_at - time.time()))
            rec = dict(run=args.cmd, t=time.time(), mhz=mhz, power=power, pit=pit,
                       label="pit" if pit else label[power]["label"], vtx=cfg)
            rec.update(measure(mhz))
            f.write(json.dumps(rec) + "\n")
            f.flush()
            print(f"[{i + 1}/{len(plan)}] {mhz} {rec['label']:>6} {summary(rec)}", file=sys.stderr)
            if time.time() > t0 + args.step:
                print(f"  step overran: {time.time() - t0:.1f} s", file=sys.stderr)
            time.sleep(max(0.0, t0 + args.step - time.time()))


def summary(rec):
    if "meter" in rec:
        return " ".join(f"{k}:{v['dbm']:.2f}" for k, v in rec["meter"].items())
    return " ".join(f"g{g['gain']}:{g['rssi_db']:.1f}{'*' if g['saturated'] else ''}" for g in rec["c5"][::4])


def cmd_vtx(args):
    msp = Msp(args.fc)
    print(json.dumps(msp.vtx_config()))
    for lv in msp.power_levels():
        print(json.dumps(lv))


def cmd_meter(args):
    meter = Meter(args.meter)
    print("meter", meter.version(), file=sys.stderr)

    def measure(mhz):
        lo = max(f for f in METER_FREQS if f <= mhz)
        pts = sorted({lo, min(lo + 50, 6000)})
        out = {}
        for fm in pts:
            meter.set_freq(fm)
            dbm, spread = meter.read(args.reads, args.settle)
            out[str(fm)] = dict(dbm=dbm, spread=spread)
        return dict(meter=out)

    sweep(args, measure)


def c5_setup(link, args, mhz):
    for line in ["PROTOCOL MONITOR", f"CHANNELS X={mhz}", "MODE SINGLE", "ENABLE 1",
                 f"WIN {args.win_hz} {max(args.win_hz, 12000000)}", "METRICS FULL",
                 "SPEC 0 256 40000000 MAX", "SCAN START"]:
        r = link.command(line, timeout=2.0)
        if r.startswith("ERR"):
            raise RuntimeError(f"C5: {line!r} -> {r}")


def c5_measure(link, gain, frames):
    r = link.command(f"GAIN MANUAL {gain}")
    if r.startswith("ERR"):
        raise RuntimeError(f"C5: GAIN MANUAL {gain} -> {r}")
    link.ser.reset_input_buffer()
    link.buf.clear()
    link.pending = []
    for _ in range(3):  # hops already under way at the old gain
        link.wait_frame(w.FRAME_METRICS)
    rssi, floor, sat, flags = [], [], 0, 0
    while len(rssi) < frames:
        m = w.parse_metrics(link.wait_frame(w.FRAME_METRICS))
        if m["gain"] != gain:
            continue
        c = m["channels"][0]
        rssi.append(c["rssi"] / 100)
        floor.append(c["floor"] / 100)
        sat += m["saturated"]
        flags |= c["flags"]
    # The median as well: Wi-Fi bursts in the window (channel 149 overlaps R3
    # and R4) raise the mean; an analog VTX's level is steady.
    return dict(gain=gain, rssi_db=mean_db(rssi), rssi_med=sorted(rssi)[len(rssi) // 2], rssi_spread=max(rssi) - min(rssi),
                floor_db=mean_db(floor), saturated=sat, flags=flags)


def cmd_c5(args):
    link = w.Link(args.c5)
    print(link.command("INFO", prefix="INFO"), file=sys.stderr)
    state = {"mhz": None}

    def measure(mhz):
        if mhz != state["mhz"]:
            c5_setup(link, args, mhz)
            state["mhz"] = mhz
        return dict(c5=[c5_measure(link, g, args.frames) for g in args.gains])

    sweep(args, measure)


def meter_dbm(rec):
    """Meter reading interpolated to the channel frequency."""
    pts = sorted((int(f), v["dbm"]) for f, v in rec["meter"].items())
    if len(pts) == 1:
        return pts[0][1]
    (f0, a), (f1, b) = pts
    return a + (b - a) * (rec["mhz"] - f0) / (f1 - f0)


def cmd_fit(args):
    meter, c5 = {}, {}
    floors = {}
    for path in args.files:
        for line in open(path):
            rec = json.loads(line)
            key = (rec["mhz"], rec["power"], rec["pit"])
            if "meter" in rec:
                if rec["pit"]:
                    floors.setdefault(rec["mhz"], []).append(meter_dbm(rec))
                meter.setdefault(key, []).append(meter_dbm(rec))
            else:
                c5.setdefault(key, []).append(rec)
    # The meter's own floor (pit mode reads at or near it), subtracted linearly.
    def corrected(mhz, dbm):
        fl = floors.get(mhz)
        if not fl:
            return dbm, None
        fl = mean_db(fl)
        if dbm - fl < args.min_meter_margin:
            return None, fl
        return 10 * math.log10(10 ** (dbm / 10) - 10 ** (fl / 10)), fl
    print("drift (meter runs, max - min per step):",
          f"{max((max(v) - min(v) for v in meter.values() if len(v) > 1), default=0):.2f} dB")
    by_gain = {}
    for key, recs in sorted(c5.items()):
        mhz, power, pit = key
        if pit or key not in meter:
            continue
        dbm, fl = corrected(mhz, mean_db(meter[key]))
        if dbm is None:
            continue
        for rec in recs:
            for g in rec["c5"]:
                if g["saturated"] or g["rssi_db"] - g["floor_db"] < args.min_snr:
                    continue
                by_gain.setdefault(g["gain"], []).append((mhz, dbm - args.pad_db, g["rssi_db"]))
    print(f"{'gain':>4} {'n':>3} {'offset':>7} {'sd':>5} {'slope':>6}  per-channel offsets (dBm - dBFS)")
    for gain, pts in sorted(by_gain.items()):
        off = [d - r for _, d, r in pts]
        mo = sum(off) / len(off)
        sd = math.sqrt(sum((o - mo) ** 2 for o in off) / len(off))
        slope = float("nan")
        if len(pts) > 2:
            xs, ys = [d for _, d, _ in pts], [r for _, _, r in pts]
            mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
            sxx = sum((x - mx) ** 2 for x in xs)
            if sxx > 1:
                slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
        per = {}
        for mhz, d, r in pts:
            per.setdefault(mhz, []).append(d - r)
        perstr = " ".join(f"{m}:{sum(v) / len(v):+.2f}" for m, v in sorted(per.items()))
        print(f"{gain:4} {len(pts):3} {mo:+7.2f} {sd:5.2f} {slope:6.3f}  {perstr}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fc", default=find_port("Betaflight", "STM32"), help="flight controller (MSP)")
    ap.add_argument("--meter", default=find_port("ImmersionRC"))
    ap.add_argument("--c5", default=find_port("Espressif"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("vtx")
    for name in ("meter", "c5"):
        s = sub.add_parser(name)
        s.add_argument("-o", "--output", required=True, help="JSON lines, appended")
        s.add_argument("--channels", type=lambda v: [int(x) for x in v.split(",")], default=RACEBAND)
        s.add_argument("--powers", type=parse_range, default=None, help="VTX table power indices (default: all but MAX)")
        s.add_argument("--no-pit", dest="pit", action="store_false", help="skip the pit mode steps")
        s.add_argument("--step", type=float, default=8.0, help="seconds per step")
        s.add_argument("--measure-at", type=float, default=2.0, help="seconds after the VTX change")
    m = sub.choices["meter"]
    m.add_argument("--reads", type=int, default=10, help="meter readings per calibration point")
    m.add_argument("--settle", type=float, default=0.7, help="seconds after a meter frequency change")
    c = sub.choices["c5"]
    c.add_argument("--gains", type=parse_range, default=parse_range("0-60:3"))
    c.add_argument("--frames", type=int, default=30, help="metrics frames per gain")
    c.add_argument("--win-hz", type=int, default=15000000, help="RSSI window, +/- Hz")
    f = sub.add_parser("fit")
    f.add_argument("files", nargs="+")
    f.add_argument("--pad-db", type=float, default=0.0, help="attenuation between the swap point and the C5 only")
    f.add_argument("--min-snr", type=float, default=10.0, help="dB of C5 RSSI over its window noise")
    f.add_argument("--min-meter-margin", type=float, default=6.0, help="dB of meter reading over its floor")
    args = ap.parse_args()
    {"vtx": cmd_vtx, "meter": cmd_meter, "c5": cmd_c5, "fit": cmd_fit}[args.cmd](args)


if __name__ == "__main__":
    main()
