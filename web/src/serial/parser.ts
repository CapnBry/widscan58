// Splits the device byte stream into text lines and CRC-checked binary frames.
// Frame: 0xA5 'R' type version | u16 len | payload | u32 crc32 (zlib). GPL-3.0-or-later.

export type StreamItem =
  | { kind: "text"; line: string }
  | { kind: "frame"; type: number; payload: DataView };

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(data: Uint8Array, start = 0, end = data.length): number {
  let c = 0xffffffff;
  for (let j = start; j < end; j++) c = CRC_TABLE[(c ^ data[j]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const SYNC0 = 0xa5;
const SYNC1 = 0x52; // 'R'
const MAX_PAYLOAD = 65535;

export class StreamParser {
  private buf = new Uint8Array(1 << 16);
  private len = 0;
  crcErrors = 0;
  resyncBytes = 0;
  bytes = 0;
  private decoder = new TextDecoder();

  push(chunk: Uint8Array): StreamItem[] {
    this.bytes += chunk.length;
    if (this.len + chunk.length > this.buf.length) {
      const next = new Uint8Array(Math.max(this.buf.length * 2, this.len + chunk.length));
      next.set(this.buf.subarray(0, this.len));
      this.buf = next;
    }
    this.buf.set(chunk, this.len);
    this.len += chunk.length;
    const out: StreamItem[] = [];
    let pos = 0;
    while (pos < this.len) {
      const b = this.buf[pos];
      if (b === SYNC0) {
        if (this.len - pos < 6) break;
        if (this.buf[pos + 1] !== SYNC1) {
          pos++;
          this.resyncBytes++;
          continue;
        }
        const length = this.buf[pos + 4] | (this.buf[pos + 5] << 8);
        const total = 6 + length + 4;
        if (length > MAX_PAYLOAD) {
          pos++;
          continue;
        }
        if (this.len - pos < total) break;
        const want =
          (this.buf[pos + 6 + length] |
            (this.buf[pos + 7 + length] << 8) |
            (this.buf[pos + 8 + length] << 16) |
            (this.buf[pos + 9 + length] << 24)) >>>
          0;
        if (crc32(this.buf, pos, pos + 6 + length) !== want) {
          this.crcErrors++;
          pos++;
          this.resyncBytes++;
          continue;
        }
        const payload = this.buf.slice(pos + 6, pos + 6 + length);
        out.push({ kind: "frame", type: this.buf[pos + 2], payload: new DataView(payload.buffer) });
        pos += total;
        continue;
      }
      // Text: printable lines. Anything before a newline that contains a
      // sync byte is cut at the sync byte (a frame interrupted a partial line).
      let end = pos;
      while (end < this.len && this.buf[end] !== 0x0a && this.buf[end] !== SYNC0) end++;
      if (end >= this.len) {
        if (this.len - pos > 4096) {
          // Garbage without a newline (e.g. joined mid-frame): drop it.
          this.resyncBytes += this.len - pos;
          pos = this.len;
        }
        break;
      }
      const text = this.decoder.decode(this.buf.subarray(pos, end)).replace(/\r$/, "");
      if (this.buf[end] === 0x0a) end++;
      // Lines from the firmware start with an uppercase command word; anything
      // else is the tail of a frame we joined part-way through.
      if (/^[A-Z][A-Z0-9_?]*( |$)/.test(text)) out.push({ kind: "text", line: text });
      else if (text.length) this.resyncBytes += text.length;
      pos = end;
    }
    this.buf.copyWithin(0, pos, this.len);
    this.len -= pos;
    return out;
  }
}

// Encoder used by the mock device and tests.
export function encodeFrame(type: number, payload: Uint8Array): Uint8Array {
  const out = new Uint8Array(6 + payload.length + 4);
  out[0] = SYNC0;
  out[1] = SYNC1;
  out[2] = type;
  out[3] = 1;
  out[4] = payload.length & 0xff;
  out[5] = payload.length >> 8;
  out.set(payload, 6);
  const c = crc32(out, 0, 6 + payload.length);
  new DataView(out.buffer).setUint32(6 + payload.length, c, true);
  return out;
}
