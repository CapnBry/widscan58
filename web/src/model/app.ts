// Shared UI context passed to every view. GPL-3.0-or-later.
import type { Device } from "../serial/device";
import type { Store } from "./store";
import { equivKhz } from "../proto/frames";

// Raceband R1-R8 (MHz). A scaled profile's channel takes the Raceband name
// its 5.8 GHz equivalent lands on.
const RACEBAND_MHZ = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917];

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
      const saved = localStorage.getItem("widscan58.settings");
      if (saved) Object.assign(this.settings, JSON.parse(saved));
    } catch {
      /* storage unavailable */
    }
  }

  changed() {
    try {
      localStorage.setItem("widscan58.settings", JSON.stringify(this.settings));
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

  /** Channel name for display: the Raceband name (R1-R8) of a scaled channel
   *  on the 5.8 GHz axis, otherwise the profile's own name. */
  channelName(id: number): string {
    const p = this.store.profile;
    const c = p?.channels[id];
    if (!c) return "";
    if (!this.settings.equivAxis || p.scale === 1) return c.name;
    const mhz = equivKhz(p, c.khz) / 1000;
    const r = RACEBAND_MHZ.findIndex((f) => Math.abs(f - mhz) < 1);
    return r >= 0 ? `R${r + 1}` : c.name;
  }

  /** Display a frequency offset (Hz) in kHz, scaled back to the 5.8 GHz equivalent if requested. */
  displayOffsetKhz(hz: number): number {
    const p = this.store.profile;
    const s = p && this.settings.equivAxis ? p.scale : 1;
    return hz / 1000 / s;
  }
}
