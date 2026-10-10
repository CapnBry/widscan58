#!/usr/bin/env python3
"""Host client for Widscan58 firmware (protocol v1, see firmware/src/proto/frames.h).

GPL-3.0-or-later.

  widscan58cli.py info                     INFO / PROFILE? / STATUS?
  widscan58cli.py cmd "GAIN MANUAL 50"     send raw command lines, print replies
  widscan58cli.py monitor [-s SECONDS]     live per-channel table + telemetry
  widscan58cli.py rawiq LO_KHZ N [-r RATE] [-o file.npy] [--plot]
  widscan58cli.py spectrum [-s SECONDS] [--plot out.png]   stitched max-hold of SPECTRUM frames
  widscan58cli.py bootloader               reboot into ROM download mode
"""
import argparse
import json
import struct
import sys
import time
import zlib

import serial

from ttyfix import keep_vmin1

FRAME_METRICS, FRAME_SPECTRUM, FRAME_TELEMETRY, FRAME_RAWIQ, FRAME_LOG, FRAME_LEVELS = 1, 2, 3, 4, 5, 6
METRICS_HDR = struct.Struct("<IIIIIIHhhBBBB")
CHANNEL = struct.Struct("<BBhhhhhhiihh")
LEVELS_HDR = struct.Struct("<IIIIIIHBBBBBB")
LEVEL = struct.Struct("<BBBB")
SPECTRUM_HDR = struct.Struct("<IIiIHBB")
TELEMETRY = struct.Struct("<IIIIIIIIIIIHIIIBBBb")
RAWIQ_HDR = struct.Struct("<IIHBB")
CHF = {1: "SIG", 2: "SAT", 4: "DC", 8: "EDGE", 16: "NOC", 32: "SPUR"}


def flags_str(f):
    return ",".join(n for b, n in CHF.items() if f & b) or "-"


