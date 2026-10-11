// Wire protocol v1. Mirrors firmware/src/proto/frames.h. GPL-3.0-or-later.

export const FRAME_METRICS = 1;
export const FRAME_SPECTRUM = 2;
export const FRAME_TELEMETRY = 3;
export const FRAME_RAWIQ = 4;
export const FRAME_LOG = 5;
export const FRAME_LEVELS = 6;

// What the device measures per channel (METRICS command). FULL sends
// FRAME_METRICS; RSSI and SNR send FRAME_LEVELS, which only has the channel
// power, plus floor and SNR in SNR mode. Absent values parse as null.
export type MetricsMode = "FULL" | "RSSI" | "SNR";
const LEVELS_MODES: MetricsMode[] = ["FULL", "RSSI", "SNR"];

export const CHF = {
  SIGNAL: 1,
  SATURATED: 2,
  MASKED: 4, // DC or spur bins excluded in the search window
  EDGE: 8,
  NO_CENTROID: 16,
  SPUR: 32, // FULL: a known spur masked in the search window; RSSI/SNR: one in the RSSI window, unmasked
} as const;

export interface ChannelMetrics {
  id: number;
  flags: number;
  rssi: number; // dBFS
  // null: not measured in this metrics mode.
  rssiEma: number | null;
  floor: number | null; // noise in the RSSI window, dBFS
  snr: number | null; // (S+N)/N dB
  clearFloor: number | null; // also null before the channel was ever clear
  peak: number | null; // per-bin dBFS, this hop
  // Time-averaged occupancy, vs channel nominal: most occupied frequency,
  // power-weighted center, and the band holding 99 % of the excess power.
  peakOffHz: number | null;
  centroidOffHz: number | null;
  obwLoHz: number | null;
  obwHiHz: number | null;
}

export interface Metrics {
  seq: number;
  tMs: number;
  loKhz: number;
  tuneUs: number;
  captureUs: number;
  dspUs: number;
  saturated: number;
  mode: MetricsMode;
  floorBin: number | null; // dBFS per FFT bin
  dc: number | null; // dBFS
  gain: number; // 255 = hardware AGC
  hopIndex: number;
  hops: number;
  channels: ChannelMetrics[];
}

export interface Spectrum {
  seq: number;
  loKhz: number;
  startOffHz: number;
  binHz: number;
  detector: number;
  db: Float32Array; // dBFS per display bin
}

export interface Telemetry {
  tMs: number;
  rotations: number;
  hopsTotal: number;
  rotationHz: number;
  hopUsAvg: number;
  hopUsMax: number;
  tuneUsAvg: number;
  tuneUsMax: number;
  captureUsAvg: number;
  dspUsAvg: number;
  dspUsMax: number;
  linkFill: number; // 0..1
  heapFree: number;
  drops: number;
  captureErrors: number;
  running: boolean;
  recalMode: number;
  gain: number;
  filter: number;
}

export interface RawIq {
  loKhz: number;
  rateHz: number;
  n: number;
  conjugated: boolean;
  shift: number; // samples are 10-bit values >> shift (firmware picks the smallest that fits 8 bits)
  i: Int8Array;
  q: Int8Array;
}

const METRICS_HDR = 34; // sizeof(frame_metrics_t)
const CHANNEL_SIZE = 26; // sizeof(frame_channel_t)

