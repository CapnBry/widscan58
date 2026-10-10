// Transports (Web Serial, mock, replay) and the command/reply client. GPL-3.0-or-later.
import { StreamParser, type StreamItem } from "./parser";
import { FRAME_RAWIQ } from "../proto/frames";
import { grantedUsbCdcPorts, requestUsbCdcPort } from "./webusb";

export interface Transport {
  readonly name: string;
  open(onBytes: (chunk: Uint8Array) => void, onClose: (why: string) => void): Promise<void>;
  write(data: Uint8Array): Promise<void>;
  close(): Promise<void>;
}

const ESPRESSIF_VID = 0x303a;

/** The parts of a Web Serial port used here (setSignals by the flasher);
 *  UsbCdcPort implements the same subset. */
export type Port = Pick<SerialPort, "readable" | "writable" | "open" | "close" | "getInfo" | "setSignals">;

/** Chrome on Android only offers wired serial ports on devices with Android's
 *  newer serial API (elsewhere just Bluetooth RFCOMM), so there the board's
 *  USB Serial/JTAG is driven as a CDC-ACM device over WebUSB instead. */
function viaWebUsb(): boolean {
  return /Android/i.test(navigator.userAgent) && "usb" in navigator;
}

export function requestEspressifPort(): Promise<Port> {
  if (viaWebUsb()) return requestUsbCdcPort(ESPRESSIF_VID);
  return navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
}

export class WebSerialTransport implements Transport {
  readonly name = viaWebUsb() ? "WebUSB serial" : "Web Serial";
  private port: Port | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private readDone: Promise<void> = Promise.resolve();
  private closing = false;

  /** port: a previously granted port (navigator.serial.getPorts()); otherwise
   *  open() shows the browser's port chooser. */
  constructor(private preset: Port | null = null) {}

  /** The open port, for handing over to the flasher after close(). */
  get currentPort(): Port | null {
    return this.port;
  }

  static supported(): boolean {
    return viaWebUsb() || "serial" in navigator;
  }

  /** Fires on page load and whenever a board (re)enumerates. */
  static onConnectEvent(cb: () => void): void {
    (viaWebUsb() ? navigator.usb : navigator.serial).addEventListener("connect", cb);
  }

  async open(onBytes: (chunk: Uint8Array) => void, onClose: (why: string) => void): Promise<void> {
    this.port = this.preset ?? (await requestEspressifPort());
    // USB Serial/JTAG ignores the baud rate; a large buffer absorbs bursts.
    await this.port.open({ baudRate: 115200, bufferSize: 1 << 20 });
    this.closing = false;
    this.readDone = this.readLoop(onBytes, onClose);
  }

  /** Non-fatal line conditions: Web Serial errors the current readable stream,
   *  but port.readable is replaced and reading can continue. */
  /* Chrome (up to at least 153) inherits VMIN=0 from a previous pyserial or
   * esptool user of the tty and then misreads "no data yet" as a lost device. */
  private static readonly LOST_HINT =
    "If the board is still plugged in: another serial tool probably left the tty at VMIN=0. " +
    "Run `stty -F /dev/ttyACM0 min 1` (adjust the port), or press RESET on the board, then connect again.";
  private static readonly RECOVERABLE = new Set(["BreakError", "FramingError", "ParityError", "BufferOverrunError"]);
  onWarning: (msg: string) => void = () => {};

  private async readLoop(onBytes: (chunk: Uint8Array) => void, onClose: (why: string) => void) {
    const port = this.port!;
    while (port.readable && !this.closing) {
      this.reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value) onBytes(value);
        }
      } catch (e) {
        // UsbCdcPort may surface non-DOMException errors.
        const err = e instanceof Error ? e : new Error(String(e));
        if (!WebSerialTransport.RECOVERABLE.has(err.name)) {
          this.reader.releaseLock();
          // Release the port, or the next connect attempt finds it still open.
          await port.close().catch(() => {});
          this.port = null;
          onClose(`read error: ${err.name}: ${err.message}`);
          if (err.name === "NetworkError") this.onWarning(WebSerialTransport.LOST_HINT);
          return;
        }
        this.onWarning(`${err.name} (${err.message}); continuing`);
      } finally {
        try {
          this.reader.releaseLock();
        } catch {
          /* already released */
        }
      }
    }
    onClose(this.closing ? "closed" : "port ended");
  }

  async write(data: Uint8Array): Promise<void> {
    if (!this.port?.writable) throw new Error("port not open");
    const w = this.port.writable.getWriter();
    try {
      await w.write(data);
    } finally {
      w.releaseLock();
    }
  }

  async close(): Promise<void> {
    this.closing = true;
    try {
      await this.reader?.cancel();
    } catch {
      /* already closed */
    }
    // port.close() rejects while the read loop still holds the reader lock,
    // which would leave the port open (and the flasher unable to open it).
    await this.readDone;
    try {
      await this.port?.close();
    } catch {
      /* already closed */
    }
    this.port = null;
  }
}

/** Talks to tools/serial_bridge.py, which relays the raw serial byte stream. */
export class WebSocketTransport implements Transport {
  readonly name: string;
  private ws: WebSocket | null = null;
  constructor(private url: string) {
    this.name = `Bridge ${url}`;
  }

