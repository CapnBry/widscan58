// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { ATTENUATOR_TABLE, CALIBRATION, INPUT_MAX_DBM, calOffset, formatMw, lessNoise, meanDb } from "./power";
import { Series } from "./store";

describe("calOffset", () => {
  it("interpolates between calibration points and flags the frequencies outside them", () => {
    const pts = CALIBRATION["c5-race"];
    expect(calOffset("c5-race", pts[0][0])).toEqual({ db: pts[0][1], calibrated: true });
    const mid = calOffset("c5-race", (pts[0][0] + pts[1][0]) / 2)!;
    expect(mid.db).toBeCloseTo((pts[0][1] + pts[1][1]) / 2, 6);
    expect(mid.calibrated).toBe(true);
    const last = pts[pts.length - 1];
    expect(calOffset("c5-race", last[0] + 5)).toEqual({ db: last[1], calibrated: true });
    expect(calOffset("c5-race", last[0] + 20)).toEqual({ db: last[1], calibrated: false });
  });

  it("has no offset for a board without a calibration", () => {
    expect(calOffset("s3-race24", 5658)).toBeNull();
  });
});

describe("meanDb", () => {
  it("averages power, not dB, over the window, and falls back to the newest value", () => {
    const s = new Series(8);
    s.push(0, -40);
    s.push(9.5, -30);
    s.push(10, -20);
    expect(meanDb(s, 10, 0)).toBe(-20);
    expect(meanDb(s, 10, 1)).toBeCloseTo(10 * Math.log10((1e-3 + 1e-2) / 2), 6);
    expect(meanDb(s, 20, 1)).toBe(-20); // nothing in the last second
    expect(meanDb(new Series(4), 0, 1)).toBeNull();
  });
});

describe("lessNoise", () => {
  it("subtracts the noise power and refuses readings within 3 dB of it", () => {
    expect(lessNoise(-30, null)).toBe(-30);
    expect(lessNoise(-50, -50)).toBeNull();
    expect(lessNoise(-30, -40)!).toBeCloseTo(10 * Math.log10(1e-3 - 1e-4), 6);
    expect(lessNoise(-38, -40)).toBeNull();
  });
});

describe("formatMw", () => {
  it("picks the unit and precision", () => {
    expect(formatMw(14)).toBe("25.1 mW");
    expect(formatMw(26)).toBe("398 mW");
    expect(formatMw(30)).toBe("1.00 W");
    expect(formatMw(0)).toBe("1.00 mW");
    expect(formatMw(-10)).toBe("0.100 mW");
  });
});

describe("ATTENUATOR_TABLE", () => {
  it("keeps the board's input in range with 2 dB to spare", () => {
    for (const r of ATTENUATOR_TABLE) expect(r.dbm + 2 - r.attenDb).toBeLessThanOrEqual(INPUT_MAX_DBM);
  });
});