class Link:
    """Splits the device stream into text lines and CRC-checked frames."""

    def __init__(self, port, timeout=0.05):
        self.ser = serial.Serial(port, 115200, timeout=timeout)
        keep_vmin1(self.ser)
        self.buf = bytearray()
        self.crc_errors = 0

    def send(self, line):
        self.ser.write(line.encode() + b"\n")

    def poll(self):
        """Yield ('text', str) and ('frame', type, payload) items."""
        self.buf += self.ser.read(65536)
        out = []
        while self.buf:
            if self.buf[0] == 0xA5:
                if len(self.buf) < 6:
                    break
                if self.buf[1] != ord("R"):
                    del self.buf[0]
                    continue
                ftype, _ver, length = self.buf[2], self.buf[3], struct.unpack_from("<H", self.buf, 4)[0]
                total = 6 + length + 4
                if len(self.buf) < total:
                    break
                crc = struct.unpack_from("<I", self.buf, 6 + length)[0]
                if zlib.crc32(bytes(self.buf[: 6 + length])) != crc:
                    self.crc_errors += 1
                    del self.buf[0]
                    continue
                out.append(("frame", ftype, bytes(self.buf[6 : 6 + length])))
                del self.buf[:total]
            else:
                nl = self.buf.find(b"\n")
                sync = self.buf.find(b"\xa5")
                if nl < 0 or (0 <= sync < nl):
                    if sync > 0:
                        out.append(("text", self.buf[:sync].decode(errors="replace")))
                        del self.buf[:sync]
                        continue
                    break
                out.append(("text", self.buf[:nl].decode(errors="replace")))
                del self.buf[: nl + 1]
        return out

    def command(self, line, prefix=None, timeout=2.0):
        """Send a command and return the first reply line (optionally with prefix)."""
        self.send(line)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for item in self.poll():
                if item[0] == "text" and (prefix is None or item[1].startswith(prefix)):
                    if prefix is None and not (item[1].startswith(("OK", "ERR"))):
                        continue
                    return item[1]
        raise TimeoutError(f"no reply to {line!r}")

    def wait_frame(self, ftype, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for item in self.poll():
                if item[0] == "frame" and item[1] == ftype:
                    return item[2]
                if item[0] == "text" and item[1].startswith("ERR"):
                    raise RuntimeError(item[1])
        raise TimeoutError(f"no frame type {ftype}")


def parse_metrics(p):
    h = METRICS_HDR.unpack_from(p)
    keys = ("seq t_ms lo_khz tune_us capture_us dsp_us saturated floor_bin_cdb dc_cdb gain "
            "hop_index hops nch").split()
    m = dict(zip(keys, h))
    chans = []
    for j in range(m["nch"]):
        c = CHANNEL.unpack_from(p, METRICS_HDR.size + j * CHANNEL.size)
        chans.append(dict(zip("id flags rssi ema floor snr clear peak peak_off centroid_off obw_lo_khz obw_hi_khz".split(), c)))
    m["channels"] = chans
    return m


def parse_levels(p):
    """FRAME_LEVELS (METRICS RSSI|SNR) in parse_metrics' shape: centi-dB, None where not measured."""
    h = LEVELS_HDR.unpack_from(p)
    keys = "seq t_ms lo_khz tune_us capture_us dsp_us saturated gain hop_index hops mode floor_bin nch".split()
    m = dict(zip(keys, h))
    snr_mode = m["mode"] == 2
    m["floor_bin_cdb"] = (m["floor_bin"] * 50 - 12750) if snr_mode else None
    m["dc_cdb"] = None
    chans = []
    for j in range(m["nch"]):
        cid, flags, rssi, snr = LEVEL.unpack_from(p, LEVELS_HDR.size + j * LEVEL.size)
        rssi_cdb = rssi * 50 - 12750
        c = dict.fromkeys("ema clear peak peak_off centroid_off obw_lo_khz obw_hi_khz".split())
        c.update(id=cid, flags=flags, rssi=rssi_cdb, snr=snr * 50 if snr_mode else None,
                 floor=rssi_cdb - snr * 50 if snr_mode else None)
        chans.append(c)
    m["channels"] = chans
    return m


def parse_spectrum(p):
    seq, lo_khz, start_off, bin_hz, n, det, _ = SPECTRUM_HDR.unpack_from(p)
    codes = p[SPECTRUM_HDR.size : SPECTRUM_HDR.size + n]
    return dict(seq=seq, lo_khz=lo_khz, start_off_hz=start_off, bin_hz=bin_hz, det=det,
                db=[c / 2 - 127.5 for c in codes])


def parse_telemetry(p):
    keys = ("t_ms rotations hops_total rotation_mhz hop_avg hop_max tune_avg tune_max cap_avg "
            "dsp_avg dsp_max link_fill heap drops cap_errors running recal gain filter").split()
    return dict(zip(keys, TELEMETRY.unpack_from(p)))


def get_profile(link):
    line = link.command("PROFILE?", prefix="PROFILE ")
    return json.loads(line[len("PROFILE "):])


def cmd_info(link, args):
    print(link.command("INFO", prefix="INFO"))
    print(json.dumps(get_profile(link), indent=1))
    print(link.command("STATUS?", prefix="STATUS"))


def cmd_raw(link, args):
    for line in args.lines:
        link.send(line)
        deadline = time.time() + 1.0
        while time.time() < deadline:
            for item in link.poll():
                if item[0] == "text":
                    print(item[1])


def cmd_monitor(link, args):
    prof = get_profile(link)
    names = {c["id"]: c["name"] for c in prof["channels"]}
    khz = {c["id"]: c["khz"] for c in prof["channels"]}
    latest, tel = {}, None
    end = time.time() + args.seconds if args.seconds else None
    last_draw = 0
    while end is None or time.time() < end:
        for item in link.poll():
            if item[0] == "frame" and item[1] in (FRAME_METRICS, FRAME_LEVELS):
                m = (parse_metrics if item[1] == FRAME_METRICS else parse_levels)(item[2])
                for c in m["channels"]:
                    latest[c["id"]] = (m, c)
            elif item[0] == "frame" and item[1] == FRAME_TELEMETRY:
                tel = parse_telemetry(item[2])
            elif item[0] == "text":
                print(item[1])
        if time.time() - last_draw > 0.5:
            last_draw = time.time()
            lines = ["\x1b[2J\x1b[H" if not args.plain else "",
                     f"{'ch':4}{'MHz':>10}{'LO':>11}{'RSSI':>8}{'EMA':>8}{'floor':>8}{'SNR':>7}{'clear':>8}"
                     f"{'peak':>8}{'occ kHz':>9}{'cen kHz':>9}{'OBW99 kHz':>11} flags"]
            # Blank: not measured in this metrics mode (FRAME_LEVELS).
            def f(v, w, scale=100, prec=1):
                return f"{v / scale:{w}.{prec}f}" if v is not None else " " * w
            for cid in sorted(latest):
                m, c = latest[cid]
                clear = None if c["clear"] is None else c["clear"] if c["clear"] > -32768 else float("nan")
                obw = None if c["obw_lo_khz"] is None else c["obw_hi_khz"] - c["obw_lo_khz"]
                lines.append(f"{names[cid]:4}{khz[cid] / 1000:10.3f}{m['lo_khz'] / 1000:11.3f}{f(c['rssi'], 8)}"
                             f"{f(c['ema'], 8)}{f(c['floor'], 8)}{f(c['snr'], 7)}{f(clear, 8)}"
                             f"{f(c['peak'], 8)}{f(c['peak_off'], 9, 1000, 0)}{f(c['centroid_off'], 9, 1000, 0)}"
                             f"{f(obw, 11, 1, 0)} {flags_str(c['flags'])}")
            if latest:
                m = next(iter(latest.values()))[0]
                lines.append(f"floor/bin {f(m['floor_bin_cdb'], 0)} dBFS  DC {f(m['dc_cdb'], 0)} dBFS  "
                             f"gain {m['gain']}  sat {m['saturated']}")
            if tel:
                lines.append(f"rot {tel['rotation_mhz'] / 1000:.1f}/s  hop avg {tel['hop_avg']} max {tel['hop_max']} us  "
                             f"tune {tel['tune_avg']}/{tel['tune_max']}  cap {tel['cap_avg']}  dsp {tel['dsp_avg']}/"
                             f"{tel['dsp_max']} us  link {tel['link_fill'] / 10:.0f}%  drops {tel['drops']}  "
                             f"caperr {tel['cap_errors']}  heap {tel['heap']}  crcerr {link.crc_errors}")
            print("\n".join(lines), flush=True)


def cmd_rawiq(link, args):
    import numpy as np
    cmd = f"RAWIQ {args.lo_khz} {args.n}" + (f" {args.rate}" if args.rate is not None else "")
    link.send(cmd)
    p = link.wait_frame(FRAME_RAWIQ, timeout=5)
    lo, rate, n, conj, shift = RAWIQ_HDR.unpack_from(p)
    # Back to 10-bit ADC counts.
    iq = np.frombuffer(p[RAWIQ_HDR.size : RAWIQ_HDR.size + 2 * n], dtype=np.int8).astype(np.float32) * (1 << shift)
    x = iq[0::2] + 1j * iq[1::2]
    print(f"lo {lo} kHz rate {rate} n {n} conj {conj} shift {shift} (10-bit counts) mean {x.mean():.2f} rms {np.sqrt(np.mean(abs(x) ** 2)):.2f} "
          f"max |I|,|Q| {abs(iq[0::2]).max():.0f},{abs(iq[1::2]).max():.0f}")
    if args.output:
        np.save(args.output, x)
        print("saved", args.output)
    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        nfft = 2048
        segs = len(x) // nfft
        w = np.hanning(nfft)
        psd = np.zeros(nfft)
        for s in range(segs):
            seg = x[s * nfft : (s + 1) * nfft]
            psd += abs(np.fft.fftshift(np.fft.fft((seg - seg.mean()) * w))) ** 2
        f = (np.arange(nfft) - nfft / 2) * rate / nfft / 1e6 + lo / 1000
        fig, ax = plt.subplots(2, 1, figsize=(12, 7))
        ax[0].plot(f, 10 * np.log10(psd / segs + 1e-9))
        ax[0].set_xlabel("MHz"); ax[0].grid(True)
        t = np.arange(min(2000, len(x))) / rate * 1e6
        ax[1].plot(t, x.real[: len(t)], t, x.imag[: len(t)])
        ax[1].set_xlabel("us"); ax[1].grid(True)
        fig.tight_layout(); fig.savefig(args.plot)
        print("plot", args.plot)


def cmd_spectrum(link, args):
    prof = get_profile(link)
    hold = {}
    end = time.time() + args.seconds
    frames = 0
    while time.time() < end:
        for item in link.poll():
            if item[0] == "frame" and item[1] == FRAME_SPECTRUM:
                s = parse_spectrum(item[2])
                frames += 1
                for j, db in enumerate(s["db"]):
                    f = round((s["lo_khz"] * 1000 + s["start_off_hz"] + j * s["bin_hz"]) / 1e3)
                    hold[f] = max(hold.get(f, -200), db)
    print(f"{frames} spectrum frames, {len(hold)} points")
    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fs = sorted(hold)
        fig, ax = plt.subplots(figsize=(14, 5))
        ax.plot([f / 1000 for f in fs], [hold[f] for f in fs], ".", ms=2)
        for c in prof["channels"]:
            ax.axvline(c["khz"] / 1000, color="r", alpha=0.3)
            ax.text(c["khz"] / 1000, ax.get_ylim()[1], c["name"], ha="center", va="top")
        ax.set_xlabel("MHz"); ax.set_ylabel("dBFS/bin (max hold)"); ax.grid(True)
        fig.tight_layout(); fig.savefig(args.plot)
        print("plot", args.plot)


def cmd_bootloader(link, args):
    link.send("BOOTLOADER")
    time.sleep(0.5)
    print("requested; the port re-enumerates in ROM download mode")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    c = sub.add_parser("cmd"); c.add_argument("lines", nargs="+")
    m = sub.add_parser("monitor"); m.add_argument("-s", "--seconds", type=float, default=0)
    m.add_argument("--plain", action="store_true")
    r = sub.add_parser("rawiq"); r.add_argument("lo_khz", type=int); r.add_argument("n", type=int)
    r.add_argument("-r", "--rate", type=int); r.add_argument("-o", "--output"); r.add_argument("--plot")
    s = sub.add_parser("spectrum"); s.add_argument("-s", "--seconds", type=float, default=3)
    s.add_argument("--plot")
    sub.add_parser("bootloader")
    args = ap.parse_args()
    link = Link(args.port)
    # The firmware boots silent in RotorHazard mode; our commands need monitor
    # mode (older firmware answers ERR command, and is always in it).
    if args.cmd != "bootloader":
        try:
            link.command("PROTOCOL MONITOR", timeout=1.0)
        except TimeoutError:
            print("no reply to PROTOCOL MONITOR", file=sys.stderr)
    {"info": cmd_info, "cmd": cmd_raw, "monitor": cmd_monitor, "rawiq": cmd_rawiq,
     "spectrum": cmd_spectrum, "bootloader": cmd_bootloader}[args.cmd](link, args)


if __name__ == "__main__":
    main()
