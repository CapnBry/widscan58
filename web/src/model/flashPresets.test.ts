// GPL-3.0-or-later.
import { describe, expect, it } from "vitest";
import { FLASH_PRESETS, presetWarning } from "./flashPresets";

describe("FLASH_PRESETS", () => {
  it("starts with None, which does nothing", () => {
    const p = FLASH_PRESETS[0];
    expect([p.name, p.group, p.eraseFlash, p.commands]).toEqual(["None", "Basic", undefined, []]);
    expect(presetWarning(p)).toBeNull();
  });

  it("keeps groups together and Hardware sorted by name", () => {
    const groups = FLASH_PRESETS.map((p) => p.group).filter((g, i, a) => g !== a[i - 1]);
    expect(groups).toEqual(["Basic", "Hardware"]);
    const hw = FLASH_PRESETS.filter((p) => p.group === "Hardware").map((p) => p.name);
    expect(hw).toEqual([...hw].sort());
  });

  it("erases only for Erase flash", () => {
    expect(FLASH_PRESETS.filter((p) => p.eraseFlash).map((p) => p.name)).toEqual(["Erase flash"]);
  });

  it("clears both configurations first and saves what it sets", () => {
    for (const p of FLASH_PRESETS.filter((x) => x.commands.length)) {
      expect(p.commands.slice(0, 2)).toEqual(["CONFIG RESET", "GPIO RESET"]);
      expect(p.commands).toContain("GPIO SAVE");
      expect(presetWarning(p)).toContain("erased after flashing");
      if (p.commands.some((c) => c.startsWith("GAIN"))) expect(p.commands.at(-1)).toBe("CONFIG SAVE");
    }
  });
});
