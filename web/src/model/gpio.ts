// GPIO outputs set at startup (firmware src/gpio_out.c): the GPIO? reply.
// GPL-3.0-or-later.

/** Per pin: not used, low, high, or not available (flash, USB). */
export type GpioSetting = "NONE" | "LOW" | "HIGH" | "RESERVED";

export interface GpioPin {
  n: number;
  /** In effect now. */
  active: GpioSetting;
  /** Applied at startup. */
  saved: GpioSetting;
}

const SETTING: Record<string, GpioSetting> = { "-": "NONE", L: "LOW", H: "HIGH", x: "RESERVED" };

/** `GPIO {"count":29,"active":"--H…","saved":"--L…"}`, one character per GPIO number. */
export function parseGpio(line: string): GpioPin[] {
  if (!line.startsWith("GPIO ")) throw new Error(`not a GPIO reply: ${line}`);
  const j = JSON.parse(line.slice(5)) as { count: number; active: string; saved: string };
  if (j.active.length !== j.count || j.saved.length !== j.count) throw new Error("GPIO reply: wrong length");
  const at = (s: string, n: number) => {
    const v = SETTING[s[n]];
    if (!v) throw new Error(`GPIO reply: bad setting ${s[n]}`);
    return v;
  };
  return Array.from({ length: j.count }, (_, n) => ({ n, active: at(j.active, n), saved: at(j.saved, n) }));
}

/** Pins whose setting in effect would not survive a restart. */
export function gpioUnsaved(pins: GpioPin[]): GpioPin[] {
  return pins.filter((p) => p.active !== p.saved);
}
