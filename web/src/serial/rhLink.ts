// Request/response transactions in RotorHazard mode, the way RotorHazard's
// serial_node.py runs them: one at a time, the input flushed before each
// request, a short timeout and a few retries. Our own command lines (those
// the firmware allows in this mode) go through the same queue and are
// answered by one text line. GPL-3.0-or-later.
import { checkReply, encodeWrite } from "../proto/rh";

const READ_TIMEOUT_MS = 250; // serial_node.py's
const READ_RETRIES = 2;

type Want = { kind: "bytes"; n: number } | { kind: "line" };

export interface RhLinkStats {
  reads: number;
  writes: number;
  errors: number;
}

export class RhLink {
  private buf: number[] = [];
  private want: Want | null = null;
  private done: ((v: Uint8Array | string | null) => void) | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  closed = false;
  stats: RhLinkStats = { reads: 0, writes: 0, errors: 0 };
  /** Time of the last read's request and response (performance.now()). */
  lastRequest = 0;
  lastResponse = 0;

  constructor(private send: (d: Uint8Array) => Promise<void>) {}

  push(chunk: Uint8Array) {
    for (const b of chunk) this.buf.push(b);
    this.check();
  }

  close() {
    this.closed = true;
    this.finish(null);
  }

  private check() {
    if (!this.want) return;
    if (this.want.kind === "bytes" && this.buf.length >= this.want.n) {
      this.finish(Uint8Array.from(this.buf.splice(0, this.want.n)));
    } else if (this.want.kind === "line") {
      const nl = this.buf.indexOf(10);
      if (nl >= 0) this.finish(new TextDecoder().decode(Uint8Array.from(this.buf.splice(0, nl + 1))).trimEnd());
    }
  }

  private finish(v: Uint8Array | string | null) {
    const d = this.done;
    this.want = null;
    this.done = null;
    d?.(v);
  }

  /** Send data and wait for the wanted reply (null on timeout). */
  private exchange(data: Uint8Array, want: Want | null, timeoutMs: number): Promise<Uint8Array | string | null> {
    if (this.closed) return Promise.reject(new Error("RotorHazard link closed"));
    this.buf.length = 0; // serial_node.py: flushInput before every request
    return new Promise((resolve, reject) => {
      if (want) {
        const timer = window.setTimeout(() => this.finish(null), timeoutMs);
        this.want = want;
        this.done = (v) => {
          clearTimeout(timer);
          resolve(v);
        };
      }
      this.send(data).then(
        () => {
          if (!want) resolve(null);
        },
        (e) => {
          this.want = null;
          this.done = null;
          reject(e);
        },
      );
    });
  }

  private serial<T>(fn: () => Promise<T>): Promise<T> {
    const p = this.queue.then(fn, fn);
    this.queue = p.catch(() => undefined);
    return p;
  }

  /** A read command's payload (checksum verified); throws after the retries. */
  read(cmd: number, size: number): Promise<Uint8Array> {
    return this.serial(async () => {
      for (let attempt = 0; attempt <= READ_RETRIES; attempt++) {
        const t0 = performance.now();
        const reply = (await this.exchange(Uint8Array.of(cmd), { kind: "bytes", n: size + 1 }, READ_TIMEOUT_MS)) as Uint8Array | null;
        this.stats.reads++;
        const payload = reply && checkReply(reply, size);
        if (payload) {
          this.lastRequest = t0;
          this.lastResponse = performance.now();
          return payload;
        }
        this.stats.errors++;
        if (this.closed) break;
      }
      throw new Error(`RotorHazard read 0x${cmd.toString(16)}: no valid reply`);
    });
  }

  /** A write command (no reply in the protocol). */
  write(cmd: number, payload: number[]): Promise<void> {
    return this.serial(async () => {
      await this.exchange(encodeWrite(cmd, payload), null, 0);
      this.stats.writes++;
    });
  }

  /** One of our command lines; resolves with its reply line. */
  text(line: string, timeoutMs = 2000): Promise<string> {
    return this.serial(async () => {
      const reply = await this.exchange(new TextEncoder().encode(line + "\n"), { kind: "line" }, timeoutMs);
      if (reply === null) throw new Error(`timeout: ${line}`);
      return reply as string;
    });
  }
}
