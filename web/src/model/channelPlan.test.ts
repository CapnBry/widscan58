// Channel plan helpers and the simulator's planner port (same cases as the
// firmware's test_plan_*). GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { BANDS, channelMhz, channelsCommand, normalizePlan } from "./channelPlan";
import { planBuild, type ProfileDef } from "../mock/dsp";

const c5 = { hopMaxSpanKhz: 40000, singleLoOffsetKhz: -8000, loStepKhz: 1000 } as ProfileDef;
const chans = (mhz: number[]) => mhz.map((m) => ({ khz: m * 1000 }));

describe("channel plan", () => {
  it("looks up chart frequencies", () => {
    expect(channelMhz("R1")).toBe(5658);
    expect(channelMhz("E8")).toBe(5945);
    expect(channelMhz("A9")).toBeUndefined();
    expect(channelMhz("X1")).toBeUndefined();
  });

  it("sorts by frequency, drops unknowns and duplicates, caps at 8", () => {
    expect(normalizePlan(["R2", "A8", "Q1", "R2", "F8", "R7"])).toEqual(["R2", "A8", "R7", "F8"]);
    expect(normalizePlan(["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "R1"])).toHaveLength(8);
    expect(channelsCommand(["R1", "A8"])).toBe("CHANNELS R1=5658 A8=5725");
  });
});

describe("planBuild", () => {
  it("pairs Raceband like before", () => {
    const hops = planBuild(c5, chans([5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]), 0xff, true);
    expect(hops.map((h) => h.loKhz)).toEqual([5677000, 5751000, 5825000, 5899000]);
    expect(hops[0].offsetHz).toEqual([-19e6, 18e6]);
    expect(planBuild(c5, chans([5658]), 1, false)[0].loKhz).toBe(5650000);
  });

  it("groups channels within 40 MHz", () => {
    expect(planBuild(c5, chans([5732, 5725, 5733]), 7, true)).toEqual([
      { loKhz: 5717000, ch: [1, 0, 2], offsetHz: [8e6, 15e6, 16e6] },
    ]);
    expect(planBuild(c5, chans([5740, 5760, 5780]), 7, true).map((h) => h.loKhz)).toEqual([5759000]);
    expect(planBuild(c5, chans([5733, 5732, 5752, 5760]), 15, true).map((h) => h.loKhz)).toEqual([5743000]);
    expect(planBuild(c5, chans([5733, 5774]), 3, true)).toHaveLength(2);
    const hops = (b: string) => planBuild(c5, chans(BANDS.find((x) => x.letter === b)!.mhz), 0xff, true).length;
    expect(["R", "A", "B", "E", "F", "L"].map(hops)).toEqual([4, 3, 3, 4, 3, 4]);
    expect(planBuild(c5, chans([5880, 5880]), 3, true).map((h) => h.loKhz)).toEqual([5872000]);
  });
});
