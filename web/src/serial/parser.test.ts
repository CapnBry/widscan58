import { describe, expect, it } from "vitest";
import { StreamParser, crc32, encodeFrame } from "./parser";
import { FRAME_METRICS, FRAME_SPECTRUM, RawIqAssembler, parseLevels, parseMetrics, parseSpectrum, parseTelemetry, rawIqChunkIsLast } from "../proto/frames";

const text = (s: string) => new TextEncoder().encode(s);
const join = (...parts: Uint8Array[]) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
};

function metricsPayload(): Uint8Array {
  const H = 34; // sizeof(frame_metrics_t)
  const dv = new DataView(new ArrayBuffer(H + 26));
  dv.setUint32(0, 7, true);
  dv.setUint32(8, 2410437, true);
  dv.setUint32(20, 67602, true); // dsp_us beyond the old u16 range
  dv.setInt16(26, -6860, true);
  dv.setUint8(30, 60);
  dv.setUint8(32, 4);
  dv.setUint8(33, 1);
  dv.setUint8(H, 3);
  dv.setUint8(H + 1, 1);
  dv.setInt16(H + 2, -3600, true);
  dv.setInt16(H + 10, -32768, true);
  dv.setInt32(H + 14, -1234, true);
  dv.setInt32(H + 18, 885000, true);
  dv.setInt16(H + 22, -4000, true);
  dv.setInt16(H + 24, 4400, true);
  return new Uint8Array(dv.buffer);
}

// frame_levels_t + two frame_level_t, in SNR (2) or RSSI (1) mode.
function levelsPayload(mode: number): DataView {
  const H = 32;
  const dv = new DataView(new ArrayBuffer(H + 8));
  dv.setUint32(0, 9, true);
  dv.setUint32(8, 5677000, true);
  dv.setUint32(20, 1150, true);
  dv.setUint8(26, 60);
  dv.setUint8(28, 4);
  dv.setUint8(29, mode);
  dv.setUint8(30, mode === 2 ? 118 : 0); // floor/bin -68.5 dBFS
  dv.setUint8(31, 2);
  dv.setUint8(H, 0);
  dv.setUint8(H + 1, 1);
  dv.setUint8(H + 2, 111); // -36 dBFS
  dv.setUint8(H + 3, mode === 2 ? 82 : 0); // 20.5 dB
  dv.setUint8(H + 4, 1);
  dv.setUint8(H + 6, 25); // -57.5 dBFS
  return dv;
}

describe("crc32", () => {
  it("matches zlib for a known vector", () => {
    expect(crc32(text("123456789"))).toBe(0xcbf43926);
  });
});