export function parseMetrics(p: DataView): Metrics {
  const m: Metrics = {
    seq: p.getUint32(0, true),
    tMs: p.getUint32(4, true),
    loKhz: p.getUint32(8, true),
    tuneUs: p.getUint32(12, true),
    captureUs: p.getUint32(16, true),
    dspUs: p.getUint32(20, true),
    saturated: p.getUint16(24, true),
    mode: "FULL",
    floorBin: p.getInt16(26, true) / 100,
    dc: p.getInt16(28, true) / 100,
    gain: p.getUint8(30),
    hopIndex: p.getUint8(31),
    hops: p.getUint8(32),
    channels: [],
  };
  const nch = p.getUint8(METRICS_HDR - 1);
  for (let j = 0; j < nch; j++) {
    const o = METRICS_HDR + j * CHANNEL_SIZE;
    if (o + CHANNEL_SIZE > p.byteLength) break;
    const clear = p.getInt16(o + 10, true);
    m.channels.push({
      id: p.getUint8(o),
      flags: p.getUint8(o + 1),
      rssi: p.getInt16(o + 2, true) / 100,
      rssiEma: p.getInt16(o + 4, true) / 100,
      floor: p.getInt16(o + 6, true) / 100,
      snr: p.getInt16(o + 8, true) / 100,
      clearFloor: clear === -32768 ? null : clear / 100,
      peak: p.getInt16(o + 12, true) / 100,
      peakOffHz: p.getInt32(o + 14, true),
      centroidOffHz: p.getInt32(o + 18, true),
      obwLoHz: p.getInt16(o + 22, true) * 1000,
      obwHiHz: p.getInt16(o + 24, true) * 1000,
    });
  }
  return m;
}

const LEVELS_HDR = 32; // sizeof(frame_levels_t)
const LEVEL_SIZE = 4; // sizeof(frame_level_t)
// Level codes (0.25 dB): dBFS = code / 4 - 63.75; SNR dB = code / 4. The
// header's per-bin floor is a spectrum code: dBFS = code / 2 - 127.5.
const levelDb = (code: number) => code / 4 - 63.75;
const specDb = (code: number) => code / 2 - 127.5;

export function parseLevels(p: DataView): Metrics {
  const mode = LEVELS_MODES[p.getUint8(29)] ?? "RSSI";
  const snrMode = mode === "SNR";
  const m: Metrics = {
    seq: p.getUint32(0, true),
    tMs: p.getUint32(4, true),
    loKhz: p.getUint32(8, true),
    tuneUs: p.getUint32(12, true),
    captureUs: p.getUint32(16, true),
    dspUs: p.getUint32(20, true),
    saturated: p.getUint16(24, true),
    gain: p.getUint8(26),
    hopIndex: p.getUint8(27),
    hops: p.getUint8(28),
    mode,
    floorBin: snrMode ? specDb(p.getUint8(30)) : null,
    dc: null,
    channels: [],
  };
  const nch = p.getUint8(LEVELS_HDR - 1);
  for (let j = 0; j < nch; j++) {
    const o = LEVELS_HDR + j * LEVEL_SIZE;
    if (o + LEVEL_SIZE > p.byteLength) break;
    const rssi = levelDb(p.getUint8(o + 2));
    const snr = snrMode ? p.getUint8(o + 3) / 4 : null;
    m.channels.push({
      id: p.getUint8(o),
      flags: p.getUint8(o + 1),
      rssi,
      rssiEma: null,
      floor: snr === null ? null : rssi - snr,
      snr,
      clearFloor: null,
      peak: null,
      peakOffHz: null,
      centroidOffHz: null,
      obwLoHz: null,
      obwHiHz: null,
    });
  }
  return m;
}

export function parseSpectrum(p: DataView): Spectrum {
  const n = p.getUint16(16, true);
  const db = new Float32Array(n);
  for (let j = 0; j < n; j++) db[j] = p.getUint8(20 + j) / 2 - 127.5;
  return {
    seq: p.getUint32(0, true),
    loKhz: p.getUint32(4, true),
    startOffHz: p.getInt32(8, true),
    binHz: p.getUint32(12, true),
    detector: p.getUint8(18),
    db,
  };
}

export function parseTelemetry(p: DataView): Telemetry {
  return {
    tMs: p.getUint32(0, true),
    rotations: p.getUint32(4, true),
    hopsTotal: p.getUint32(8, true),
    rotationHz: p.getUint32(12, true) / 1000,
    hopUsAvg: p.getUint32(16, true),
    hopUsMax: p.getUint32(20, true),
    tuneUsAvg: p.getUint32(24, true),
    tuneUsMax: p.getUint32(28, true),
    captureUsAvg: p.getUint32(32, true),
    dspUsAvg: p.getUint32(36, true),
    dspUsMax: p.getUint32(40, true),
    linkFill: p.getUint16(44, true) / 1000,
    heapFree: p.getUint32(46, true),
    drops: p.getUint32(50, true),
    captureErrors: p.getUint32(54, true),
    running: p.getUint8(58) !== 0,
    recalMode: p.getUint8(59),
    gain: p.getUint8(60),
    filter: p.getInt8(61),
  };
}

