// Application state built from the device stream. Views read from here and
// redraw when `version` changes. GPL-3.0-or-later.
import {
  FRAME_LOG,
  FRAME_METRICS,
  FRAME_RAWIQ,
  FRAME_SPECTRUM,
  FRAME_TELEMETRY,
  parseMetrics,
  parseRawIq,
  parseSpectrum,
  parseTelemetry,
  type ChannelMetrics,
  type Metrics,
  type Profile,
  type RawIq,
  type Spectrum,
  type Status,
  type Telemetry,
} from "../proto/frames";
import type { StreamItem } from "../serial/parser";

export const HISTORY_SECONDS = 120;

/** Fixed-capacity time series (seconds, value). */
export class Series {
  t: Float64Array;
  v: Float32Array;
  head = 0;
  size = 0;
  constructor(readonly capacity: number) {
    this.t = new Float64Array(capacity);
    this.v = new Float32Array(capacity);
  }
  push(t: number, v: number) {
    this.t[this.head] = t;
    this.v[this.head] = v;
    this.head = (this.head + 1) % this.capacity;
    if (this.size < this.capacity) this.size++;
  }
  /** Iterate oldest -> newest. */
  forEach(fn: (t: number, v: number) => void) {
    const start = (this.head - this.size + this.capacity) % this.capacity;
    for (let j = 0; j < this.size; j++) {
      const k = (start + j) % this.capacity;
      fn(this.t[k], this.v[k]);
    }
  }
  clear() {
    this.head = this.size = 0;
  }
}

export interface ChannelState {
  latest: ChannelMetrics | null;
  loKhz: number;
  updated: number; // performance.now() of the last metrics
  rssi: Series;
  ema: Series;
  floor: Series;
}

export interface Segment {
  latest: Spectrum;
  maxHold: Float32Array;
  avg: Float32Array; // exponential average, linear power
  updated: number;
}

export interface LogLine {
  t: number;
  dir: "rx" | "tx" | "info" | "error";
  text: string;
}

export class Store {
  version = 0;
  profile: Profile | null = null;
  status: Status | null = null;
  info = "";
  telemetry: Telemetry | null = null;
  lastMetrics: Metrics | null = null;
  channels: ChannelState[] = [];
  segments = new Map<number, Segment>();
  rawiq: RawIq | null = null;
  spursKhz: number[] = [];
  floorByLo = new Map<number, number>(); // per-bin noise floor (dBFS) per LO
  log: LogLine[] = [];
  hopTimes: { tune: number; capture: number; dsp: number }[] = [];
  t0 = performance.now();
  /** Panorama rows for the waterfall: one composite per completed rotation. */
  waterfallVersion = 0;
  private listeners = new Set<() => void>();

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  touch() {
    this.version++;
    for (const fn of this.listeners) fn();
  }

  now(): number {
    return (performance.now() - this.t0) / 1000;
  }

  /** Changes with the profile or its channel plan; per-channel state is keyed by plan index. */
  planKey = "";

  setProfile(p: Profile) {
    const key = `${p.name}/${p.channels.map((c) => `${c.name}=${c.khz}`).join(",")}`;
    const changed = key !== this.planKey;
    this.profile = p;
    this.planKey = key;
    if (changed) {
      const cap = HISTORY_SECONDS * 120;
      this.channels = p.channels.map(() => ({
        latest: null,
        loKhz: 0,
        updated: 0,
        rssi: new Series(cap),
        ema: new Series(cap),
        floor: new Series(cap),
      }));
      this.segments.clear();
    }
    this.touch();
  }

  addLog(dir: LogLine["dir"], text: string) {
    this.log.push({ t: this.now(), dir, text });
    if (this.log.length > 500) this.log.splice(0, this.log.length - 500);
    this.touch();
  }

  resetHolds() {
    for (const s of this.segments.values()) {
      s.maxHold.set(s.latest.db);
      for (let j = 0; j < s.avg.length; j++) s.avg[j] = 10 ** (s.latest.db[j] / 10);
    }
    this.touch();
  }

  ingest(item: StreamItem) {
    if (item.kind === "text") {
      const line = item.line;
      try {
        if (line.startsWith("PROFILE ")) this.setProfile(JSON.parse(line.slice(8)) as Profile);
        else if (line.startsWith("STATUS ")) this.status = JSON.parse(line.slice(7)) as Status;
      } catch (e) {
        this.addLog("error", `bad ${line.split(" ")[0]} reply (${(e as Error).message}); ignored`);
      }
      if (line.startsWith("INFO ")) this.info = line.slice(5);
      else if (line.startsWith("SPURS ")) {
        const f = line.split(" ");
        const n = Number(f[1]);
        this.spursKhz = f.slice(2, 2 + n).map(Number);
      }
      this.addLog(line.startsWith("ERR") ? "error" : "rx", line.length > 300 ? line.slice(0, 300) + " …" : line);
      return;
    }
    switch (item.type) {
      case FRAME_METRICS:
        this.onMetrics(parseMetrics(item.payload));
        break;
      case FRAME_SPECTRUM:
        this.onSpectrum(parseSpectrum(item.payload));
        break;
      case FRAME_TELEMETRY:
        this.telemetry = parseTelemetry(item.payload);
        break;
      case FRAME_RAWIQ:
        this.rawiq = parseRawIq(item.payload);
        break;
      case FRAME_LOG:
        this.addLog("info", new TextDecoder().decode(new Uint8Array(item.payload.buffer)));
        break;
    }
    this.version++;
  }

  private onMetrics(m: Metrics) {
    this.lastMetrics = m;
    this.floorByLo.set(m.loKhz, m.floorBin);
    const t = this.now();
    const wall = performance.now();
    this.hopTimes.push({ tune: m.tuneUs, capture: m.captureUs, dsp: m.dspUs });
    if (this.hopTimes.length > 400) this.hopTimes.splice(0, this.hopTimes.length - 400);
    for (const c of m.channels) {
      const st = this.channels[c.id];
      if (!st) continue;
      st.latest = c;
      st.loKhz = m.loKhz;
      st.updated = wall;
      st.rssi.push(t, c.rssi);
      st.ema.push(t, c.rssiEma);
      st.floor.push(t, c.floor);
    }
  }

  private onSpectrum(s: Spectrum) {
    const seg = this.segments.get(s.loKhz);
    const wall = performance.now();
    if (!seg || seg.maxHold.length !== s.db.length || seg.latest.binHz !== s.binHz) {
      const avg = new Float32Array(s.db.length);
      for (let j = 0; j < avg.length; j++) avg[j] = 10 ** (s.db[j] / 10);
      this.segments.set(s.loKhz, { latest: s, maxHold: Float32Array.from(s.db), avg, updated: wall });
    } else {
      seg.latest = s;
      seg.updated = wall;
      for (let j = 0; j < s.db.length; j++) {
        if (s.db[j] > seg.maxHold[j]) seg.maxHold[j] = s.db[j];
        seg.avg[j] += 0.05 * (10 ** (s.db[j] / 10) - seg.avg[j]);
      }
    }
    // Drop segments whose LO is no longer in the plan (stale after a replan).
    if (this.status) {
      const los = new Set(this.status.plan.map((h) => h.lo_khz));
      for (const [lo, sg] of this.segments) if (!los.has(lo) && wall - sg.updated > 3000) this.segments.delete(lo);
    }
    this.waterfallVersion++;
  }
}