describe("StreamParser", () => {
  it("splits interleaved text lines and frames", () => {
    const p = new StreamParser();
    const items = p.push(join(text("OK\n"), encodeFrame(FRAME_METRICS, metricsPayload()), text("STATUS {\"a\":1}\n")));
    expect(items.map((i) => i.kind)).toEqual(["text", "frame", "text"]);
    const f = items[1];
    if (f.kind !== "frame") throw new Error();
    const m = parseMetrics(f.payload);
    expect(m.loKhz).toBe(2410437);
    expect(m.floorBin).toBeCloseTo(-68.6);
    expect(m.dspUs).toBe(67602);
    expect(m.channels[0]).toMatchObject({ id: 3, flags: 1, rssi: -36, clearFloor: null, peakOffHz: -1234, centroidOffHz: 885000, obwLoHz: -4000000, obwHiHz: 4400000 });
  });

  it("parses levels frames, leaving what the mode does not measure null", () => {
    const snr = parseLevels(levelsPayload(2));
    expect(snr).toMatchObject({ seq: 9, loKhz: 5677000, dspUs: 1150, gain: 60, hops: 4, mode: "SNR", floorBin: -68.5, dc: null });
    expect(snr.channels).toHaveLength(2);
    expect(snr.channels[0]).toMatchObject({ id: 0, flags: 1, rssi: -36, snr: 20.5, floor: -56.5, rssiEma: null, peak: null, centroidOffHz: null });
    const rssi = parseLevels(levelsPayload(1));
    expect(rssi.mode).toBe("RSSI");
    expect(rssi.floorBin).toBeNull();
    expect(rssi.channels[1]).toMatchObject({ id: 1, rssi: -57.5, snr: null, floor: null });
  });

  it("reassembles frames split across chunks", () => {
    const p = new StreamParser();
    const frame = encodeFrame(FRAME_METRICS, metricsPayload());
    const all = [] as ReturnType<StreamParser["push"]>;
    for (let j = 0; j < frame.length; j += 5) all.push(...p.push(frame.subarray(j, j + 5)));
    expect(all).toHaveLength(1);
  });

  it("drops a corrupted frame and recovers on the next one", () => {
    const p = new StreamParser();
    const bad = encodeFrame(FRAME_METRICS, metricsPayload());
    bad[10] ^= 0xff;
    const items = p.push(join(bad, encodeFrame(FRAME_METRICS, metricsPayload()), text("OK\n")));
    expect(p.crcErrors).toBeGreaterThan(0);
    expect(items.filter((i) => i.kind === "frame")).toHaveLength(1);
    expect(items.at(-1)).toEqual({ kind: "text", line: "OK" });
  });

  it("ignores the tail of a frame joined part-way through", () => {
    const p = new StreamParser();
    const frame = encodeFrame(FRAME_METRICS, metricsPayload());
    const items = p.push(join(frame.subarray(9), text("\n"), encodeFrame(FRAME_METRICS, metricsPayload()), text("ERR busy\n")));
    expect(items.filter((i) => i.kind === "frame")).toHaveLength(1);
    expect(items.filter((i) => i.kind === "text")).toEqual([{ kind: "text", line: "ERR busy" }]);
  });

  it("decodes spectrum codes to dBFS", () => {
    const dv = new DataView(new ArrayBuffer(20 + 3));
    dv.setUint32(4, 2431312, true);
    dv.setInt32(8, -11300000, true);
    dv.setUint32(12, 88281, true);
    dv.setUint16(16, 3, true);
    new Uint8Array(dv.buffer).set([0, 115, 255], 20);
    const p = new StreamParser();
    const [f] = p.push(encodeFrame(FRAME_SPECTRUM, new Uint8Array(dv.buffer)));
    if (f.kind !== "frame") throw new Error();
    const s = parseSpectrum(f.payload);
    expect(Array.from(s.db)).toEqual([-127.5, -70, 0]);
  });
});

describe("telemetry", () => {
  it("decodes u32 timings", () => {
    const dv = new DataView(new ArrayBuffer(59));
    dv.setUint32(16, 69382, true);
    dv.setUint32(40, 66713, true);
    dv.setUint16(44, 250, true);
    dv.setUint32(46, 29872, true);
    dv.setUint8(58, 1);
    const t = parseTelemetry(dv);
    expect([t.hopUsAvg, t.dspUsMax, t.linkFill, t.heapFree, t.running]).toEqual([69382, 66713, 0.25, 29872, true]);
  });
});

describe("raw I/Q chunks", () => {
  const chunk = (n: number, offset: number, count: number) => {
    const dv = new DataView(new ArrayBuffer(14 + 2 * count));
    dv.setUint32(0, 5677000, true);
    dv.setUint32(4, 80000000, true);
    dv.setUint16(8, n, true);
    dv.setUint16(10, offset, true);
    dv.setUint8(12, 1);
    dv.setUint8(13, 2);
    for (let j = 0; j < count; j++) {
      dv.setInt8(14 + 2 * j, (offset + j) % 100);
      dv.setInt8(15 + 2 * j, -((offset + j) % 100));
    }
    return dv;
  };

  it("assembles a snapshot from consecutive chunks", () => {
    const a = new RawIqAssembler();
    expect(a.add(chunk(2500, 0, 1024))).toBeNull();
    expect(rawIqChunkIsLast(chunk(2500, 1024, 1024))).toBe(false);
    expect(a.add(chunk(2500, 1024, 1024))).toBeNull();
    expect(rawIqChunkIsLast(chunk(2500, 2048, 452))).toBe(true);
    const r = a.add(chunk(2500, 2048, 452));
    expect(r && [r.n, r.loKhz, r.shift, r.conjugated, r.i[2499], r.q[1501]]).toEqual([2500, 5677000, 2, true, 99, -1]);
  });

  it("drops a snapshot with a missing chunk and starts over at offset 0", () => {
    const a = new RawIqAssembler();
    a.add(chunk(3000, 0, 1024));
    expect(a.add(chunk(3000, 2048, 952))).toBeNull();
    expect(a.add(chunk(100, 0, 100))?.n).toBe(100);
  });
});
