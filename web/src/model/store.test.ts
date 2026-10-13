// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { parseInfo } from "./store";

describe("parseInfo", () => {
  it("reads the version and commit time", () => {
    expect(parseInfo("widscan58|esp32c5|1|b7a62d2-dirty|2026-10-13T15:48:03-04:00|109,edb400d6,Mar 10 2026,10:22:11")).toEqual({
      target: "esp32c5",
      version: "b7a62d2-dirty",
      date: "2026-10-13T15:48:03-04:00",
    });
  });
  it("reads the version of older, space-separated firmware", () => {
    expect(parseInfo("widscan58 esp32c5 1 c579fcf 109,edb400d6,Mar 10 2026,10:22:11")).toEqual({
      target: "esp32c5",
      version: "c579fcf",
      date: null,
    });
  });
  it("rejects other replies", () => {
    expect(parseInfo("")).toBeNull();
  });
});
