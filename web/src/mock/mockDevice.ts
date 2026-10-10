// Simulated Widscan58 device: speaks the real protocol from a synthetic RF
// environment so the UI can be developed and demoed without hardware.
// GPL-3.0-or-later.
import type { Transport } from "../serial/device";
import { encodeFrame } from "../serial/parser";
import { CHF, FRAME_METRICS, FRAME_RAWIQ, FRAME_SPECTRUM, FRAME_TELEMETRY } from "../proto/frames";
import { channelMetrics, db, floorEstimate, percentileBias, planBuild, type Hop, type ProfileDef } from "./dsp";

const K24 = 83.5 / 296;
const race24: ProfileDef = {
  name: "s3-race24",
  description: "Raceband scaled to 2.4 GHz (k=0.2821), ESP32-S3 mock-up [SIMULATED]",
  target: "esp32s3",
  bandLoKhz: 2400000,
  bandHiKhz: 2483500,
  baseKhz: 2400000,
  equivBaseKhz: 5639500,
  scale: K24,
  rateHz: 40e6,
  nfft: 1024,
  segments: 4,
  usableHz: 11.3e6,
  passbandHz: 6.77e6,
  rssiHz: 1.41e6,
  searchHz: 3.39e6,
  pairMaxSpacingKhz: 11000,
  singleLoOffsetKhz: -2260,
  loStepKhz: 1,
  channels: [2405219, 2415656, 2426094, 2436531, 2446969, 2457406, 2467844, 2478281].map((khz, i) => ({
    name: `S${i + 1}`,
    khz,
  })),
  spursKhz: [2400000, 2440000, 2448000, 2480000],
};
const race58: ProfileDef = {
  name: "c5-race",
  description: "Raceband R1-R8, ESP32-C5 [SIMULATED]",
  target: "esp32c5",
  bandLoKhz: 5645000,
  bandHiKhz: 5945000,
  baseKhz: 5639500,
  equivBaseKhz: 5639500,
  scale: 1,
  rateHz: 80e6,
  nfft: 1024,
  segments: 4,
  usableHz: 40e6,
  passbandHz: 24e6,
  rssiHz: 5e6,
  searchHz: 12e6,
  pairMaxSpacingKhz: 40000,
  singleLoOffsetKhz: -8000,
  loStepKhz: 1000,
  channels: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917].map((mhz, i) => ({ name: `R${i + 1}`, khz: mhz * 1000 })),
  spursKhz: [],
};
const PROFILES = [race24, race58];

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

export class MockTransport implements Transport {
  readonly name = "Simulated device";
  private onBytes: (c: Uint8Array) => void = () => {};
  private timer = 0;
  private rx = "";
  private profile = race24;
  private enabled = 0xff;
  private pair = true;
  private running = true;
  private gainManual = true;
  private gain = 60;
  private specEvery = 4;
  private specBins = 256;
  private specSpanHz = race24.usableHz;
  private specDetector = 1;
  private ema = 0.25;
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
  private wifi = { centerKhz: 2437000, until: 0, next: 0 };
  private state = new Map<number, { ema: number; clear: number | null }>();

  async open(onBytes: (c: Uint8Array) => void): Promise<void> {
    this.onBytes = onBytes;
    this.reset();
    this.timer = window.setInterval(() => this.tick(), 20);
    this.text("READY widscan58 esp32s3 (simulated)\n");
  }

  async close(): Promise<void> {
    clearInterval(this.timer);
  }

