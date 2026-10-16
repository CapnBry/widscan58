// Gate-crossing detection for the simulator's RotorHazard seats: the same
// behaviour as firmware/src/scan/lap.c (255 ms running median from a
// histogram of the level codes, crossing from EnterAt to below ExitAt, pass at
// the middle of the peak plateau, peak/nadir history with hysteresis), and
// its "EMA" filter (median of 3, then an exponential average with a fixed
// time constant whatever the update rate). GPL-3.0-or-later.

const HISTORY = 8;
const HYSTERESIS = 4; // level codes: 1 dB
const WINDOW_MS = 255; // median window
const MAX_SAMPLES = 255; // window cap: one byte per histogram bin
const TS_EVERY = 8; // updates per stored timestamp
const TS_SLOTS = (MAX_SAMPLES + 1) / TS_EVERY + 2;
const EMA_TAU_US = 2720; // alpha 0.3 at 1030 updates/s

export type LapFilter = "MEDIAN" | "EMA";

export interface Extremum {
  level: number;
  firstMs: number;
  durationMs: number;
}

export class LapSeat {
  enterAt: number;
  exitAt: number;
  filter: LapFilter = "MEDIAN";
  private ema = 0;
  // Median window: updates first .. seq - 1 (update k is ring[k & 255]), a
  // histogram of their levels, and the lower middle level with `below` under it.
  private ring = new Uint8Array(256);
  private bins = new Uint16Array(256);
  private ts = new Float64Array(TS_SLOTS); // time of update k, for k % TS_EVERY == 0
  private seq = 0;
  private first = 0;
  private lastMs = 0;
  private med = 0;
  private below = 0;
  private filled = false;
  valid = false;
  level = 0;
  levelMs = 0;
  nodePeak = 0;
  nodeNadir = 255;
  crossing = false;
  lap = 0;
  passMs = 0;
  passPeak = 0;
  passNadir = 255;
  loopUs = 0;
  private curPeak = 0;
  private curFirst = 0;
  private curLast = 0;
  private dir = 1;
  private ext: Extremum = { level: 0, firstMs: 0, durationMs: 0 };
  private extLast = 0;
  peaks: Extremum[] = [];
  nadirs: Extremum[] = [];

  constructor(enterAt: number, exitAt: number) {
    this.enterAt = enterAt;
    this.exitAt = exitAt;
  }

  /** After a frequency change: keeps thresholds, lap count and last pass. */
  reset() {
    this.bins.fill(0);
    this.seq = this.first = 0;
    this.med = this.below = 0;
    this.filled = false;
    this.valid = false;
    this.level = 0;
    this.nodePeak = 0;
    this.nodeNadir = 255;
    this.crossing = false;
    this.passNadir = 255;
    this.dir = 1;
    this.peaks = [];
    this.nadirs = [];
    this.loopUs = 0;
  }

  setFilter(f: LapFilter) {
    if (f === this.filter) return;
    this.filter = f;
    this.reset();
  }

  /** Time of update k in the window: stored every TS_EVERY updates, interpolated between. */
  private timeOf(k: number): number {
    const k0 = k - (k % TS_EVERY);
    const t0 = this.ts[(k0 / TS_EVERY) % TS_SLOTS];
    if (k === k0) return t0;
    let k1 = k0 + TS_EVERY;
    let t1: number;
    if (k1 < this.seq) t1 = this.ts[(k1 / TS_EVERY) % TS_SLOTS];
    else {
      k1 = this.seq - 1;
      t1 = this.lastMs;
    }
    return t0 + Math.floor(((t1 - t0) * (k - k0)) / (k1 - k0));
  }

  private dropOldest() {
    const v = this.ring[this.first & 255];
    this.bins[v]--;
    if (v < this.med) this.below--;
    this.first++;
  }

