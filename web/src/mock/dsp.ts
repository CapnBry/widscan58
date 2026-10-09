// TypeScript ports of the firmware's planner and metrics (firmware/src/scan),
// used by the mock device. Keep in step with the C versions. GPL-3.0-or-later.
import { CHF } from "../proto/frames";

export interface ProfileDef {
  name: string;
  description: string;
  target: string;
  bandLoKhz: number;
  bandHiKhz: number;
  baseKhz: number;
  equivBaseKhz: number;
  scale: number;
  rateHz: number;
  nfft: number;
  segments: number;
  usableHz: number;
  passbandHz: number;
  rssiHz: number;
  searchHz: number;
  pairMaxSpacingKhz: number;
  singleLoOffsetKhz: number;
  loStepKhz: number;
  channels: { name: string; khz: number }[];
  spursKhz: number[];
}

export interface Hop {
  loKhz: number;
  ch: number[];
  offsetHz: number[];
}

export function planBuild(p: ProfileDef, mask: number, pair: boolean): Hop[] {
  const order = p.channels.map((c, i) => ({ i, khz: c.khz })).filter((c) => mask & (1 << c.i));
  order.sort((a, b) => a.khz - b.khz);
  const round = (khz: number) => (p.loStepKhz <= 1 ? Math.trunc(khz) : Math.floor((khz + p.loStepKhz / 2) / p.loStepKhz) * p.loStepKhz);
  const hops: Hop[] = [];
  for (let j = 0; j < order.length; ) {
    const a = order[j];
    if (pair && j + 1 < order.length && order[j + 1].khz - a.khz <= p.pairMaxSpacingKhz) {
      const b = order[j + 1];
      const lo = round(Math.trunc((a.khz + b.khz) / 2));
      hops.push({ loKhz: lo, ch: [a.i, b.i], offsetHz: [(a.khz - lo) * 1000, (b.khz - lo) * 1000] });
      j += 2;
    } else {
      const lo = round(a.khz + p.singleLoOffsetKhz);
      hops.push({ loKhz: lo, ch: [a.i], offsetHz: [(a.khz - lo) * 1000] });
      j += 1;
    }
  }
  return hops;
}

export function percentileBias(segments: number, q: number): number {
  let lo = 0;
  let hi = 10 * segments + 50;
  for (let it = 0; it < 100; it++) {
    const x = (lo + hi) / 2;
    let term = 1;
    let sum = 0;
    for (let i = 0; i < segments; i++) {
      sum += term;
      term *= x / (i + 1);
    }
    if (1 - Math.exp(-x) * sum < q) lo = x;
    else hi = x;
  }
  return segments / ((lo + hi) / 2);
}

export const db = (x: number) => (x > 1e-20 ? 10 * Math.log10(x) : -200);

export interface MetricsInput {
  p: Float64Array;
  valid: Uint8Array;
  binHz: number;
  enbwBins: number;
  floorBias: number;
  passbandHz: number;
  rssiHz: number;
  searchHz: number;
  signalDb: number;
  centroidDb: number;
}

export function floorEstimate(m: MetricsInput, q: number): number {
  const n = m.p.length;
  const vals: number[] = [];
  for (let k = 0; k < n; k++) if (m.valid[k] && Math.abs((k - n / 2) * m.binHz) <= m.passbandHz) vals.push(m.p[k]);
  if (!vals.length) return 0;
  vals.sort((a, b) => a - b);
  return vals[Math.floor(q * (vals.length - 1))] * m.floorBias;
}

export function channelMetrics(m: MetricsInput, floorBin: number, offHz: number) {
  const n = m.p.length;
  const half = n / 2;
  const f = (k: number) => (k - half) * m.binHz;
  let k0 = half + Math.floor((offHz - m.searchHz) / m.binHz);
  let k1 = half + Math.ceil((offHz + m.searchHz) / m.binHz);
  k0 = Math.max(0, k0);
  k1 = Math.min(n - 1, k1);
  let flags = 0;
  let sum = 0;
  let total = 0;
  let used = 0;
  let peakK = -1;
  let peak = 0;
  for (let k = k0; k <= k1; k++) {
    const inRssi = Math.abs(f(k) - offHz) <= m.rssiHz;
    if (inRssi) total++;
    if (!m.valid[k]) {
      flags |= CHF.MASKED;
      continue;
    }
    if (inRssi) {
      sum += m.p[k];
      used++;
    }
    if (m.p[k] > peak) {
      peak = m.p[k];
      peakK = k;
    }
  }
  if (used && used < total) sum *= total / used;
  const rssi = db(sum / m.enbwBins);
  const floor = db((floorBin * total) / m.enbwBins);
  const snr = rssi - floor;
  if (snr >= m.signalDb) flags |= CHF.SIGNAL;
  if (Math.abs(offHz) + m.rssiHz > m.passbandHz) flags |= CHF.EDGE;
  let peakOff = 0;
  let centroid = 0;
  let obwLo = 0;
  let obwHi = 0;
  if (peakK < 0) flags |= CHF.NO_CENTROID;
  else {
    let frac = 0;
    if (peakK > k0 && peakK < k1 && m.valid[peakK - 1] && m.valid[peakK + 1]) {
      const a = db(m.p[peakK - 1]);
      const b = db(peak);
      const c = db(m.p[peakK + 1]);
      const den = a - 2 * b + c;
      if (den < 0) frac = (0.5 * (a - c)) / den;
    }
    peakOff = f(peakK) + frac * m.binHz - offHz;
    const thresh = floorBin * 10 ** (m.centroidDb / 10);
    let ws = 0;
    let fs = 0;
    for (let k = k0; k <= k1; k++) {
      if (!m.valid[k] || m.p[k] <= thresh) continue;
      const w = m.p[k] - floorBin;
      ws += w;
      fs += w * (f(k) - offHz);
    }
    if (ws > 0) centroid = fs / ws;
    else flags |= CHF.NO_CENTROID;
    const edge = peak * 0.1;
    let l = peakK;
    let r = peakK;
    while (l > k0 && m.valid[l - 1] && m.p[l - 1] > edge) l--;
    while (r < k1 && m.valid[r + 1] && m.p[r + 1] > edge) r++;
    // The mock reports one snapshot's -10 dB run; the firmware averages over time.
    obwLo = f(l) - m.binHz / 2 - offHz;
    obwHi = f(r) + m.binHz / 2 - offHz;
  }
  return { rssi, floor, snr, peak: db(peak), peakOff, centroid, obwLo, obwHi, flags };
}
