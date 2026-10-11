// 5.8 GHz FPV bands, and the device settings the UI restores on connect.
// GPL-3.0-or-later.

export interface Band {
  letter: string;
  name: string;
  mhz: number[]; // CH1..CH8
}

// Raceband first, then the order of the usual FPV frequency charts.
export const BANDS: Band[] = [
  { letter: "R", name: "Raceband", mhz: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917] },
  { letter: "A", name: "Boscam A", mhz: [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725] },
  { letter: "B", name: "Boscam B", mhz: [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866] },
  { letter: "E", name: "Boscam E", mhz: [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945] },
  { letter: "F", name: "Fatshark", mhz: [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880] },
];

/** The firmware scans at most this many channels. */
export const MAX_CHANNELS = 8;

/** Frequency (MHz) of a channel name such as "R1", or undefined. */
export function channelMhz(name: string): number | undefined {
  const band = BANDS.find((b) => b.letter === name[0]);
  const n = Number(name.slice(1));
  return band && Number.isInteger(n) && n >= 1 && n <= 8 ? band.mhz[n - 1] : undefined;
}

/** Known names only, no duplicates, at most MAX_CHANNELS, sorted by frequency
 *  (ties keep chart order) so channel ids run up the band like the panorama. */
export function normalizePlan(names: string[]): string[] {
  const order = BANDS.flatMap((b) => b.mhz.map((_, j) => `${b.letter}${j + 1}`));
  const known = [...new Set(names)].filter((n) => channelMhz(n) !== undefined).slice(0, MAX_CHANNELS);
  return known.sort((a, b) => channelMhz(a)! - channelMhz(b)! || order.indexOf(a) - order.indexOf(b));
}

/** CHANNELS command for a plan: each channel by its 5.8 GHz frequency. */
export function channelsCommand(names: string[]): string {
  return `CHANNELS ${names.map((n) => `${n}=${channelMhz(n)}`).join(" ")}`;
}

export interface DeviceSettings {
  channels: string[];
  mode: "GROUP" | "SINGLE";
  gainMode: "MANUAL" | "HARDWARE";
  gain: number;
}

export const DEFAULT_DEVICE_SETTINGS: DeviceSettings = {
  channels: ["R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"],
  mode: "GROUP",
  gainMode: "MANUAL",
  gain: 50,
};

const KEY = "widscan58.device";

export function loadDeviceSettings(): DeviceSettings {
  const s = { ...DEFAULT_DEVICE_SETTINGS };
  try {
    const saved = localStorage.getItem(KEY);
    if (saved) Object.assign(s, JSON.parse(saved));
  } catch {
    /* storage unavailable or corrupt */
  }
  s.channels = normalizePlan(Array.isArray(s.channels) ? s.channels : []);
  if (!s.channels.length) s.channels = [...DEFAULT_DEVICE_SETTINGS.channels];
  if (s.mode !== "SINGLE") s.mode = "GROUP";
  if (s.gainMode !== "HARDWARE") s.gainMode = "MANUAL";
  if (!Number.isInteger(s.gain) || s.gain < 0) s.gain = DEFAULT_DEVICE_SETTINGS.gain;
  return s;
}

export function saveDeviceSettings(s: DeviceSettings) {
  try {
    localStorage.setItem(KEY, JSON.stringify(s));
  } catch {
    /* storage unavailable */
  }
}

/** Commands that put a freshly connected (or rebooted) device in the stored state. */
export function restoreCommands(s: DeviceSettings): string[] {
  return [channelsCommand(s.channels), `MODE ${s.mode}`, s.gainMode === "MANUAL" ? `GAIN MANUAL ${s.gain}` : "GAIN HARDWARE"];
}
