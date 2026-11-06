// GPL-3.0-or-later.
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { applyPreset, type FlashEvents } from "./flasher";
import type { Port } from "./device";

/** A board on a port: answers command lines through `reply`. */
function fakePort(reply: (line: string) => string | null, openFails = false): Port & { lines: string[] } {
  const lines: string[] = [];
  let ctrl: ReadableStreamDefaultController<Uint8Array>;
  const port = {
    lines,
    readable: null as ReadableStream<Uint8Array> | null,
    writable: null as WritableStream<Uint8Array> | null,
    async open() {
      if (openFails) throw new Error("gone");
      port.readable = new ReadableStream({ start: (c) => void (ctrl = c) });
      port.writable = new WritableStream({
        write(data) {
          const line = new TextDecoder().decode(data).trim();
          lines.push(line);
          const r = reply(line);
          if (r !== null) ctrl.enqueue(new TextEncoder().encode(r + "\n"));
        },
      });
    },
    async close() {
      port.readable = port.writable = null;
    },
    getInfo: () => ({}),
    setSignals: async () => {},
  };
  return port as unknown as Port & { lines: string[] };
}

const INFO = "INFO widscan58|esp32c5|1|abc|2026-11-06T00:00:00Z|phy";
const events = (log: string[]): FlashEvents => ({ stage: (t) => log.push(t), progress: () => {}, log: () => {} });

describe("applyPreset", () => {
  beforeEach(() => void vi.stubGlobal("window", globalThis));
  afterEach(() => void vi.unstubAllGlobals());

  it("waits for INFO, then sends the commands in order and closes the port", async () => {
    let boots = 2; // no answer to the first attempts: still booting
    const port = fakePort((l) => (l === "INFO" ? (boots-- > 0 ? null : INFO) : "OK"));
    await applyPreset(async () => [port], ["CONFIG RESET", "GPIO SAVE"], events([]));
    expect(port.lines.filter((l) => l === "INFO").length).toBe(3);
    expect(port.lines.slice(-2)).toEqual(["CONFIG RESET", "GPIO SAVE"]);
    expect(port.readable).toBeNull();
  }, 10000);

  it("moves to a port that opens when the old one is gone", async () => {
    const dead = fakePort(() => null, true);
    const fresh = fakePort((l) => (l === "INFO" ? INFO : "OK"));
    await applyPreset(async () => [dead, fresh], ["GPIO SAVE"], events([]));
    expect(fresh.lines).toContain("GPIO SAVE");
  });

  it("stops at a command the board refuses", async () => {
    const port = fakePort((l) => (l === "INFO" ? INFO : l === "BAD" ? "ERR command" : "OK"));
    await expect(applyPreset(async () => [port], ["BAD", "GPIO SAVE"], events([]))).rejects.toThrow(/BAD/);
    expect(port.lines).not.toContain("GPIO SAVE");
    expect(port.readable).toBeNull();
  });

  it("rejects a board that answers as something else", async () => {
    vi.spyOn(Date, "now").mockReturnValueOnce(0).mockReturnValue(1e9); // the deadline passes at once
    const port = fakePort(() => "INFO other|thing");
    await expect(applyPreset(async () => [port], ["GPIO SAVE"], events([]))).rejects.toThrow(/did not answer/);
    expect(port.lines).not.toContain("GPIO SAVE");
    vi.restoreAllMocks();
  });
});
