// "Config preset" in the flash dialog: commands sent to the board once it runs
// the new firmware, and whether to erase its flash first. The board boots in
// lap timer mode, where all of these work (firmware commands.c, rh_allowed).
// GPL-3.0-or-later.

export interface FlashPreset {
  name: string;
  /** Dropdown section; a preset's section is the group of the ones around it. */
  group: string;
  /** Erase the whole flash before writing the firmware (esptool's erase_flash). */
  eraseFlash?: boolean;
  /** Each must answer OK; empty: nothing is sent after flashing. */
  commands: string[];
}

const CLEAR = ["CONFIG RESET", "GPIO RESET"];

/** In display order: keep each group together, and "Hardware" sorted by name. */
export const FLASH_PRESETS: FlashPreset[] = [
  { name: "None", group: "Basic", commands: [] },
  { name: "Erase flash", group: "Basic", eraseFlash: true, commands: [] },
  { name: "Espressif C5 DevKit", group: "Hardware", commands: [...CLEAR, "GPIO 27 WS2812", "GPIO SAVE"] },
  { name: "Seeed Studio XIAO C5", group: "Hardware", commands: [...CLEAR, "GPIO 27 LED", "GPIO SAVE"] },
  {
    name: "Waveshare C5 Mini - Builtin Antenna",
    group: "Hardware",
    commands: [...CLEAR, "GPIO 26 LOW", "GPIO 27 WS2812", "GPIO SAVE", "GAIN MANUAL 40", "CONFIG SAVE"],
  },
  {
    name: "Waveshare C5 Mini - External Antenna",
    group: "Hardware",
    commands: [...CLEAR, "GPIO 26 HIGH", "GPIO 27 WS2812", "GPIO SAVE"],
  },
];

/** What the selected preset does to the board's settings; null for None. */
export function presetWarning(p: FlashPreset): string | null {
  if (p.eraseFlash) return "The whole flash is erased before flashing: the device loses its configuration.";
  if (p.commands.length) return "The device's configuration is erased after flashing and replaced by this preset.";
  return null;
}