  feed(tMs: number, level: number, loopUs: number) {
    this.loopUs = Math.min(65535, loopUs);
    if (this.filter === "EMA") return this.feedEma(tMs, level, loopUs);
    if (this.seq - this.first === MAX_SAMPLES) {
      this.dropOldest();
      this.filled = true;
    }
    const k = this.seq++;
    if (k % TS_EVERY === 0) this.ts[(k / TS_EVERY) % TS_SLOTS] = tMs;
    this.lastMs = tMs;
    this.ring[k & 255] = level;
    this.bins[level]++;
    if (level < this.med) this.below++;
    while (this.seq - this.first > 1 && tMs - this.timeOf(this.first) >= WINDOW_MS) {
      this.dropOldest();
      this.filled = true;
    }
    // Move the median to the lower middle level of the n updates in the window.
    const r = Math.floor((this.seq - this.first - 1) / 2);
    while (this.below > r) this.below -= this.bins[--this.med];
    while (this.below + this.bins[this.med] <= r) this.below += this.bins[this.med++];
    if (!this.filled) return;
    this.filtered(this.timeOf(this.first + r), this.med);
  }

  // The firmware works in integers (Q8 average, a Padé weight); this is the exact form.
  private feedEma(tMs: number, level: number, loopUs: number) {
    const k = this.seq++;
    const prevMs = this.lastMs;
    this.ring[k & 255] = level;
    this.lastMs = tMs;
    if (k < 2) {
      this.ema = level;
      return this.filtered(tMs, level);
    }
    const med = [level, this.ring[(k - 1) & 255], this.ring[(k - 2) & 255]].sort((x, y) => x - y)[1];
    this.ema += (1 - Math.exp(-loopUs / EMA_TAU_US)) * (med - this.ema);
    this.filtered(prevMs - Math.round(EMA_TAU_US / 1000), Math.round(this.ema));
  }

  endCrossing() {
    if (this.crossing) this.end();
  }

  private end() {
    this.crossing = false;
    this.lap = (this.lap + 1) & 0xff;
    this.passMs = this.curFirst + Math.floor((this.curLast - this.curFirst) / 2);
    this.passPeak = this.curPeak;
    this.passNadir = this.level;
  }

  private close(q: Extremum[]) {
    q.push({ ...this.ext, durationMs: Math.min(65535, this.extLast - this.ext.firstMs) });
    if (q.length > HISTORY) q.shift();
  }

  private filtered(t: number, v: number) {
    this.level = v;
    this.levelMs = t;
    this.nodePeak = Math.max(this.nodePeak, v);
    this.nodeNadir = Math.min(this.nodeNadir, v);
    if (!this.valid || (this.dir > 0 ? v > this.ext.level : v < this.ext.level)) {
      this.ext = { level: v, firstMs: t, durationMs: 0 };
      this.extLast = t;
    } else if (v === this.ext.level) {
      this.extLast = t;
    } else if (this.dir > 0 ? v + HYSTERESIS <= this.ext.level : v >= this.ext.level + HYSTERESIS) {
      this.close(this.dir > 0 ? this.peaks : this.nadirs);
      this.dir = -this.dir;
      this.ext = { level: v, firstMs: t, durationMs: 0 };
      this.extLast = t;
    }
    this.valid = true;
    if (!this.crossing && v >= this.enterAt) {
      this.crossing = true;
      this.curPeak = v;
      this.curFirst = this.curLast = t;
    } else if (this.crossing) {
      if (v > this.curPeak) {
        this.curPeak = v;
        this.curFirst = this.curLast = t;
      } else if (v === this.curPeak) this.curLast = t;
      if (v < this.exitAt) this.end();
    }
    if (!this.crossing && v < this.passNadir) this.passNadir = v;
  }

  /** The oldest queued peak or nadir. */
  popExtremum(): { peak: boolean; e: Extremum } | null {
    const p = this.peaks[0];
    const n = this.nadirs[0];
    if (!p && !n) return null;
    const peak = !!p && (!n || p.firstMs < n.firstMs);
    return { peak, e: (peak ? this.peaks : this.nadirs).shift()! };
  }
}
