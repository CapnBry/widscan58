// Power meter mode: channel power in dBm from the C5's dBFS, with the board's
// calibration and the user's attenuator. GPL-3.0-or-later.
import type { Series } from "./store";

/** The scan the calibration was measured with (tools/powercal.py): one LO
 *  per channel, manual gain 0, RSSI window +/- 15 MHz, no display spectrum. */
export const POWER_GAIN = 0;
export const POWER_WIN_HZ = 15000000;
export const powerCommands = (usableHz: number) => [
  "MODE SINGLE",
  `GAIN MANUAL ${POWER_GAIN}`,
  `WIN ${POWER_WIN_HZ} ${POWER_WIN_HZ}`,
  "METRICS FULL",
  `SPEC 0 256 ${usableHz} MAX`,
];

/** Monitor mode's windows and spectrum per profile, put back on leaving
 *  power meter mode when no snapshot of the user's own is at hand. */
export const MONITOR_DEFAULTS: Record<string, { rssiHz: number; searchHz: number; spec: string }> = {
  "c5-race": { rssiHz: 5000000, searchHz: 12000000, spec: "SPEC 4 256 40000000 MAX" },
  "s3-race24": { rssiHz: 1410000, searchHz: 3390000, spec: "SPEC 4 256 11300000 MAX" },
};

/** dBm at the board's RF input minus dBFS at POWER_GAIN, by channel MHz.
 *  From 2026-10-19 (PLAN.md "Power calibration"): R1-R8 against an
 *  ImmersionRC RF Power Meter v2, the mean of 25/100/400 mW, about +/- 1 dB
 *  (the C5's steps between power levels read ~0.8 of the meter's). Outside
 *  the measured points the offset stays at the nearest one's value and,
 *  beyond 10 MHz, the reading counts as uncalibrated. R1-R5 are 0.5 dB
 *  lower than measured: the calibration was made at ADC rate 0, and the
 *  firmware now runs rate 1 at every LO, which reads ~0.3-1.2 dB higher there
 *  (PLAN.md, "The 5830/5831 MHz skirt"); R6-R8 already ran rate 1. An
 *  estimate until the calibration is redone. */
export const CALIBRATION: Record<string, [number, number][]> = {
  "c5-race": [
    [5658, 27.4],
    [5695, 27.3],
    [5732, 26.8],
    [5769, 26.1],
    [5806, 25.9],
    [5843, 25.3],
    [5880, 25.2],
    [5917, 26.1],
  ],
};

/** Measured range at the board's input: the noise floor in the window is
 *  about -32 dBm; the strongest input measured without compression -5.4 dBm. */
export const INPUT_MAX_DBM = -5;

export interface Offset {
  db: number;
  calibrated: boolean; // within the measured frequencies
}

export function calOffset(profile: string, mhz: number): Offset | null {
  const pts = CALIBRATION[profile];
  if (!pts?.length) return null;
  const [lo, hi] = [pts[0][0], pts[pts.length - 1][0]];
  if (mhz <= lo) return { db: pts[0][1], calibrated: mhz >= lo - 10 };
  if (mhz >= hi) return { db: pts[pts.length - 1][1], calibrated: mhz <= hi + 10 };
  for (let j = 1; j < pts.length; j++) {
    const [f0, a] = pts[j - 1];
    const [f1, b] = pts[j];
    if (mhz <= f1) return { db: a + ((b - a) * (mhz - f0)) / (f1 - f0), calibrated: true };
  }
  return null;
}

/** Smoothing choices (seconds of averaging; 0: the latest hop). */
export const SMOOTHING: [number, string][] = [
  [0, "Off"],
  [0.1, "0.1 s"],
  [0.5, "0.5 s"],
  [1, "1 s"],
  [3, "3 s"],
];

/** Mean power (linear) of a dBFS series over the last `seconds`, in dBFS,
 *  leaving out the values at the times in `skip`; the newest value when
 *  seconds is 0 or nothing falls in the window. */
export function meanDb(series: Series, now: number, seconds: number, skip?: Set<number>): number | null {
  if (!series.size) return null;
  const newest = series.v[(series.head - 1 + series.capacity) % series.capacity];
  if (seconds <= 0) return newest;
  let sum = 0;
  let n = 0;
  series.forEach((t, v) => {
    if (now - t <= seconds && !skip?.has(t)) {
      sum += 10 ** (v / 10);
      n++;
    }
  });
  return n ? 10 * Math.log10(sum / n) : newest;
}

/** The times of the clipped captures (a 0/1 series) in the last `seconds`,
 *  and the share of the window's captures they are. */
export function clippedIn(clipped: Series, now: number, seconds: number): { times: Set<number>; share: number } {
  const times = new Set<number>();
  let n = 0;
  clipped.forEach((t, v) => {
    if (now - t > seconds) return;
    n++;
    if (v) times.add(t);
  });
  return { times, share: n ? times.size / n : 0 };
}

/** Signal power without the window's noise, dBFS; null within 3 dB of it. */
export function lessNoise(db: number, noiseDb: number | null): number | null {
  if (noiseDb === null) return db;
  const p = 10 ** (db / 10) - 10 ** (noiseDb / 10);
  return p > 0 && db - noiseDb >= 3 ? 10 * Math.log10(p) : null;
}

export function formatMw(dbm: number): string {
  const mw = 10 ** (dbm / 10);
  if (mw >= 1000) return `${(mw / 1000).toFixed(mw >= 10000 ? 1 : 2)} W`;
  if (mw >= 100) return `${mw.toFixed(0)} mW`;
  if (mw >= 10) return `${mw.toFixed(1)} mW`;
  if (mw >= 1) return `${mw.toFixed(2)} mW`;
  return `${mw.toFixed(3)} mW`;
}

/** Attenuator sizes for the warning table: the board's input stays at or
 *  below INPUT_MAX_DBM with 2 dB to spare for a VTX above its label. The
 *  first attenuator takes nearly all the power: rated at least twice it. */
export const ATTENUATOR_TABLE: { vtx: string; dbm: number; attenDb: number; rating: string }[] = [
  { vtx: "25 mW", dbm: 14, attenDb: 30, rating: "0.5 W" },
  { vtx: "500 mW", dbm: 27, attenDb: 40, rating: "1 W" },
  { vtx: "1 W", dbm: 30, attenDb: 40, rating: "2 W" },
  { vtx: "2 W", dbm: 33, attenDb: 40, rating: "5 W" },
];
