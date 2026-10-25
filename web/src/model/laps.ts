// Laps of one lap timer seat: a pass closer than the minimum lap to the
// previous counted pass is ignored (a false or double detection), and the
// fastest `count` consecutive laps are kept. A break (Clear, a channel change,
// a device restart or a reconnect) ends the run: the next pass has no lap
// time. The pass times since the last clear are kept, so a change of the rules
// recomputes everything from them. GPL-3.0-or-later.

export interface LapRules {
  fastestOn: boolean; // keep the fastest run
  count: number; // consecutive laps in a fastest run
  minLapOn: boolean; // ignore short laps
  minLapS: number; // shorter laps are ignored
}
export const DEFAULT_LAP_RULES: LapRules = { fastestOn: true, count: 3, minLapOn: true, minLapS: 5 };

export interface FastestRun {
  totalS: number;
  laps: number[];
}

export type PassResult =
  | { ignored: true; sinceS: number } // since the previous counted pass
  | { ignored: false; lapS?: number; fastest?: FastestRun; previous?: FastestRun | null }; // fastest: a new record

export class LapTracker {
  passes = 0; // counted passes since the last clear
  lastLapS: number | null = null;
  fastest: FastestRun | null = null;
  private last: number | null = null; // the last counted pass
  private run: number[] = []; // the current run's last `count` laps
  private history: (number | null)[] = []; // pass times since the last clear; null: a break

  pass(t: number, rules: LapRules): PassResult {
    this.history.push(t);
    return this.step(t, rules);
  }

  /** End the current run: the next pass starts a new one. */
  break() {
    if (this.history.length && this.history[this.history.length - 1] !== null) this.history.push(null);
    this.endRun();
  }

  clear() {
    this.history = [];
    this.reset();
  }

  recompute(rules: LapRules) {
    this.reset();
    for (const t of this.history) {
      if (t === null) this.endRun();
      else this.step(t, rules);
    }
  }

  private step(t: number, rules: LapRules): PassResult {
    if (rules.minLapOn && this.last !== null && t - this.last < rules.minLapS) return { ignored: true, sinceS: t - this.last };
    const lapS = this.last !== null ? t - this.last : undefined;
    this.last = t;
    this.passes++;
    this.lastLapS = lapS ?? null;
    if (lapS === undefined) return { ignored: false };
    if (!rules.fastestOn) return { ignored: false, lapS };
    this.run.push(lapS);
    if (this.run.length > rules.count) this.run.shift();
    if (this.run.length < rules.count) return { ignored: false, lapS };
    const totalS = this.run.reduce((a, b) => a + b, 0);
    if (this.fastest && totalS >= this.fastest.totalS) return { ignored: false, lapS };
    const previous = this.fastest;
    this.fastest = { totalS, laps: [...this.run] };
    return { ignored: false, lapS, fastest: this.fastest, previous };
  }

  private endRun() {
    this.last = null;
    this.run = [];
  }

  private reset() {
    this.passes = 0;
    this.lastLapS = null;
    this.fastest = null;
    this.endRun();
  }
}
