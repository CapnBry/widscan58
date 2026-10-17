// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { DEFAULT_LAP_RULES, LapTracker, type LapRules } from "./laps";

const rules: LapRules = { ...DEFAULT_LAP_RULES, count: 3, minLapS: 8 };

function feed(tr: LapTracker, times: number[], r = rules) {
  return times.map((t) => tr.pass(t, r));
}

describe("LapTracker", () => {
  it("finds the fastest consecutive laps, posting each new record", () => {
    const tr = new LapTracker();
    const res = feed(tr, [0, 10, 20, 30, 39, 48, 60]);
    expect(res[0]).toEqual({ ignored: false });
    expect(res[2]).toEqual({ ignored: false, lapS: 10 });
    expect(res[3]).toEqual({ ignored: false, lapS: 10, fastest: { totalS: 30, laps: [10, 10, 10] }, previous: null });
    expect(res[4].ignored === false && res[4].fastest?.totalS).toBe(29);
    expect(res[5].ignored === false && res[5].previous?.totalS).toBe(29);
    expect(tr.fastest).toEqual({ totalS: 28, laps: [10, 9, 9] });
    expect(res[6]).toEqual({ ignored: false, lapS: 12 }); // 30: slower
    expect(tr.passes).toBe(7);
    expect(tr.lastLapS).toBe(12);
  });

  it("needs a strictly faster run for a record", () => {
    const tr = new LapTracker();
    const res = feed(tr, [0, 10, 20, 30, 40]);
    expect(res[4]).toEqual({ ignored: false, lapS: 10 });
  });

  it("ignores passes shorter than the minimum lap, timing from the counted pass", () => {
    const tr = new LapTracker();
    const res = feed(tr, [0, 3, 10, 20, 25, 30]);
    expect(res[1]).toEqual({ ignored: true, sinceS: 3 });
    expect(res[2]).toEqual({ ignored: false, lapS: 10 });
    expect(res[4]).toEqual({ ignored: true, sinceS: 5 });
    expect(tr.fastest).toEqual({ totalS: 30, laps: [10, 10, 10] });
    expect(tr.passes).toBe(4);
  });

  it("does not run laps across a break, but keeps the fastest", () => {
    const tr = new LapTracker();
    feed(tr, [0, 10, 20, 30]);
    tr.break();
    const res = feed(tr, [35, 44, 53]);
    expect(res[0]).toEqual({ ignored: false }); // no lap across the break, nor ignored
    expect(tr.lastLapS).toBe(9);
    expect(tr.fastest?.totalS).toBe(30); // only 2 laps since the break
    expect(feed(tr, [62])[0]).toMatchObject({ fastest: { totalS: 27 }, previous: { totalS: 30 } });
  });

  it("forgets everything on clear", () => {
    const tr = new LapTracker();
    feed(tr, [0, 10, 20, 30]);
    tr.clear();
    expect(tr.fastest).toBeNull();
    expect(tr.passes).toBe(0);
    expect(feed(tr, [32])[0]).toEqual({ ignored: false }); // no lap across the clear
  });

  it("recomputes from the passes since the clear when the rules change", () => {
    const tr = new LapTracker();
    feed(tr, [0, 5, 10, 20, 30]);
    tr.break();
    feed(tr, [40, 47, 54]);
    expect(tr.fastest).toEqual({ totalS: 30, laps: [10, 10, 10] });
    tr.recompute({ ...rules, count: 2 });
    expect(tr.fastest).toEqual({ totalS: 20, laps: [10, 10] });
    tr.recompute({ ...rules, count: 2, minLapS: 4 });
    expect(tr.fastest).toEqual({ totalS: 10, laps: [5, 5] });
    expect(tr.passes).toBe(8);
    tr.recompute(rules); // 47 is short again; 54 is a 14 s lap from 40
    expect(tr.fastest).toEqual({ totalS: 30, laps: [10, 10, 10] });
    expect(tr.passes).toBe(6);
  });

  it("counts every pass with the minimum lap off", () => {
    const tr = new LapTracker();
    const res = feed(tr, [0, 3, 10], { ...rules, minLapOn: false });
    expect(res[1]).toEqual({ ignored: false, lapS: 3 });
    expect(tr.passes).toBe(3);
  });

  it("keeps no fastest run with it off, and finds it when turned on", () => {
    const tr = new LapTracker();
    const off = { ...rules, fastestOn: false };
    const res = feed(tr, [0, 10, 20, 30], off);
    expect(res[3]).toEqual({ ignored: false, lapS: 10 });
    expect(tr.fastest).toBeNull();
    tr.recompute(rules);
    expect(tr.fastest).toEqual({ totalS: 30, laps: [10, 10, 10] });
  });
});
