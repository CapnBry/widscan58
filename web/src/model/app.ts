// Shared UI context passed to every view. GPL-3.0-or-later.
import type { Device } from "../serial/device";
import type { Store } from "./store";
import { equivKhz } from "../proto/frames";

export interface View {
  el: HTMLElement;
  render(): void;
}

export interface UiSettings {
  selected: number; // channel id for zoom / raw IQ
  equivAxis: boolean; // label scaled profiles in their 5.8 GHz equivalent
  showMaxHold: boolean;
  showAverage: boolean;
  historyMetric: "ema" | "rssi";
  historySeconds: number;
  dbMin: number;
  dbMax: number;
}

export class App {
  device: Device | null = null;
  readOnly = false; // replay
  settings: UiSettings = {
    selected: 0,
    equivAxis: false,
    showMaxHold: true,
    showAverage: true,
    historyMetric: "ema",
    historySeconds: 30,
    dbMin: -100,
    dbMax: -20,
  };
  private listeners = new Set<() => void>();

  constructor(readonly store: Store) {
    try {
      const saved = localStorage.getItem("rx5808.settings");
      if (saved) Object.assign(this.settings, JSON.parse(saved));
    } catch {
      /* storage unavailable */
    }
  }

  changed() {
    try {
      localStorage.setItem("rx5808.settings", JSON.stringify(this.settings));
    } catch {
      /* storage unavailable */
    }
    for (const fn of this.listeners) fn();
    this.store.touch();
  }

  onChange(fn: () => void) {
    this.listeners.add(fn);
  }

  /** Send a command; errors land in the log instead of throwing. */
  async send(line: string): Promise<string | null> {
    if (!this.device || this.readOnly) {
      this.store.addLog("error", `not connected: ${line}`);
      return null;
    }
    this.store.addLog("tx", line);
    try {
      const item = await this.device.command(line);
      return item.kind === "text" ? item.line : "frame";
    } catch (e) {
      this.store.addLog("error", (e as Error).message);
      return null;
    }
  }

  /** Re-read STATUS after a configuration change. */
  async refreshStatus() {
    if (!this.device || this.readOnly) return;
    try {
      await this.device.command("STATUS?");
    } catch (e) {
      this.store.addLog("error", (e as Error).message);
    }
  }

  /** Display frequency in MHz for an absolute kHz value (optionally 5.8 GHz equivalent). */
  displayMHz(khz: number): number {
    const p = this.store.profile;
    if (!p || !this.settings.equivAxis || p.scale === 1) return khz / 1000;
    return equivKhz(p, khz) / 1000;
  }

  /** Display a frequency offset (Hz) in kHz, scaled back to the 5.8 GHz equivalent if requested. */
  displayOffsetKhz(hz: number): number {
    const p = this.store.profile;
    const s = p && this.settings.equivAxis ? p.scale : 1;
    return hz / 1000 / s;
  }
}
