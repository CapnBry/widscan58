// Simulated Widscan58 device: speaks the real protocol from a synthetic RF
// environment so the UI can be developed and demoed without hardware.
// GPL-3.0-or-later.
import type { Transport } from "../serial/device";
import { encodeFrame } from "../serial/parser";
import { CHF, FRAME_LEVELS, FRAME_METRICS, FRAME_RAWIQ, FRAME_SPECTRUM, FRAME_TELEMETRY, type MetricsMode } from "../proto/frames";
import { channelMetrics, db, floorEstimate, percentileBias, planBuild, type Hop, type ProfileDef } from "./dsp";
import { LapSeat, type LapFilter } from "./lap";
import { RH, RH_API_MARKER, checksum } from "../proto/rh";

// The firmware's band profiles (firmware/src/scan/profile.c), plus what only
// the simulator needs.
interface SimProfile extends ProfileDef {
  rateCode: number;
  filterCode: number;
  wifiKhz: number[]; // 20 MHz Wi-Fi channel centers that burst in the band
}

const K24 = 83.5 / 296;
const race24: SimProfile = {
  name: "s3-race24",
  description: "Raceband scaled to 2.4 GHz (k=0.2821), ESP32-S3 mock-up [SIMULATED]",
  target: "esp32s3",
  bandLoKhz: 2400000,
  bandHiKhz: 2486500,
  baseKhz: 2400000,
  equivBaseKhz: 5639500,
  scale: K24,
  rateHz: 40e6,
  rateCode: 1,
  filterCode: 60,
  nfft: 1024,
  segments: 4,
  usableHz: 11.3e6,
  passbandHz: 6.77e6,
  rssiHz: 1.41e6,
  searchHz: 3.39e6,
  hopMaxSpanKhz: 11290,
  singleLoOffsetKhz: -2260,
  loStepKhz: 1,
  channels: [2405219, 2415656, 2426094, 2436531, 2446969, 2457406, 2467844, 2478281].map((khz, i) => ({
    name: `R${i + 1}`,
    khz,
  })),
  spursKhz: [2400000, 2440000, 2448000, 2480000],
  wifiKhz: [2412000, 2437000, 2462000],
};

// The real band (ESP32-C5). Spurs: the LO sweep in PLAN.md, "Board spurs on the C5".
const c5race: SimProfile = {
  name: "c5-race",
  description: "Raceband R1-R8, ESP32-C5 [SIMULATED]",
  target: "esp32c5",
  bandLoKhz: 5645000,
  bandHiKhz: 5945000,
  baseKhz: 5639500,
  equivBaseKhz: 5639500,
  scale: 1,
  rateHz: 80e6,
  rateCode: 0,
  filterCode: 0,
  nfft: 1024,
  segments: 4,
  usableHz: 40e6,
  passbandHz: 24e6,
  rssiHz: 5e6,
  searchHz: 12e6,
  hopMaxSpanKhz: 40000,
  singleLoOffsetKhz: -8000,
  loStepKhz: 1000,
  channels: [5658000, 5695000, 5732000, 5769000, 5806000, 5843000, 5880000, 5917000].map((khz, i) => ({
    name: `R${i + 1}`,
    khz,
  })),
  spursKhz: [5600000, 5680000, 5712000, 5760000, 5840000, 5920000],
  wifiKhz: [5745000, 5765000, 5785000, 5805000, 5825000],
};

export type SimTarget = "c5" | "s3";

interface Vtx {
  ch: number;
  on: boolean;
  powerDb: number; // total power, dBFS
  offsetHz: number; // drift from nominal
  nextToggle: number;
}

function gaussRand(): number {
  let u = 0;
  while (!u) u = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * Math.random());
}

interface HopChannel {
  id: number;
  m: ReturnType<typeof channelMetrics>;
  ema: number;
  clear: number | null;
}