// RAWIQ arrives in consecutive chunks: the header (n = whole snapshot, offset
// = first sample in this frame), then 8-bit I/Q pairs.
const RAWIQ_HDR = 14;
export interface RawIqChunk {
  loKhz: number;
  rateHz: number;
  n: number;
  offset: number;
  count: number;
  conjugated: boolean;
  shift: number;
  p: DataView;
}

export function parseRawIqChunk(p: DataView): RawIqChunk {
  return {
    loKhz: p.getUint32(0, true),
    rateHz: p.getUint32(4, true),
    n: p.getUint16(8, true),
    offset: p.getUint16(10, true),
    count: (p.byteLength - RAWIQ_HDR) >> 1,
    conjugated: p.getUint8(12) !== 0,
    shift: p.getUint8(13),
    p,
  };
}

/** Whether this chunk ends its snapshot. */
export function rawIqChunkIsLast(p: DataView): boolean {
  const c = parseRawIqChunk(p);
  return c.offset + c.count >= c.n;
}

/** Collects chunks into a snapshot; add() returns it once the last one is in. */
export class RawIqAssembler {
  private cur: RawIq | null = null;
  private next = 0;

  add(p: DataView): RawIq | null {
    const c = parseRawIqChunk(p);
    if (c.offset === 0) {
      this.cur = { loKhz: c.loKhz, rateHz: c.rateHz, n: c.n, conjugated: c.conjugated, shift: c.shift, i: new Int8Array(c.n), q: new Int8Array(c.n) };
      this.next = 0;
    }
    const r = this.cur;
    if (!r || c.offset !== this.next || c.n !== r.n || c.offset + c.count > r.n) {
      this.cur = null; // a chunk went missing: drop the snapshot
      return null;
    }
    for (let j = 0; j < c.count; j++) {
      r.i[c.offset + j] = p.getInt8(RAWIQ_HDR + 2 * j);
      r.q[c.offset + j] = p.getInt8(RAWIQ_HDR + 1 + 2 * j);
    }
    this.next = c.offset + c.count;
    if (this.next < r.n) return null;
    this.cur = null;
    return r;
  }
}

// Profile / status JSON (text replies).
export interface ProfileChannel {
  id: number;
  name: string;
  khz: number;
}
export interface Profile {
  name: string;
  description: string;
  target: string;
  band_khz: [number, number];
  base_khz: number;
  equiv_base_khz: number;
  scale: number;
  rate_hz: number;
  usable_hz: number;
  passband_hz: number;
  rssi_hz: number;
  search_hz: number;
  channels: ProfileChannel[];
}
export interface PlanHop {
  lo_khz: number;
  ch: number[];
  offset_hz: number[];
}
export interface Status {
  profile: string;
  running: number;
  enabled: number;
  mode: "GROUP" | "SINGLE";
  capture_samples: number;
  nfft: number;
  segments: number;
  rate_code: number;
  filter: number;
  conjugate: number;
  iq_corr: number;
  settle_us: number;
  gain_mode: "MANUAL" | "HARDWARE";
  gain: number;
  gain_max: number;
  rawiq_max?: number; // largest RAWIQ snapshot (the capture buffer)
  recal: "ONCE" | "EVERY" | "CACHE";
  metrics?: MetricsMode; // absent from firmware before METRICS existed
  rssi_hz: number;
  search_hz: number;
  dc_guard: number;
  ema: number;
  clear_ema: number;
  signal_db: number;
  avg_shift: number;
  spec_every: number;
  spec_bins: number;
  spec_span_hz: number;
  spec_detector: number;
  plan: PlanHop[];
}
