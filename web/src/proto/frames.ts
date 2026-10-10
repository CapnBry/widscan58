// Wire protocol v1. Mirrors firmware/src/proto/frames.h. GPL-3.0-or-later.

export const FRAME_METRICS = 1;
export const FRAME_SPECTRUM = 2;
export const FRAME_TELEMETRY = 3;
export const FRAME_RAWIQ = 4;
export const FRAME_LOG = 5;

export const CHF = {
  SIGNAL: 1,
  SATURATED: 2,
  MASKED: 4, // DC or spur bins excluded in the search window
  EDGE: 8,
  NO_CENTROID: 16,
  SPUR: 32,
} as const;

export interface ChannelMetrics {
  id: number;
  flags: number;
  rssi: number; // dBFS
  rssiEma: number;
  floor: number; // noise in the RSSI window, dBFS
  snr: number; // (S+N)/N dB
  clearFloor: number | null;
  peak: number; // per-bin dBFS, this hop
  // Time-averaged occupancy, vs channel nominal: most occupied frequency,
  // power-weighted centre, and the band holding 99 % of the excess power.
  peakOffHz: number;
  centroidOffHz: number;
  obwLoHz: number;
  obwHiHz: number;
}

export interface Metrics {
  seq: number;
  tMs: number;
  loKhz: number;
  tuneUs: number;
  captureUs: number;
  dspUs: number;
  saturated: number;
  floorBin: number; // dBFS per FFT bin
  dc: number; // dBFS
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

export function parseRawIq(p: DataView): RawIq {
  const n = p.getUint16(8, true);
  const i = new Int8Array(n);
  const q = new Int8Array(n);
  for (let j = 0; j < n; j++) {
    i[j] = p.getInt8(12 + 2 * j);
    q[j] = p.getInt8(13 + 2 * j);
  }
  return { loKhz: p.getUint32(0, true), rateHz: p.getUint32(4, true), n, conjugated: p.getUint8(10) !== 0, shift: p.getUint8(11), i, q };
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
  rawiq_max?: number; // largest RAWIQ the link can carry in one frame
  recal: "ONCE" | "EVERY" | "CACHE";
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