export class MockTransport implements Transport {
  readonly name = "Simulated device";
  private onBytes: (c: Uint8Array) => void = () => {};
  private timer = 0;
  private rx = "";
  private readonly profile: SimProfile;
  private channels: { name: string; khz: number }[];
  private enabled = 0xff;
  private group = true;
  private running = true;
  private gainManual = true;
  private gain = 45;
  private rssiHz = 0; // WIN; the profile's until set
  private searchHz = 0;
  private specEvery = 4;
  private specBins = 256;
  private specSpanHz = 0;
  private specDetector = 1;
  private ema = 0.25;
  private metricsMode: MetricsMode = "FULL";
  private clearEma = 0.02;
  private plan: Hop[] = [];
  private hop = 0;
  private seq = 0;
  private specSeq = 0;
  private hopsTotal = 0;
  private rotations = 0;
  private start = performance.now();
  private lastTelemetry = 0;
  private winHops = 0;
  private winRot = 0;
  private vtx: Vtx[] = [];
  private wifi = { centerKhz: 0, until: 0, next: 0 };
  private state = new Map<number, { ema: number; clear: number | null }>();
  // Protocol, as the firmware: boots silent in RotorHazard mode.
  private proto: "RH" | "MONITOR" = "RH";
  private rhSeats = Array.from({ length: 8 }, (_, j) => ({
    mhz: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917][j],
    mapped: false,
    lap: new LapSeat(125, 105),
    nextPass: 0,
    lastMs: 0,
  }));
  private rhCur = 0;
  private rhWrite: { cmd: number; need: number; got: number[] } | null = null;
  private rhText: string | null = null;
  private rhMetric: "RSSI" | "SNR" = "RSSI";
  private rhNfft = 64; // the capture is 4 × this in RotorHazard mode
  private rhFilter: LapFilter = "MEDIAN";
  // CONFIG SAVE: applied on every PROTOCOL RH, as the firmware (not kept across page loads).
  private static readonly RH_BUILTIN = {
    gainManual: true, gain: 45, group: true, metric: "RSSI" as "RSSI" | "SNR", nfft: 64, filter: "MEDIAN" as LapFilter,
  };
  private rhSaved = { ...MockTransport.RH_BUILTIN };
  private readonly tskipDefault: number; // the firmware's TUNESKIP default per target
  private tskip: number;
  // GPIO?: one character per pin, '-' not used, 'L', 'H', 'x' flash or USB
  // (as the firmware). The saved ones are kept across REBOOT, not page loads.
  private gpioActive: string[];
  private gpioSaved: string[];

  constructor(target: SimTarget = "c5") {
    this.profile = target === "s3" ? race24 : c5race;
    this.channels = this.profile.channels;
    this.rssiHz = this.profile.rssiHz;
    this.searchHz = this.profile.searchHz;
    this.tskipDefault = this.tskip = target === "s3" ? 7 : 3;
    const reserved = target === "s3" ? [19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32] : [13, 14, 16, 17, 18, 20, 21, 22];
    this.gpioSaved = Array.from({ length: target === "s3" ? 49 : 29 }, (_, n) => (reserved.includes(n) ? "x" : "-"));
    this.gpioActive = [...this.gpioSaved];
  }

  async open(onBytes: (c: Uint8Array) => void): Promise<void> {
    this.onBytes = onBytes;
    this.reset();
    this.timer = window.setInterval(() => this.tick(), 20);
  }

  async close(): Promise<void> {
    clearInterval(this.timer);
  }

  async write(data: Uint8Array): Promise<void> {
    if (this.proto === "RH") {
      for (const b of data) this.rhByte(b);
      return;
    }
    this.rx += new TextDecoder().decode(data);
    let nl: number;
    while ((nl = this.rx.indexOf("\n")) >= 0) {
      const line = this.rx.slice(0, nl).trim();
      this.rx = this.rx.slice(nl + 1);
      if (line) setTimeout(() => this.command(line), 2);
    }
  }

  private reset() {
    const p = this.profile;
    this.plan = planBuild(p, this.channels, this.enabled, this.group);
    this.specSpanHz = p.usableHz;
    this.state.clear();
    const now = performance.now();
    this.vtx = this.channels.map((_, ch) => ({
      ch,
      on: ch === 1 || ch === 5,
      powerDb: -22 - Math.random() * 14,
      offsetHz: (Math.random() - 0.5) * 0.6e6 * (p.scale < 1 ? p.scale * 3 : 1),
      nextToggle: now + 4000 + Math.random() * 12000,
    }));
  }

  private text(s: string) {
    this.onBytes(new TextEncoder().encode(s));
  }

  private command(line: string) {
    const p = this.profile;
    const ok = () => this.text("OK\n");
    const parts = line.split(" ");
    const num = (i: number) => Number(parts[i]);
    switch (true) {
      case line === "INFO":
        return this.text(`INFO widscan58|${p.target}|1|0.1.0-sim|2026-01-01T00:00:00+00:00|SIMULATED\n`);
      case line === "PROFILE?":
        return this.text(
          `PROFILE ${JSON.stringify({
            name: p.name,
            description: p.description,
            target: p.target,
            band_khz: [p.bandLoKhz, p.bandHiKhz],
            base_khz: p.baseKhz,
            equiv_base_khz: p.equivBaseKhz,
            scale: p.scale,
            rate_hz: p.rateHz,
            usable_hz: p.usableHz,
            passband_hz: p.passbandHz,
            rssi_hz: this.rssiHz,
            search_hz: this.searchHz,
            channels: this.channels.map((c, id) => ({ id, name: c.name, khz: c.khz })),
          })}\n`,
        );
      case line === "PROTOCOL?":
        return this.text(`PROTOCOL ${this.proto}\n`);
      case line === "PROTOCOL RH" || line === "PROTOCOL MONITOR":
        this.proto = parts[1] as "RH" | "MONITOR";
        this.rx = "";
        if (this.proto === "RH") this.applyRhSaved();
        else this.metricsMode = "FULL";
        this.state.clear();
        return ok();
      case line === "CONFIG SAVE":
        if (this.proto !== "RH") return this.text("ERR rh_only\n");
        this.rhSaved = {
          gainManual: this.gainManual, gain: this.gain, group: this.group, metric: this.rhMetric, nfft: this.rhNfft, filter: this.rhFilter,
        };
        return ok();
      case line === "CONFIG RESET":
        if (this.proto !== "RH") return this.text("ERR rh_only\n");
        this.rhSaved = { ...MockTransport.RH_BUILTIN };
        this.applyRhSaved();
        this.state.clear();
        return ok();
      case line === "LEVELFILTER MEDIAN" || line === "LEVELFILTER EMA":
        if (this.proto !== "RH") return this.text("ERR rh_only\n");
        this.setRhFilter(parts[1] as LapFilter);
        return ok();
      case line === "LEVELFILTER?":
        return this.text(`LEVELFILTER ${this.rhFilter}\n`);
      case this.proto === "RH" && parts[0] === "FFT":
        this.rhNfft = num(1);
        return ok();
      case line === "METRICS RSSI" || line === "METRICS SNR":
        if (this.proto === "RH") this.rhMetric = parts[1] as "RSSI" | "SNR";
        else this.metricsMode = parts[1] as MetricsMode;
        this.state.clear();
        return ok();
      case line === "STATUS?" || line === "CONFIG?":
        return this.text(
          `${parts[0].slice(0, -1)} ${JSON.stringify({
            protocol: this.proto,
            profile: p.name,
            running: +this.running,
            enabled: this.enabled,
            mode: this.group ? "GROUP" : "SINGLE",
            capture_samples: this.proto === "RH" ? 4 * this.rhNfft : p.nfft * p.segments,
            nfft: this.proto === "RH" ? this.rhNfft : p.nfft,
            segments: p.segments,
            rate_code: p.rateCode,
            filter: p.filterCode,
            conjugate: 1,
            iq_corr: 1,
            settle_us: 100,
            tskip: this.tskip,
            gain_mode: this.gainManual ? "MANUAL" : "HARDWARE",
            gain: this.gain,
            gain_max: 82,
            rawiq_max: 16380,
            recal: "ONCE",
            metrics: this.proto === "RH" ? this.rhMetric : this.metricsMode,
            rssi_hz: this.rssiHz,
            search_hz: this.searchHz,
            dc_guard: 1,
            ema: this.ema,
            clear_ema: this.clearEma,
            signal_db: 6,
            avg_shift: 7,
            spec_every: this.specEvery,
            spec_bins: this.specBins,
            spec_span_hz: this.specSpanHz,
            spec_detector: this.specDetector,
            level_filter: this.rhFilter,
            plan: this.plan.map((h) => ({ lo_khz: h.loKhz, ch: h.ch, offset_hz: h.offsetHz })),
          })}\n`,
        );
      case line === "CHANNELS DEFAULT":
        this.channels = p.channels;
        this.enabled = (1 << this.channels.length) - 1;
        this.reset();
        return ok();
      case parts[0] === "CHANNELS": {
        // Same mapping as the firmware: 5.8 GHz MHz into the profile's band.
        const list = parts.slice(1).map((t) => {
          const [name, mhz] = t.split("=");
          const khz = Math.round(p.baseKhz + (Number(mhz) * 1000 - p.equivBaseKhz) * p.scale);
          return /^[A-Za-z0-9]{1,5}$/.test(name ?? "") && khz >= p.bandLoKhz && khz <= p.bandHiKhz ? { name, khz } : null;
        });
        if (!list.length || list.length > 8 || list.some((c) => !c)) return this.text("ERR channels\n");
        this.channels = list as { name: string; khz: number }[];
        this.enabled = (1 << this.channels.length) - 1;
        this.reset();
        return ok();
      }
      case line === "ENABLE?":
        return this.text(`ENABLE ${this.enabled}\n`);
      case parts[0] === "ENABLE":
        this.enabled = num(1) & ((1 << this.channels.length) - 1);
        this.plan = planBuild(p, this.channels, this.enabled, this.group);
        this.hop = 0;
        return this.text(`OK ENABLE ${this.enabled}\n`);
      case line === "MODE GROUP" || line === "MODE SINGLE":
        this.group = line === "MODE GROUP";
        this.plan = planBuild(p, this.channels, this.enabled, this.group);
        this.hop = 0;
        return ok();
      case line === "SCAN START" || line === "SCAN STOP":
        this.running = line === "SCAN START";
        return ok();
      case line === "GAIN HARDWARE":
        this.gainManual = false;
        return ok();
      case parts[0] === "GAIN" && parts[1] === "MANUAL":
        this.gainManual = true;
        this.gain = num(2);
        return ok();
      case parts[0] === "SPEC":
        this.specEvery = num(1);
        this.specBins = num(2);
        this.specSpanHz = num(3);
        this.specDetector = parts[4] === "MAX" ? 1 : 0;
        return ok();
      case line === "METRICS FULL":
        this.metricsMode = "FULL";
        this.state.clear();
        return ok();
      case line === "METRICS?":
        return this.text(`METRICS ${this.metricsMode}\n`);
      case parts[0] === "EMA":
        this.ema = num(1) / 1000;
        this.clearEma = num(2) / 1000;
        return ok();
      case parts[0] === "RAWIQ":
        if (num(1) % p.loStepKhz) return this.text("ERR rawiq_args\n");
        return this.rawiq(num(1), num(2));
      case line === "TIMING?":
        return this.text("TIMING psd 2650 (prep 600 fft 1650 power 260) metrics 470 spectrum 520\n");
      case line === "SPURS?":
        return this.text(`SPURS ${p.spursKhz.length} ${p.spursKhz.join(" ")} guard 60000\n`);
      case line === "RECAL":
        return this.text("OK RECAL\n");
      case parts[0] === "TUNESKIP":
        if (parts.length !== 2 || !/^[0-7]$/.test(parts[1])) return this.text("ERR command\n");
        this.tskip = num(1);
        return ok();
      case parts[0] === "WIN":
        if (parts.length !== 3 || !(num(1) > 0 && num(2) >= num(1) && num(2) <= 20e6)) return this.text("ERR command\n");
        this.rssiHz = num(1);
        this.searchHz = num(2);
        return ok();
      case ["FILTER", "CAPN", "FFT", "SEGMENTS", "RATE", "SETTLE", "CONJ", "IQCORR", "RECALMODE", "SPURS", "AVG", "PROMISC", "BOOTLOADER"].includes(parts[0]):
        return ok();
      case line === "REBOOT":
        // Like the firmware: back to defaults, silent in RotorHazard mode.
        setTimeout(() => {
          this.proto = "RH";
          this.rx = "";
          this.channels = p.channels;
          this.enabled = (1 << this.channels.length) - 1;
          this.rssiHz = p.rssiHz;
          this.searchHz = p.searchHz;
          this.metricsMode = "FULL";
          this.tskip = this.tskipDefault;
          this.gpioActive = [...this.gpioSaved];
          this.reset();
          this.applyRhSaved();
        }, 300);
        return ok();
      case line === "GPIO?":
        return this.text(
          `GPIO ${JSON.stringify({ count: this.gpioActive.length, active: this.gpioActive.join(""), saved: this.gpioSaved.join("") })}\n`,
        );
      case line === "GPIO SAVE":
        this.gpioSaved = [...this.gpioActive];
        return ok();
      case parts[0] === "GPIO" && parts.length === 3 && /^\d+$/.test(parts[1]): {
        const s = { NONE: "-", LOW: "L", HIGH: "H" }[parts[2]];
        if (!s) return this.text("ERR gpio_state\n");
        if (this.gpioActive[num(1)] === undefined || this.gpioActive[num(1)] === "x") return this.text("ERR gpio_pin\n");
        this.gpioActive[num(1)] = s;
        return ok();
      }
      case parts[0] === "SYNC":
        return this.text(`SYNC ${parts[1]}\n`);
      default:
        return this.text("ERR command\n");
    }
  }

  // ---- RotorHazard mode -------------------------------------------------------
  // The firmware's protocol (firmware/src/proto/rh.c) on synthetic levels:
  // each seat on a channel sees a quad pass the gate every 5-9 s.

  private applyRhSaved() {
    ({ gainManual: this.gainManual, gain: this.gain, group: this.group, metric: this.rhMetric, nfft: this.rhNfft } =
      this.rhSaved);
    this.setRhFilter(this.rhSaved.filter);
    this.plan = planBuild(this.profile, this.channels, this.enabled, this.group);
    this.hop = 0;
  }

  private setRhFilter(f: LapFilter) {
    this.rhFilter = f;
    for (const s of this.rhSeats) s.lap.setFilter(f);
  }

  private rhAllowed(line: string): boolean {
    return (
      ["PROTOCOL?", "PROTOCOL MONITOR", "PROTOCOL RH", "INFO", "CONFIG?", "METRICS RSSI", "METRICS SNR", "METRICS?",
        "GAIN?", "GAIN HARDWARE", "FILTER AUTO", "MODE GROUP", "MODE SINGLE", "CONFIG SAVE", "CONFIG RESET", "RECAL",
        "LEVELFILTER MEDIAN", "LEVELFILTER EMA", "LEVELFILTER?", "GPIO?"].includes(line) ||
      ["GAIN MANUAL ", "FFT ", "FILTER ", "IQCORR ", "GPIO "].some((p) => line.startsWith(p))
    );
  }

  private rhByte(b: number) {
    if (this.rhText !== null) {
      if (b === 10) {
        const line = this.rhText.trim();
        this.rhText = null;
        if (line === "METRICS?") this.text(`METRICS ${this.rhMetric}\n`);
        else if (this.rhAllowed(line)) setTimeout(() => this.command(line), 2);
        else this.text("ERR rh_mode\n");
      } else if (b !== 13) this.rhText += String.fromCharCode(b);
      return;
    }
    if (this.rhWrite) {
      const w = this.rhWrite;
      w.got.push(b);
      if (w.got.length < w.need + 1) return;
      this.rhWrite = null;
      if (checksum(w.got, w.need) === w.got[w.need]) this.rhWriteCmd(w.cmd, w.got);
      return;
    }
    if (b >= 0x41 && b <= 0x50) {
      this.rhText = String.fromCharCode(b);
      return;
    }
    if (b <= 0x50) return this.rhRead(b);
    const need = b === RH.WRITE_FREQUENCY || b === 0x75 ? 2 : [0x71, 0x72, 0x78, 0x79, 0x7a, 0x7e].includes(b) ? 1 : 0;
    if (need) this.rhWrite = { cmd: b, need, got: [] };
  }

  private rhNow(): number {
    return Math.floor(performance.now() - this.start);
  }

  private rhRead(cmd: number) {
    const s = this.rhSeats[this.rhCur];
    const l = s.lap;
    const now = this.rhNow();
    const out: number[] = [];
    const u16 = (v: number) => out.push((v >> 8) & 0xff, v & 0xff);
    const passStats = () => {
      out.push(l.lap);
      u16((now - l.passMs) & 0xffff);
      out.push(l.valid ? l.level : 1, l.nodePeak, l.passPeak);
      u16(l.loopUs);
    };
    const extremums = () => {
      const x = l.popExtremum();
      out.push((l.crossing ? 1 : 0) | (x?.peak ? 2 : 0), l.passNadir, l.nodeNadir);
      if (x) {
        out.push(x.e.level);
        u16((now - x.e.firstMs) & 0xffff);
        u16(x.e.durationMs);
      } else out.push(0, 0, 0, 0, 0);
    };
    const text = (t: string) => {
      for (let j = 0; j < 16; j++) out.push(j < t.length ? t.charCodeAt(j) : 0);
    };
    switch (cmd) {
      case RH.READ_FREQUENCY: u16(s.mhz); break;
      case RH.TEST_RX_REGISTER: out.push(+s.mapped); break;
      case RH.READ_LAP_PASS_STATS: passStats(); break;
      case RH.READ_LAP_EXTREMUMS: extremums(); break;
      case RH.READ_REVISION_CODE: out.push(RH_API_MARKER, 36); break;
      case RH.READ_NODE_RSSI_PEAK: out.push(l.nodePeak); break;
      case RH.READ_NODE_RSSI_NADIR: out.push(l.nodeNadir); break;
      case RH.READ_ENTER_AT_LEVEL: out.push(l.enterAt); break;
      case RH.READ_EXIT_AT_LEVEL: out.push(l.exitAt); break;
      case RH.READ_TIME_MILLIS: out.push((now >>> 24) & 0xff, (now >> 16) & 0xff, (now >> 8) & 0xff, now & 0xff); break;
      case RH.READ_MULTINODE_COUNT: out.push(8); break;
      case RH.READ_CURNODE_INDEX: case 0x3c: out.push(this.rhCur); break;
      case 0x11: u16(0); break;
      case RH.READ_FW_VERSION: text("0.1.0-sim"); break;
      case 0x40: text("Widscan58 sim"); break;
      default: return;
    }
    out.push(checksum(out));
    setTimeout(() => this.onBytes(Uint8Array.from(out)), 1);
  }

  private rhWriteCmd(cmd: number, d: number[]) {
    const s = this.rhSeats[this.rhCur];
    if (cmd === RH.WRITE_CURNODE_INDEX) {
      if (d[0] < 8) this.rhCur = d[0];
    } else if (cmd === RH.WRITE_FREQUENCY) {
      const mhz = (d[0] << 8) | d[1];
      if (mhz < 100 || mhz > 9999) return;
      const p = this.profile;
      const khz = Math.round(p.baseKhz + (mhz * 1000 - p.equivBaseKhz) * p.scale);
      if (mhz !== s.mhz || !s.mapped) s.lap.reset();
      s.mhz = mhz;
      s.mapped = khz >= p.bandLoKhz && khz <= p.bandHiKhz;
      s.nextPass = performance.now() + 1500 + Math.random() * 4000;
    } else if (cmd === RH.WRITE_ENTER_AT_LEVEL) s.lap.enterAt = d[0];
    else if (cmd === RH.WRITE_EXIT_AT_LEVEL) s.lap.exitAt = d[0];
    else if (cmd === RH.FORCE_END_CROSSING) s.lap.endCrossing();
  }

  /** Levels at ~5 ms per seat update: floor plus noise, and a pass bump. */
  private rhTick(now: number) {
    const tMs = this.rhNow();
    const snr = this.rhMetric === "SNR";
    for (const [j, s] of this.rhSeats.entries()) {
      if (!s.mapped) continue;
      if (now > s.nextPass + 1500) s.nextPass = now + 5000 + Math.random() * 4000 + j * 300;
      const peakDb = 16 + (j % 4) * 4; // a little different per seat
      for (let k = 3; k >= 0; k--) {
        const t = now - k * 5;
        const bump = peakDb * Math.exp(-(((t - s.nextPass) / 220) ** 2));
        const db = bump + gaussRand() * 0.6;
        const code = snr ? Math.max(1, 4 * db + 1) : 4 * (db - 47.5) + 255;
        s.lap.feed(tMs - k * 5, Math.max(1, Math.min(254, Math.round(code))), 5000);
      }
    }
  }

  // ---- RF environment -------------------------------------------------------

  private gainDb(): number {
    return this.gainManual ? (this.gain - 60) * 1.0 : 0;
  }

  /** Power spectral density (linear, tone-normalised per bin) at absolute Hz. */
  private emitters(now: number): { hz: number; powerDb: number; kind: "vtx" | "imd" }[] {
    const p = this.profile;
    const out: { hz: number; powerDb: number; kind: "vtx" | "imd" }[] = [];
    const on = this.vtx.filter((v) => v.on);
    for (const v of on) {
      const wobble = Math.sin(now / 1700 + v.ch) * 40e3 * (p.scale < 1 ? p.scale : 1);
      out.push({ hz: this.channels[v.ch].khz * 1000 + v.offsetHz + wobble, powerDb: v.powerDb, kind: "vtx" });
    }
    // Third-order intermodulation between every pair of active carriers.
    for (const a of on)
      for (const b of on) {
        if (a === b) continue;
        const fa = this.channels[a.ch].khz * 1000 + a.offsetHz;
        const fb = this.channels[b.ch].khz * 1000 + b.offsetHz;
        out.push({ hz: 2 * fa - fb, powerDb: 2 * a.powerDb + b.powerDb + 62, kind: "imd" });
      }
    return out;
  }

  private psd(loKhz: number, now: number): Float64Array {
    const p = this.profile;
    const n = p.nfft;
    const binHz = p.rateHz / n;
    const out = new Float64Array(n);
    const gain = 10 ** (this.gainDb() / 10);
    const s = p.scale;
    const noise = 10 ** (-68 / 10) * gain;
    const adc = 10 ** (-80 / 10);
    const em = this.emitters(now);
    const wifiOn = now < this.wifi.until;
    for (let k = 0; k < n; k++) {
      const off = (k - n / 2) * binHz;
      const abs = loKhz * 1000 + off;
      const x = off / p.passbandHz;
      const filter = 1 / (1 + x ** 12);
      let sig = 0;
      for (const e of em) {
        const d = (abs - e.hz) / (s < 1 ? s : 1);
        const pw = 10 ** ((e.powerDb + this.gainDb()) / 10);
        if (e.kind === "vtx") {
          // FM video: strong core, wide pedestal, audio subcarrier lines at +/-6.5 MHz.
          sig += pw * (0.12 * Math.exp(-((d / 0.7e6) ** 2)) + 0.012 * Math.exp(-((d / 3.5e6) ** 2)));
          sig += pw * 0.004 * (Math.exp(-(((d - 6.5e6) / 0.15e6) ** 2)) + Math.exp(-(((d + 6.5e6) / 0.15e6) ** 2)));
        } else {
          sig += pw * 0.05 * Math.exp(-((d / 1.2e6) ** 2));
        }
      }
      if (wifiOn && Math.abs(abs - this.wifi.centerKhz * 1000) < 8.3e6) sig += 10 ** ((-60 + this.gainDb()) / 10);
      for (const sp of p.spursKhz) if (Math.abs(abs - sp * 1000) < binHz) sig += 10 ** (-58 / 10);
      // Average of `segments` exponential variates (Welch).
      let fluct = 0;
      for (let j = 0; j < p.segments; j++) fluct -= Math.log(1 - Math.random());
      out[k] = ((noise + sig) * filter + adc) * (fluct / p.segments);
    }
    return out;
  }

  private evolve(now: number) {
    for (const v of this.vtx) {
      if (now > v.nextToggle) {
        v.on = !v.on;
        v.nextToggle = now + (v.on ? 6000 + Math.random() * 15000 : 3000 + Math.random() * 20000);
        if (v.on) v.powerDb = -20 - Math.random() * 18;
      }
    }
    if (now > this.wifi.next) {
      this.wifi.until = now + 3 + Math.random() * 25;
      this.wifi.next = now + 30 + Math.random() * 400;
      const w = this.profile.wifiKhz;
      this.wifi.centerKhz = w[Math.floor(Math.random() * w.length)];
    }
  }

  private tick() {
    const now = performance.now();
    if (this.proto === "RH") return this.rhTick(now);
    this.evolve(now);
    // ~5 ms per hop like the hardware: emit hops for the elapsed tick.
    const hops = this.running && this.plan.length ? 4 : 0;
    for (let j = 0; j < hops; j++) this.doHop(now);
    if (now - this.lastTelemetry > 250) this.telemetry(now);
  }

  private doHop(now: number) {
    const p = this.profile;
    const h = this.plan[this.hop];
    const psd = this.psd(h.loKhz, now);
    const n = p.nfft;
    const binHz = p.rateHz / n;
    const valid = new Uint8Array(n);
    for (let k = 0; k < n; k++) valid[k] = +(Math.abs(k - n / 2) > 1 && Math.abs((k - n / 2) * binHz) <= p.usableHz);
    for (const sp of p.spursKhz) {
      const k = n / 2 + Math.round((sp * 1000 - h.loKhz * 1000) / binHz);
      for (let m = k - 2; m <= k + 2; m++) if (m >= 0 && m < n) valid[m] = 0;
    }
    const input = {
      p: psd,
      valid,
      binHz,
      enbwBins: 1.5,
      floorBias: percentileBias(p.segments, 0.2),
      passbandHz: p.passbandHz,
      rssiHz: this.rssiHz,
      searchHz: this.searchHz,
      signalDb: 6,
      centroidDb: 6,
    };
    const floorBin = floorEstimate(input, 0.2);
    const chans = h.ch.map((id, j) => {
      const m = channelMetrics(input, floorBin, h.offsetHz[j]);
      for (const sp of p.spursKhz)
        if (Math.abs((sp * 1000 - h.loKhz * 1000) - h.offsetHz[j]) <= this.searchHz) m.flags |= CHF.SPUR;
      const lin = 10 ** (m.rssi / 10);
      const st = this.state.get(id) ?? { ema: lin, clear: null };
      st.ema += this.ema * (lin - st.ema);
      if (!(m.flags & CHF.SIGNAL)) st.clear = st.clear === null ? lin : st.clear + this.clearEma * (lin - st.clear);
      this.state.set(id, st);
      return { id, m, ema: db(st.ema), clear: st.clear === null ? null : db(st.clear) };
    });
    const tune = 900 + Math.random() * 200;
    // Same LO rotation as the firmware: one spectrum per specEvery hops.
    if (this.specEvery && this.hop % this.specEvery === this.rotations % this.specEvery) this.spectrum(h.loKhz, psd, binHz);
    if (this.metricsMode !== "FULL") this.emitLevels(now, h.loKhz, tune, floorBin, chans);
    else this.emitMetrics(now, h.loKhz, tune, floorBin, chans);
    this.hopsTotal++;
    this.winHops++;
    if (++this.hop >= this.plan.length) {
      this.hop = 0;
      this.rotations++;
      this.winRot++;
    }
  }

  private emitMetrics(now: number, loKhz: number, tune: number, floorBin: number, chans: HopChannel[]) {
    const buf = new DataView(new ArrayBuffer(34 + 26 * chans.length));
    buf.setUint32(0, this.seq++, true);
    buf.setUint32(4, Math.floor(now - this.start), true);
    buf.setUint32(8, loKhz, true);
    buf.setUint32(12, tune, true);
    buf.setUint32(16, 160, true);
    buf.setUint32(20, 3400 + Math.random() * 300, true);
    buf.setUint16(24, 0, true);
    buf.setInt16(26, Math.round(db(floorBin) * 100), true);
    buf.setInt16(28, -38 * 100, true);
    buf.setUint8(30, this.gainManual ? this.gain : 255);
    buf.setUint8(31, this.hop);
    buf.setUint8(32, this.plan.length);
    buf.setUint8(33, chans.length);
    const c16 = (v: number) => Math.max(-32768, Math.min(32767, Math.round(v * 100)));
    chans.forEach((c, j) => {
      const o = 34 + 26 * j;
      buf.setUint8(o, c.id);
      buf.setUint8(o + 1, c.m.flags);
      buf.setInt16(o + 2, c16(c.m.rssi), true);
      buf.setInt16(o + 4, c16(c.ema), true);
      buf.setInt16(o + 6, c16(c.m.floor), true);
      buf.setInt16(o + 8, c16(c.m.snr), true);
      buf.setInt16(o + 10, c.clear === null ? -32768 : c16(c.clear), true);
      buf.setInt16(o + 12, c16(c.m.peak), true);
      buf.setInt32(o + 14, Math.round(c.m.peakOff), true);
      buf.setInt32(o + 18, Math.round(c.m.centroid), true);
      buf.setInt16(o + 22, Math.round(c.m.obwLo / 1000), true);
      buf.setInt16(o + 24, Math.round(c.m.obwHi / 1000), true);
    });
    this.onBytes(encodeFrame(FRAME_METRICS, new Uint8Array(buf.buffer)));
  }

  // METRICS RSSI|SNR: frame_levels_t, level codes (dBFS = code / 4 - 63.75; the
  // per-bin floor is a spectrum code, dBFS = code / 2 - 127.5).
  private emitLevels(now: number, loKhz: number, tune: number, floorBin: number, chans: HopChannel[]) {
    const snr = this.metricsMode === "SNR";
    const code = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
    const buf = new DataView(new ArrayBuffer(32 + 4 * chans.length));
    buf.setUint32(0, this.seq++, true);
    buf.setUint32(4, Math.floor(now - this.start), true);
    buf.setUint32(8, loKhz, true);
    buf.setUint32(12, tune, true);
    buf.setUint32(16, 60, true);
    buf.setUint32(20, 900 + Math.random() * 100, true);
    buf.setUint16(24, 0, true);
    buf.setUint8(26, this.gainManual ? this.gain : 255);
    buf.setUint8(27, this.hop);
    buf.setUint8(28, this.plan.length);
    buf.setUint8(29, snr ? 2 : 1);
    buf.setUint8(30, snr ? code(2 * db(floorBin) + 255) : 0);
    buf.setUint8(31, chans.length);
    chans.forEach((c, j) => {
      const o = 32 + 4 * j;
      buf.setUint8(o, c.id);
      buf.setUint8(o + 1, snr ? c.m.flags & ~(CHF.NO_CENTROID) : c.m.flags & ~(CHF.SIGNAL | CHF.NO_CENTROID));
      buf.setUint8(o + 2, code(4 * c.m.rssi + 255));
      buf.setUint8(o + 3, snr ? code(4 * c.m.snr) : 0);
    });
    this.onBytes(encodeFrame(FRAME_LEVELS, new Uint8Array(buf.buffer)));
  }

  private spectrum(loKhz: number, psd: Float64Array, binHz: number) {
    const n = psd.length;
    const span = Math.min(this.specSpanHz, (binHz * n) / 2);
    const k0 = Math.max(0, n / 2 - Math.floor(span / binHz));
    const k1 = Math.min(n - 1, n / 2 + Math.floor(span / binHz) - 1);
    const nin = k1 - k0 + 1;
    const nout = Math.min(this.specBins, nin);
    const stride = nin / nout;
    const out = new Uint8Array(20 + nout);
    const dv = new DataView(out.buffer);
    for (let j = 0; j < nout; j++) {
      const a = k0 + Math.floor(j * stride);
      const b = Math.max(a + 1, k0 + Math.floor((j + 1) * stride));
      let acc = 0;
      let cnt = 0;
      for (let k = a; k < b; k++) {
        if (Math.abs(k - n / 2) <= 1) continue;
        acc = this.specDetector ? Math.max(acc, psd[k]) : acc + psd[k];
        cnt++;
      }
      const v = cnt ? (this.specDetector ? acc : acc / cnt) : psd[a];
      const d = db(v);
      out[20 + j] = d <= -127.5 ? 0 : d >= 0 ? 255 : Math.round((d + 127.5) * 2);
    }
    dv.setUint32(0, this.specSeq++, true);
    dv.setUint32(4, loKhz, true);
    dv.setInt32(8, Math.round((k0 - n / 2 + stride / 2 - 0.5) * binHz), true);
    dv.setUint32(12, Math.round(stride * binHz), true);
    dv.setUint16(16, nout, true);
    dv.setUint8(18, this.specDetector);
    this.onBytes(encodeFrame(FRAME_SPECTRUM, out));
  }

  private telemetry(now: number) {
    const dt = (now - this.lastTelemetry) / 1000;
    this.lastTelemetry = now;
    const out = new DataView(new ArrayBuffer(59));
    out.setUint32(0, Math.floor(now - this.start), true);
    out.setUint32(4, this.rotations, true);
    out.setUint32(8, this.hopsTotal, true);
    out.setUint32(12, Math.round((this.winRot / dt) * 1000), true);
    out.setUint32(16, 4700, true);
    out.setUint32(20, 7600, true);
    out.setUint32(24, 1050, true);
    out.setUint32(28, 3700, true);
    out.setUint32(32, 165, true);
    out.setUint32(36, 3500, true);
    out.setUint32(40, 3790, true);
    out.setUint16(44, 12, true);
    out.setUint32(46, 85800, true);
    out.setUint32(50, 0, true);
    out.setUint32(54, 0, true);
    out.setUint8(58, +this.running);
    this.winHops = this.winRot = 0;
    this.onBytes(encodeFrame(FRAME_TELEMETRY, new Uint8Array(out.buffer)));
  }

  private rawiq(loKhz: number, n: number) {
    const p = this.profile;
    const now = performance.now();
    const rate = p.rateHz;
    const iq = new Int8Array(2 * n);
    const em = this.emitters(now).filter((e) => Math.abs(e.hz - loKhz * 1000) < p.passbandHz);
    const phases = em.map(() => Math.random() * 2 * Math.PI);
    for (let j = 0; j < n; j++) {
      const t = j / rate;
      let i = gaussRand() * 6 + 1.5;
      let q = gaussRand() * 6 - 1;
      em.forEach((e, k) => {
        const a = 127 * 10 ** ((e.powerDb + this.gainDb() + 3) / 20);
        // FM: line-rate sawtooth video with ~1 MHz (scaled) deviation.
        const dev = 1e6 * (p.scale < 1 ? p.scale : 1);
        const video = ((t * 15734) % 1) * 2 - 1;
        const ph = phases[k] + 2 * Math.PI * (e.hz - loKhz * 1000) * t + (2 * Math.PI * dev * video) / (2 * 15734);
        i += a * Math.cos(ph);
        q += a * Math.sin(ph);
      });
      iq[2 * j] = Math.max(-128, Math.min(127, Math.round(i)));
      iq[2 * j + 1] = Math.max(-128, Math.min(127, Math.round(q)));
    }
    // In chunks of 1024 samples, as the firmware sends them.
    for (let off = 0; off < n; off += 1024) {
      const len = Math.min(1024, n - off);
      const out = new Uint8Array(14 + 2 * len);
      const dv = new DataView(out.buffer);
      dv.setUint32(0, loKhz, true);
      dv.setUint32(4, rate, true);
      dv.setUint16(8, n, true);
      dv.setUint16(10, off, true);
      dv.setUint8(12, 1);
      dv.setUint8(13, 2);
      out.set(new Uint8Array(iq.buffer, 2 * off, 2 * len), 14);
      this.onBytes(encodeFrame(FRAME_RAWIQ, out));
    }
  }
}
