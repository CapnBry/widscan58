import { describe, expect, it } from "vitest";
import { StreamParser, crc32, encodeFrame } from "./parser";
import { FRAME_METRICS, FRAME_SPECTRUM, parseMetrics, parseSpectrum, parseTelemetry } from "../proto/frames";

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
  dv.setUint32(H + 22, 59000, true);
  return new Uint8Array(dv.buffer);
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
    expect(m.channels[0]).toMatchObject({ id: 3, flags: 1, rssi: -36, clearFloor: null, peakOffHz: -1234, centroidOffHz: 885000, obwHz: 59000 });
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
    const dv = new DataView(new ArrayBuffer(62));
    dv.setUint32(16, 69382, true);
    dv.setUint32(40, 66713, true);
    dv.setUint16(44, 250, true);
    dv.setUint32(46, 29872, true);
    dv.setInt8(61, -1);
    const t = parseTelemetry(dv);
    expect([t.hopUsAvg, t.dspUsMax, t.linkFill, t.heapFree, t.filter]).toEqual([69382, 66713, 0.25, 29872, -1]);
  });
});