  async write(data: Uint8Array): Promise<void> {
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
    this.plan = planBuild(p, this.enabled, this.pair);
    this.specSpanHz = p.usableHz;
    this.state.clear();
    const now = performance.now();
    this.vtx = p.channels.map((_, ch) => ({
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
        return this.text(`INFO widscan58 ${p.target} 1 0.1.0-sim SIMULATED\n`);
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
            rssi_hz: p.rssiHz,
            search_hz: p.searchHz,
            channels: p.channels.map((c, id) => ({ id, name: c.name, khz: c.khz })),
          })}\n`,
        );
      case line === "STATUS?":
        return this.text(
          `STATUS ${JSON.stringify({
            profile: p.name,
            running: +this.running,
            enabled: this.enabled,
            mode: this.pair ? "PAIR" : "SINGLE",
            capture_samples: p.nfft * p.segments,
            nfft: p.nfft,
            segments: p.segments,
            rate_code: 1,
            filter: 60,
            conjugate: 1,
            settle_us: 100,
            gain_mode: this.gainManual ? "MANUAL" : "HARDWARE",
            gain: this.gain,
            gain_max: 82,
            rawiq_max: 16380,
            recal: "ONCE",
            rssi_hz: p.rssiHz,
            search_hz: p.searchHz,
            dc_guard: 1,
            ema: this.ema,
            clear_ema: this.clearEma,
            signal_db: 6,
            avg_shift: 7,
            spec_every: this.specEvery,
            spec_bins: this.specBins,
            spec_span_hz: this.specSpanHz,
            spec_detector: this.specDetector,
            plan: this.plan.map((h) => ({ lo_khz: h.loKhz, ch: h.ch, offset_hz: h.offsetHz })),
          })}\n`,
        );
      case parts[0] === "PROFILE": {
        const np = PROFILES.find((x) => x.name === parts[1]);
        if (!np) return this.text("ERR profile\n");
        this.profile = np;
        this.reset();
        return ok();
      }
      case line === "ENABLE?":
        return this.text(`ENABLE ${this.enabled}\n`);
      case parts[0] === "ENABLE":
        this.enabled = num(1) & 0xff;
        this.plan = planBuild(p, this.enabled, this.pair);
        this.hop = 0;
        return this.text(`OK ENABLE ${this.enabled}\n`);
      case line === "MODE PAIR" || line === "MODE SINGLE":
        this.pair = line === "MODE PAIR";
        this.plan = planBuild(p, this.enabled, this.pair);
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
      case parts[0] === "EMA":
        this.ema = num(1) / 1000;
        this.clearEma = num(2) / 1000;
        return ok();
      case parts[0] === "RAWIQ":
        return this.rawiq(num(1), num(2));
      case line === "TIMING?":
        return this.text("TIMING psd 2650 (prep 600 fft 1650 power 260) metrics 470 spectrum 520\n");
      case line === "SPURS?":
        return this.text(`SPURS ${p.spursKhz.length} ${p.spursKhz.join(" ")} guard 60000\n`);
      case line === "RECAL":
        return this.text("OK RECAL\n");
      case ["FILTER", "CAPN", "FFT", "SEGMENTS", "RATE", "SETTLE", "CONJ", "RECALMODE", "WIN", "SPURS", "TUNESKIP", "AVG", "PROMISC", "REBOOT", "BOOTLOADER"].includes(parts[0]):
        return ok();
      case parts[0] === "SYNC":
        return this.text(`SYNC ${parts[1]}\n`);
      default:
        return this.text("ERR command\n");
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
      out.push({ hz: p.channels[v.ch].khz * 1000 + v.offsetHz + wobble, powerDb: v.powerDb, kind: "vtx" });
    }
    // Third-order intermodulation between every pair of active carriers.
    for (const a of on)
      for (const b of on) {
        if (a === b) continue;
        const fa = p.channels[a.ch].khz * 1000 + a.offsetHz;
        const fb = p.channels[b.ch].khz * 1000 + b.offsetHz;
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
    const wifiOn = p.name === "s3-race24" && now < this.wifi.until;
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
      this.wifi.centerKhz = [2412000, 2437000, 2462000][Math.floor(Math.random() * 3)];
    }
  }

  private tick() {
    const now = performance.now();
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
      rssiHz: p.rssiHz,
      searchHz: p.searchHz,
      signalDb: 6,
      centroidDb: 6,
    };
    const floorBin = floorEstimate(input, 0.2);
    const chans = h.ch.map((id, j) => {
      const m = channelMetrics(input, floorBin, h.offsetHz[j]);
      for (const sp of p.spursKhz)
        if (Math.abs((sp * 1000 - h.loKhz * 1000) - h.offsetHz[j]) <= p.searchHz) m.flags |= CHF.SPUR;
      const lin = 10 ** (m.rssi / 10);
      const st = this.state.get(id) ?? { ema: lin, clear: null };
      st.ema += this.ema * (lin - st.ema);
      if (!(m.flags & CHF.SIGNAL)) st.clear = st.clear === null ? lin : st.clear + this.clearEma * (lin - st.clear);
      this.state.set(id, st);
      return { id, m, ema: db(st.ema), clear: st.clear === null ? null : db(st.clear) };
    });
    const tune = 900 + Math.random() * 200;
    const buf = new DataView(new ArrayBuffer(34 + 26 * chans.length));
    buf.setUint32(0, this.seq++, true);
    buf.setUint32(4, Math.floor(now - this.start), true);
    buf.setUint32(8, h.loKhz, true);
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
    // Same LO rotation as the firmware: one spectrum per specEvery hops.
    if (this.specEvery && this.hop % this.specEvery === this.rotations % this.specEvery) this.spectrum(h.loKhz, psd, binHz);
    this.onBytes(encodeFrame(FRAME_METRICS, new Uint8Array(buf.buffer)));
    this.hopsTotal++;
    this.winHops++;
    if (++this.hop >= this.plan.length) {
      this.hop = 0;
      this.rotations++;
      this.winRot++;
    }
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
    const out = new DataView(new ArrayBuffer(62));
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
    out.setUint8(59, 1);
    out.setUint8(60, this.gainManual ? this.gain : 255);
    out.setInt8(61, 60);
    this.winHops = this.winRot = 0;
    this.onBytes(encodeFrame(FRAME_TELEMETRY, new Uint8Array(out.buffer)));
  }

  private rawiq(loKhz: number, n: number) {
    const p = this.profile;
    const now = performance.now();
    const rate = p.rateHz;
    const out = new Uint8Array(12 + 2 * n);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, loKhz, true);
    dv.setUint32(4, rate, true);
    dv.setUint16(8, n, true);
    dv.setUint8(10, 1);
    dv.setUint8(11, 2);
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
      dv.setInt8(12 + 2 * j, Math.max(-128, Math.min(127, Math.round(i))));
      dv.setInt8(13 + 2 * j, Math.max(-128, Math.min(127, Math.round(q))));
    }
    this.onBytes(encodeFrame(FRAME_RAWIQ, out));
  }
}
