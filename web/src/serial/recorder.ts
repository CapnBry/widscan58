// Session recording and replay of the raw device byte stream. GPL-3.0-or-later.
// File format ".rx5808": repeated records of u32 ms | u32 length | bytes (LE).
import type { Transport } from "./device";

export class Recorder {
  private chunks: { t: number; data: Uint8Array }[] = [];
  private start = 0;
  bytes = 0;
  active = false;

  begin(preamble: string[]) {
    this.chunks = [];
    this.bytes = 0;
    this.start = performance.now();
    this.active = true;
    // Profile/status lines first, so a replay can build the UI without commands.
    for (const line of preamble) this.add(new TextEncoder().encode(line + "\n"));
  }

  add(data: Uint8Array) {
    if (!this.active) return;
    this.chunks.push({ t: performance.now() - this.start, data: data.slice() });
    this.bytes += data.length;
  }

  end(): Blob {
    this.active = false;
    const parts: BlobPart[] = [];
    for (const c of this.chunks) {
      const h = new DataView(new ArrayBuffer(8));
      h.setUint32(0, Math.round(c.t), true);
      h.setUint32(4, c.data.length, true);
      parts.push(h.buffer, c.data as Uint8Array<ArrayBuffer>);
    }
    return new Blob(parts, { type: "application/octet-stream" });
  }
}

export class ReplayTransport implements Transport {
  readonly name: string;
  private timers: number[] = [];
  constructor(private file: ArrayBuffer, fileName: string, private speed = 1) {
    this.name = `Replay: ${fileName}`;
  }

  async open(onBytes: (c: Uint8Array) => void, onClose: (why: string) => void): Promise<void> {
    const dv = new DataView(this.file);
    let pos = 0;
    let last = 0;
    while (pos + 8 <= dv.byteLength) {
      const t = dv.getUint32(pos, true);
      const n = dv.getUint32(pos + 4, true);
      if (pos + 8 + n > dv.byteLength) break;
      const data = new Uint8Array(this.file, pos + 8, n);
      this.timers.push(window.setTimeout(() => onBytes(data), t / this.speed));
      last = t;
      pos += 8 + n;
    }
    this.timers.push(window.setTimeout(() => onClose("replay finished"), last / this.speed + 100));
  }

  async write(): Promise<void> {
    throw new Error("replay is read-only");
  }

  async close(): Promise<void> {
    for (const t of this.timers) clearTimeout(t);
    this.timers = [];
  }
}