  open(onBytes: (chunk: Uint8Array) => void, onClose: (why: string) => void): Promise<void> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.url);
      ws.binaryType = "arraybuffer";
      let opened = false;
      ws.onopen = () => {
        opened = true;
        this.ws = ws;
        resolve();
      };
      ws.onmessage = (e) => onBytes(new Uint8Array(e.data as ArrayBuffer));
      ws.onerror = () => {
        if (!opened) reject(new Error(`cannot reach bridge at ${this.url}`));
      };
      ws.onclose = (e) => {
        if (opened) onClose(e.reason || "bridge closed");
      };
    });
  }

  async write(data: Uint8Array): Promise<void> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) throw new Error("bridge not open");
    this.ws.send(data.slice());
  }

  async close(): Promise<void> {
    this.ws?.close();
    this.ws = null;
  }
}

type Waiter = {
  match: (item: StreamItem) => boolean;
  resolve: (item: StreamItem) => void;
  reject: (e: Error) => void;
  timer: number;
};

export interface DeviceStats {
  bytes: number;
  crcErrors: number;
  resyncBytes: number;
  frames: Record<number, number>;
}

// Which reply ends a command. Most commands answer OK/ERR; queries answer with
// their own keyword; RAWIQ answers with a frame.
function replyMatcher(cmd: string): (item: StreamItem) => boolean {
  const word = cmd.split(" ")[0];
  const isErr = (i: StreamItem) => i.kind === "text" && i.line.startsWith("ERR");
  if (word === "RAWIQ") return (i) => (i.kind === "frame" && i.type === FRAME_RAWIQ) || isErr(i);
  const query: Record<string, string> = {
    INFO: "INFO ",
    "PROFILE?": "PROFILE ",
    "STATUS?": "STATUS ",
    "GAIN?": "GAIN ",
    "TIMING?": "TIMING ",
    "SPURS?": "SPURS ",
    "ENABLE?": "ENABLE ",
    SYNC: "SYNC ",
  };
  const prefix = query[word];
  if (prefix) return (i) => (i.kind === "text" && i.line.startsWith(prefix)) || isErr(i);
  return (i) => i.kind === "text" && (i.line === "OK" || i.line.startsWith("OK ") || i.line.startsWith("ERR"));
}

export class Device {
  private parser = new StreamParser();
  private waiters: Waiter[] = [];
  private queue: Promise<unknown> = Promise.resolve();
  private listeners = new Set<(item: StreamItem) => void>();
  private rawListeners = new Set<(chunk: Uint8Array) => void>();
  frames: Record<number, number> = {};
  connected = false;
  onClose: (why: string) => void = () => {};

  constructor(readonly transport: Transport) {}

  async open(): Promise<void> {
    await this.transport.open(
      (chunk) => this.onBytes(chunk),
      (why) => {
        this.connected = false;
        for (const w of this.waiters.splice(0)) {
          clearTimeout(w.timer);
          w.reject(new Error(why));
        }
        this.onClose(why);
      },
    );
    this.connected = true;
  }

  close(): Promise<void> {
    return this.transport.close();
  }

  onItem(fn: (item: StreamItem) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  onRaw(fn: (chunk: Uint8Array) => void): () => void {
    this.rawListeners.add(fn);
    return () => this.rawListeners.delete(fn);
  }

  stats(): DeviceStats {
    return {
      bytes: this.parser.bytes,
      crcErrors: this.parser.crcErrors,
      resyncBytes: this.parser.resyncBytes,
      frames: { ...this.frames },
    };
  }

  private onBytes(chunk: Uint8Array) {
    for (const fn of this.rawListeners) fn(chunk);
    for (const item of this.parser.push(chunk)) {
      if (item.kind === "frame") this.frames[item.type] = (this.frames[item.type] ?? 0) + 1;
      const w = this.waiters.findIndex((x) => x.match(item));
      if (w >= 0) {
        const [waiter] = this.waiters.splice(w, 1);
        clearTimeout(waiter.timer);
        waiter.resolve(item);
      }
      // A throwing listener must not reach the transport: Web Serial's read
      // loop would treat it as a fatal port error and stop reading.
      for (const fn of this.listeners) {
        try {
          fn(item);
        } catch (e) {
          console.error(e);
        }
      }
    }
  }

  /** Send one command line; resolves with the reply item. Commands are serialised. */
  command(line: string, timeoutMs = 3000): Promise<StreamItem> {
    const run = async () => {
      const match = replyMatcher(line);
      const reply = new Promise<StreamItem>((resolve, reject) => {
        const timer = window.setTimeout(() => {
          this.waiters = this.waiters.filter((x) => x.timer !== timer);
          reject(new Error(`timeout: ${line}`));
        }, timeoutMs);
        this.waiters.push({ match, resolve, reject, timer });
      });
      await this.transport.write(new TextEncoder().encode(line + "\n"));
      const item = await reply;
      if (item.kind === "text" && item.line.startsWith("ERR")) throw new Error(`${line}: ${item.line}`);
      return item;
    };
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => undefined);
    return p;
  }

  async text(line: string, timeoutMs?: number): Promise<string> {
    const item = await this.command(line, timeoutMs);
    return item.kind === "text" ? item.line : "";
  }

  async json<T>(line: string): Promise<T> {
    const reply = await this.text(line);
    return JSON.parse(reply.slice(reply.indexOf(" ") + 1)) as T;
  }
}

/** A port this origin was already granted, if any (no chooser needed). */
export async function grantedEspressifPort(): Promise<Port | null> {
  if (!WebSerialTransport.supported()) return null;
  const ports: Port[] = viaWebUsb() ? await grantedUsbCdcPorts() : await navigator.serial.getPorts();
  return ports.find((p) => p.getInfo().usbVendorId === ESPRESSIF_VID) ?? null;
}
