// The simulator's LapSeat against the same cases as lap.c's host tests
// (firmware/test/test_dsp). GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { LapSeat } from "./lap";

/* A pass at 5 ms per update: floor 160 with +/-1 noise for 400 ms, steps up
 * 170..210 (50 ms each), 150 ms at 220, steps back down, 400 ms of floor.
 * The plateau is updates 130..159 from t; dropout: updates [d, d + 10) read 150. */
const plateauMid = (t: number) => t + 5 * 130 + (5 * 29) / 2;
function feedPass(s: LapSeat, t: number, dropout = -1): number {
  let j = 0;
  for (let k = 0; k < 80; k++, j++, t += 5) s.feed(t, 160 + (k % 3 === 0 ? 1 : 0) - (k % 5 === 0 ? 1 : 0), 5000);
  const up = [170, 180, 190, 200, 210];
  const shape = [...up.flatMap((v) => Array(10).fill(v)), ...Array(30).fill(220), ...[...up].reverse().flatMap((v) => Array(10).fill(v))];
  for (const v of shape) {
    s.feed(t, dropout >= 0 && j >= dropout && j < dropout + 10 ? 150 : v, 5000);
    j++;
    t += 5;
  }
  for (let k = 0; k < 80; k++, t += 5) s.feed(t, 160, 5000);
  return t;
}

describe("simulator LapSeat (255 ms median)", () => {
  it("times a pass at the middle of its plateau", () => {
    const s = new LapSeat(190, 180);
    feedPass(s, 1000);
    expect(s.lap).toBe(1);
    expect(s.crossing).toBe(false);
    expect(s.passPeak).toBe(220);
    expect(Math.abs(s.passMs - plateauMid(1000))).toBeLessThanOrEqual(5);
    expect(s.nodeNadir).toBe(160); // the isolated 159s are filtered out
  });

  it("filters a 50 ms dropout and a 100 ms spike", () => {
    let s = new LapSeat(190, 180);
    feedPass(s, 0, 110);
    expect([s.lap, s.passPeak]).toEqual([1, 220]);
    s = new LapSeat(190, 180);
    for (let t = 0; t < 1000; t += 5) s.feed(t, t >= 500 && t < 600 ? 230 : 160, 5000);
    expect([s.lap, s.crossing, s.nodePeak]).toEqual([0, false, 160]);
  });

  it("reports nothing until the window fills, then a 255 ms window (at most 255 updates)", () => {
    for (const stepUs of [20000, 3500, 1000, 400]) {
      const s = new LapSeat(190, 180);
      const fillUs = Math.min(255000, 255 * stepUs);
      let tUs = 1000000;
      for (let k = 0; k < 3000; k++, tUs += stepUs) {
        s.feed(Math.floor(tUs / 1000), 100 + (k % 50), stepUs);
        if (tUs - 1000000 + 1000 < fillUs - 2 * stepUs) expect(s.valid).toBe(false);
      }
      expect(s.valid).toBe(true);
      // The output carries the middle update's time, about half a window back.
      const n = Math.min(255, Math.floor(255000 / stepUs));
      const expectMs = (tUs - stepUs) / 1000 - (Math.floor((n - 1) / 2) * stepUs) / 1000;
      expect(Math.abs(s.levelMs - expectMs)).toBeLessThanOrEqual(2 + stepUs / 1000);
    }
  });
});

describe("simulator LapSeat (median of 3 + EMA)", () => {
  const ema = (enter: number, exit: number) => {
    const s = new LapSeat(enter, exit);
    s.setFilter("EMA");
    return s;
  };

  it("reports from the first update and removes one-update spikes", () => {
    const s = ema(190, 180);
    s.feed(1000, 160, 0);
    expect([s.valid, s.level]).toEqual([true, 160]);
    for (let t = 1001; t < 1100; t++) s.feed(t, t === 1050 ? 230 : 160, 1000);
    expect(s.nodePeak).toBe(160);
  });

  it("weights each update by its interval (same time constant at any rate)", () => {
    for (const [dtUs, expected] of [[1000, 131], [3000, 167], [20000, 200]]) {
      const s = ema(250, 240);
      let t = 1000;
      for (let k = 0; k < 20; k++, t += dtUs / 1000) s.feed(t, 100, dtUs);
      s.feed(t, 200, dtUs);
      expect(s.level).toBe(100);
      s.feed(t + dtUs / 1000, 200, dtUs);
      expect(Math.abs(s.level - expected)).toBeLessThanOrEqual(1);
    }
  });

  it("times a pass near the middle of its plateau", () => {
    const s = ema(190, 180);
    feedPass(s, 1000);
    expect([s.lap, s.passPeak]).toEqual([1, 220]);
    expect(Math.abs(s.passMs - plateauMid(1000))).toBeLessThanOrEqual(10);
  });
});
