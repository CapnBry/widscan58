// Writes the firmware published with this page (tools/pages_firmware.py puts
// it in ./firmware/) to a board over Web Serial or WebUSB, using esptool-js.
// GPL-3.0-or-later.
import SparkMD5 from "spark-md5";
import type { ESPLoader } from "esptool-js";
import type { Port } from "./device";

export interface ManifestPart {
  path: string;
  offset: number;
  size: number;
  sha256: string;
}

/** ESP Web Tools' manifest format, plus a size and sha256 per part and the
 *  commit time (ISO 8601, as INFO reports it). */
export interface Manifest {
  name: string;
  version: string;
  date?: string | null;
  builds: { chipFamily: string; parts: ManifestPart[] }[];
}

const FIRMWARE_DIR = "./firmware/";

/** Watchdog registers, from esptool's targets/<chip>.py. esptool-js 0.7 neither
 *  disables these on connect nor implements a watchdog reset for the C5. */
interface Watchdogs {
  wdtConfig0: number;
  wdtConfig1: number;
  wdtProtect: number;
  wdtKey: number;
  swdConfig: number;
  swdProtect: number;
  swdKey: number;
  swdAutoFeed: number;
}

/** Chips this page flashes, keyed by esptool-js's CHIP_NAME. */
const CHIPS: Record<string, Watchdogs> = {
  // LP_WDT, laid out as on the C6.
  "ESP32-C5": {
    wdtConfig0: 0x600b1c00,
    wdtConfig1: 0x600b1c04,
    wdtProtect: 0x600b1c18,
    wdtKey: 0x50d83aa1,
    swdConfig: 0x600b1c1c,
    swdProtect: 0x600b1c20,
    swdKey: 0x50d83aa1,
    swdAutoFeed: 1 << 18,
  },
  // Not offered yet: flashing the S3 over USB still fails part-way at times
  // (PLAN.md, "Open: flashing reliability"). Also enable esp32s3 in
  // .github/workflows/pages.yml and the flash dialog's text.
  // "ESP32-S3": {
  //   wdtConfig0: 0x60008098,
  //   wdtConfig1: 0x6000809c,
  //   wdtProtect: 0x600080b0,
  //   wdtKey: 0x50d83aa1,
  //   swdConfig: 0x600080b4,
  //   swdProtect: 0x600080b8,
  //   swdKey: 0x8f1d312a,
  //   swdAutoFeed: 0x80000000,
  // },
};

export const SUPPORTED_CHIPS = Object.keys(CHIPS);

/** A download failed for want of a network (fetch() and import() reject with
 *  a TypeError then; an HTTP error is a Response). */
export class OfflineError extends Error {
  constructor() {
    super("the firmware could not be downloaded: no internet connection");
  }
}
const offline = (): never => {
  throw new OfflineError();
};

export async function loadManifest(): Promise<Manifest> {
  const r = await fetch(FIRMWARE_DIR + "manifest.json", { cache: "no-cache" }).catch(offline);
  // The dev server answers a missing file with index.html, hence the JSON check.
  const m = r.ok ? ((await r.json().catch(() => null)) as Manifest | null) : null;
  if (!m?.builds) throw new Error("no firmware is published with this page");
  return m;
}

async function fetchPart(p: ManifestPart): Promise<Uint8Array> {
  const r = await fetch(FIRMWARE_DIR + p.path, { cache: "no-cache" }).catch(offline);
  if (!r.ok) throw new Error(`${p.path}: HTTP ${r.status}`);
  const data = new Uint8Array(await r.arrayBuffer());
  const hash = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  const hex = Array.from(hash, (b) => b.toString(16).padStart(2, "0")).join("");
  if (data.length !== p.size || hex !== p.sha256) throw new Error(`${p.path}: download corrupted (sha256 mismatch)`);
  return data;
}

export interface FlashEvents {
  stage(text: string): void;
  progress(fraction: number): void;
  log(line: string): void;
}

/** Flash `port` (closed; it is opened here and closed again). The board may
 *  already be in download mode (BOOT + RESET); otherwise it is reset into it
 *  over USB Serial/JTAG. Ends with a watchdog reset into the new firmware. */
export async function flashFirmware(port: Port, manifest: Manifest, ev: FlashEvents): Promise<void> {
  const { ESPLoader, Transport } = await import("esptool-js").catch(offline);
  // esptool-js writes "Connecting..." and its dots without a newline.
  let pending = "";
  const terminal = {
    clean() {},
    write(s: string) {
      pending += s;
    },
    writeLine(s: string) {
      ev.log(pending + s);
      pending = "";
    },
  };
  const transport = new Transport(port as SerialPort);
  const loader = new ESPLoader({ transport, baudrate: 115200, terminal });
  try {
    ev.stage("Connecting to the bootloader…");
    try {
      // A board put into download mode by hand answers without a reset.
      await loader.main("no_reset");
    } catch {
      ev.log("No answer; resetting the board into download mode over USB");
      await transport.disconnect().catch(() => {});
      await loader.main("default_reset");
    }
    const chip = loader.chip.CHIP_NAME;
    const wdt = CHIPS[chip];
    const build = manifest.builds.find((b) => b.chipFamily === chip);
    if (!wdt || !build) throw new Error(`this page has no firmware for the ${chip}`);
    await disableWatchdogs(loader, wdt);

    ev.stage("Downloading firmware…");
    const files = await Promise.all(build.parts.map(fetchPart));

    ev.stage(`Writing ${manifest.version}…`);
    const total = build.parts.reduce((s, p) => s + p.size, 0);
    const before = build.parts.map((_, i) => build.parts.slice(0, i).reduce((s, p) => s + p.size, 0));
    await loader.writeFlash({
      fileArray: files.map((data, i) => ({ data, address: build.parts[i].offset })),
      // The bootloader header already carries the right flash settings.
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (i, written, size) => ev.progress((before[i] + (build.parts[i].size * written) / size) / total),
      calculateMD5Hash: (image) => SparkMD5.ArrayBuffer.hash(image.slice().buffer),
    });
    ev.progress(1);

    ev.stage("Restarting the board…");
    await watchdogReset(loader, wdt);
  } finally {
    await transport.disconnect().catch(() => {});
  }
}

/** USB Serial/JTAG does not reset the RTC watchdogs on entering download mode,
 *  and a watchdog the bootloader armed can reset the chip mid-flash. */
async function disableWatchdogs(loader: ESPLoader, w: Watchdogs) {
  await loader.writeReg(w.wdtProtect, w.wdtKey);
  await loader.writeReg(w.wdtConfig0, 0);
  await loader.writeReg(w.wdtProtect, 0);
  await loader.writeReg(w.swdProtect, w.swdKey);
  await loader.writeReg(w.swdConfig, ((await loader.readReg(w.swdConfig)) | w.swdAutoFeed) >>> 0);
  await loader.writeReg(w.swdProtect, 0);
}

/** An RTS reset leaves the C5 in download mode; the RTC watchdog's system
 *  reset boots the app (esptool --after watchdog-reset). */
async function watchdogReset(loader: ESPLoader, w: Watchdogs) {
  await loader.writeReg(w.wdtProtect, w.wdtKey);
  await loader.writeReg(w.wdtConfig1, 2000);
  await loader.writeReg(w.wdtConfig0, ((1 << 31) | (5 << 28) | (1 << 8) | 2) >>> 0);
  // The reset may land before this reply does.
  await loader.writeReg(w.wdtProtect, 0).catch(() => {});
  await new Promise((r) => setTimeout(r, 500));
}
