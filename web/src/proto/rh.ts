// RotorHazard serial node protocol, as firmware/src/proto/rh.c implements it
// (node API level 36). One command byte; reads (<= 0x50) are answered with
// their payload and a checksum (sum mod 256), writes (> 0x50) carry a payload
// and a checksum. Multi-byte values are big-endian. GPL-3.0-or-later.

export const RH = {
  READ_FREQUENCY: 0x03,
  TEST_RX_REGISTER: 0x04,
  READ_LAP_PASS_STATS: 0x0d,
  READ_LAP_EXTREMUMS: 0x0e,
  READ_REVISION_CODE: 0x22,
  READ_NODE_RSSI_PEAK: 0x23,
  READ_NODE_RSSI_NADIR: 0x24,
  READ_ENTER_AT_LEVEL: 0x31,
  READ_EXIT_AT_LEVEL: 0x32,
  READ_TIME_MILLIS: 0x33,
  READ_MULTINODE_COUNT: 0x39,
  READ_CURNODE_INDEX: 0x3a,
  READ_FW_VERSION: 0x3d,
  WRITE_FREQUENCY: 0x51,
  WRITE_ENTER_AT_LEVEL: 0x71,
  WRITE_EXIT_AT_LEVEL: 0x72,
  FORCE_END_CROSSING: 0x78,
  WRITE_CURNODE_INDEX: 0x7a,
} as const;

export const RH_API_MARKER = 0x25; // high byte of READ_REVISION_CODE
export const RH_FREQ_OFF = 1111; // what RotorHazard writes to switch a seat off
const FLAG_CROSSING = 0x01;
const FLAG_PEAK = 0x02;

export const checksum = (b: ArrayLike<number>, n = b.length) => {
  let s = 0;
  for (let j = 0; j < n; j++) s += b[j];
  return s & 0xff;
};

/** Bytes of a write command: command, payload, checksum of the payload. */
export function encodeWrite(cmd: number, payload: number[]): Uint8Array {
  return Uint8Array.from([cmd, ...payload, checksum(payload)]);
}
export const u16 = (v: number) => [(v >> 8) & 0xff, v & 0xff];

/** A read reply's payload, or null if the length or checksum is wrong. */
export function checkReply(reply: Uint8Array, size: number): Uint8Array | null {
  if (reply.length !== size + 1 || checksum(reply, size) !== reply[size]) return null;
  return reply.subarray(0, size);
}

export interface PassStats {
  lapId: number;
  msSinceLap: number;
  level: number;
  nodePeak: number;
  passPeak: number;
  loopUs: number;
}
export function parsePassStats(p: Uint8Array): PassStats {
  return {
    lapId: p[0],
    msSinceLap: (p[1] << 8) | p[2],
    level: p[3],
    nodePeak: p[4],
    passPeak: p[5],
    loopUs: (p[6] << 8) | p[7],
  };
}

export interface Extremums {
  crossing: boolean;
  passNadir: number;
  nodeNadir: number;
  /** The oldest queued peak or nadir, if any: level, ms since its start, duration ms. */
  extremum: { peak: boolean; level: number; msAgo: number; durationMs: number } | null;
}
export function parseExtremums(p: Uint8Array): Extremums {
  return {
    crossing: (p[0] & FLAG_CROSSING) !== 0,
    passNadir: p[1],
    nodeNadir: p[2],
    extremum: p[3] ? { peak: (p[0] & FLAG_PEAK) !== 0, level: p[3], msAgo: (p[4] << 8) | p[5], durationMs: (p[6] << 8) | p[7] } : null,
  };
}

/** Levels the server treats as valid (0 and 255 are "none"). */
export const validLevel = (v: number) => v > 0 && v < 255;

export type RhMetric = "RSSI" | "SNR";
/** A level code in display units: dBFS for RSSI, dB for SNR (metrics.h codes). */
export function levelDb(code: number, metric: RhMetric): number {
  return metric === "RSSI" ? code / 4 - 63.75 : code / 4;
}
