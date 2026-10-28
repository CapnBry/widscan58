// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { gpioUnsaved, parseGpio } from "./gpio";

describe("parseGpio", () => {
  it("reads each pin's setting in effect and saved", () => {
    const pins = parseGpio('GPIO {"count":6,"active":"-LHxTR","saved":"-H-xT-"}');
    expect(pins).toEqual([
      { n: 0, active: "NONE", saved: "NONE" },
      { n: 1, active: "LOW", saved: "HIGH" },
      { n: 2, active: "HIGH", saved: "NONE" },
      { n: 3, active: "RESERVED", saved: "RESERVED" },
      { n: 4, active: "TX", saved: "TX" },
      { n: 5, active: "RX", saved: "NONE" },
    ]);
    expect(gpioUnsaved(pins).map((p) => p.n)).toEqual([1, 2, 5]);
  });

  it("rejects a malformed reply", () => {
    expect(() => parseGpio('GPIO {"count":2,"active":"-","saved":"--"}')).toThrow();
    expect(() => parseGpio('GPIO {"count":1,"active":"?","saved":"-"}')).toThrow();
    expect(() => parseGpio("ERR command")).toThrow();
  });
});
