// Gate-crossing detection for the simulator's RotorHazard seats: the same
// behaviour as firmware/src/scan/lap.c (median of 3, crossing from EnterAt
// to below ExitAt, pass at the middle of the peak plateau, peak/nadir
// history with hysteresis). GPL-3.0-or-later.

const HISTORY = 8;
const HYSTERESIS = 2;

export interface Extremum {
  level: number;
  firstMs: number;
  durationMs: number;
}

export class LapSeat {
  enterAt: number;
  exitAt: number;
  private input: { v: number; t: number }[] = [];
  valid = false;
  level = 0;
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
    this.input = [];
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

  feed(tMs: number, level: number, loopUs: number) {
    this.loopUs = Math.min(65535, loopUs);
    this.input.push({ v: level, t: tMs });
    if (this.input.length > 3) this.input.shift();
    if (this.input.length < 3) return;
    const m = [...this.input].map((x) => x.v).sort((a, b) => a - b)[1];
    this.filtered(this.input[1].t, m);
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
