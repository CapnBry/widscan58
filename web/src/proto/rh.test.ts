import { describe, expect, it } from "vitest";
import { RH, checkReply, checksum, encodeWrite, levelDb, parseExtremums, parsePassStats, u16 } from "./rh";

describe("RotorHazard codec", () => {
  it("encodes writes with the payload checksum", () => {
    expect([...encodeWrite(RH.WRITE_FREQUENCY, u16(5658))]).toEqual([0x51, 0x16, 0x1a, (0x16 + 0x1a) & 0xff]);
    expect([...encodeWrite(RH.WRITE_ENTER_AT_LEVEL, [200])]).toEqual([0x71, 200, 200]);
  });

  it("checks reply length and checksum", () => {
    const ok = Uint8Array.of(0x25, 36, (0x25 + 36) & 0xff);
    expect([...checkReply(ok, 2)!]).toEqual([0x25, 36]);
    expect(checkReply(Uint8Array.of(0x25, 36, 0), 2)).toBeNull();
    expect(checkReply(Uint8Array.of(0x25, 36), 2)).toBeNull();
    expect(checksum([255, 2])).toBe(1);
  });

  it("parses pass stats and extremums (big-endian)", () => {
    expect(parsePassStats(Uint8Array.of(3, 0x01, 0x2c, 170, 220, 215, 0x0d, 0xac))).toEqual({
      lapId: 3, msSinceLap: 300, level: 170, nodePeak: 220, passPeak: 215, loopUs: 3500,
    });
    expect(parseExtremums(Uint8Array.of(0x03, 158, 155, 219, 0, 40, 0, 10))).toEqual({
      crossing: true, passNadir: 158, nodeNadir: 155, extremum: { peak: true, level: 219, msAgo: 40, durationMs: 10 },
    });
    expect(parseExtremums(Uint8Array.of(0, 158, 155, 0, 0, 0, 0, 0)).extremum).toBeNull();
  });

  it("converts level codes", () => {
    expect(levelDb(159, "RSSI")).toBe(-48);
    expect(levelDb(41, "SNR")).toBe(20.5);
  });
});
