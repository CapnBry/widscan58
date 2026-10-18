// Shared UI context passed to every view. GPL-3.0-or-later.
import type { Device } from "../serial/device";
import type { Store } from "./store";
import { loadDeviceSettings, restoreCommands, saveDeviceSettings, type DeviceSettings } from "./channelPlan";
import { RhModel } from "./rh";

export interface View {
  el: HTMLElement;
  render(): void;
}

export interface UiSettings {
  selected: number; // channel id for zoom / raw IQ
  showMaxHold: boolean;
  showAverage: boolean;
  historyMetric: "ema" | "rssi";
  historySeconds: number;
  dbMin: number;
  dbMax: number;
  uiMode: "monitor" | "rh" | "power"; // which page; power meter mode runs the monitor protocol
  rhShowCrossings: boolean; // detection events: list enters and exits
  rhShowFastest: boolean; // detection events: list new fastest runs
  keepScreenOn: boolean; // screen wake lock while a device is connected
  voiceOn: boolean; // RotorHazard mode: speak each lap
  voiceVolume: number; // 0-100
  voiceRate: number; // 0.1-2
  voicePitch: number; // 0-2
  powerAttenDb: number; // power meter mode: attenuation between the VTX and the board
  powerSmoothS: number; // power meter mode: averaging, seconds (0: the latest hop)
}

export class App {
  device: Device | null = null;
  readOnly = false; // replay
  settings: UiSettings = {
    selected: 0,
    showMaxHold: true,
    showAverage: true,
    historyMetric: "ema",
    historySeconds: 30,
    dbMin: -85,
    dbMax: -20,
    uiMode: "monitor",
    rhShowCrossings: false,
    rhShowFastest: true,
    keepScreenOn: false,
    voiceOn: false,
    voiceVolume: 100,
    voiceRate: 1,
    voicePitch: 1,
    powerAttenDb: 30,
    powerSmoothS: 0.5,
  };
  /** Channel plan, hop mode and gain, restored on every connect. */
  deviceSettings: DeviceSettings = loadDeviceSettings();
  /** RotorHazard mode: seats, polling and detection events. */
  rh: RhModel;
  private listeners = new Set<() => void>();

  constructor(readonly store: Store) {
    this.rh = new RhModel(store);
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

  updateDeviceSettings(patch: Partial<DeviceSettings>) {
    Object.assign(this.deviceSettings, patch);
    saveDeviceSettings(this.deviceSettings);
  }

  /** Put the device in the stored state (channel plan, mode, gain). */
  async restoreDevice(device: Device) {
    for (const cmd of restoreCommands(this.deviceSettings)) {
      this.store.addLog("tx", cmd);
      try {
        await device.command(cmd);
      } catch (e) {
        this.store.addLog("error", (e as Error).message);
      }
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

  /** Channel name for display (band and number, e.g. R1). */
  channelName(id: number): string {
    return this.store.profile?.channels[id]?.name ?? "";
  }
}
