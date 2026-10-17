// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { lapCallout } from "./speech";

describe("lapCallout", () => {
  it("reads the channel and the lap to 0.1 s", () => {
    expect(lapCallout("R4", 13.66)).toBe("R4 13.7");
    expect(lapCallout("A8", 9.04)).toBe("A8 9.0");
  });
});
