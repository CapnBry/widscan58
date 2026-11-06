// GPIO Configure (menu): pins the device drives at startup, low or high, such
// as an antenna switch, the pins of its UART host port (TX and RX) and its
// status LEDs (LED and WS2812); one pin each of those: choosing one moves it
// there. A change takes effect at once (GPIO <n> ...); Save (GPIO SAVE) makes
// the settings in effect the ones the device starts with; Defaults (GPIO RESET)
// puts back the built-in ones, now and at startup. Each pin shows its setting,
// not its live level. The pins are on tabs of PINS_PER_TAB.
// GPL-3.0-or-later.
import type { App } from "../model/app";
import { gpioUnsaved, parseGpio, type GpioPin, type GpioSetting } from "../model/gpio";
import { el } from "../util/canvas";

const LABEL: Record<GpioSetting, string> = {
  NONE: "Unused",
  LOW: "Low",
  HIGH: "High",
  TX: "TX",
  RX: "RX",
  LED: "LED",
  WS2812: "WS2812",
  RESERVED: "Reserved",
};
const CHOICES: GpioSetting[] = ["NONE", "LOW", "HIGH", "TX", "RX", "LED", "WS2812"];
const TITLE: Partial<Record<GpioSetting, string>> = {
  TX: "UART transmit (to the host's RX)",
  RX: "UART receive (from the host's TX)",
  LED: "Status LED, lit when the pin is low",
  WS2812: "Status LED: one WS2812 on this pin",
};
// Marks the row of a pin with one of the single-pin roles.
const ROLE_CLASS: Partial<Record<GpioSetting, string>> = { TX: "uart-tx", RX: "uart-rx", LED: "led", WS2812: "ws2812" };
const PINS_PER_TAB = 8;

export class GpioDialog {
  readonly el: HTMLDialogElement;
  private tabs = el("div", { class: "gpio-tabs", role: "tablist" });
  private list = el("div", { class: "gpio-list" });
  private tab = 0;
  private status = el("div", { class: "gpio-status" });
  private unsavedNote = el(
    "div",
    { class: "gpio-unsaved", hidden: "" },
    "Unsaved changes: in effect now, but the device goes back to its saved settings (highlighted) when it restarts. Save to keep them.",
  );
  private saveBtn = el("button", { class: "primary" }, "Save") as HTMLButtonElement;
  private revertBtn = el("button", { title: "Put the saved settings back in effect" }, "Revert") as HTMLButtonElement;
  private defaultsBtn = el(
    "button",
    { title: "Clear the saved settings: no outputs, the UART on its default pins" },
    "Defaults",
  ) as HTMLButtonElement;
  private pins: GpioPin[] = [];
  private busy = false;

  constructor(private app: App) {
    const closeBtn = el("button", { autofocus: "" }, "Close");
    closeBtn.onclick = () => this.el.close();
    this.saveBtn.onclick = () => void this.run(["GPIO SAVE"]);
    this.revertBtn.onclick = () => void this.run(gpioUnsaved(this.pins).map((p) => `GPIO ${p.n} ${p.saved}`));
    this.defaultsBtn.onclick = () => void this.run(["GPIO RESET"]);
    this.el = el(
      "dialog",
      { class: "options-dialog gpio-dialog" },
      el("h2", {}, "GPIO configuration"),
      el(
        "p",
        {},
        "Pins the device sets as outputs when it starts, low or high, such as an antenna switch. " +
          "A change takes effect immediately.",
      ),
      el(
        "p",
        {},
        "TX and RX are a serial port for a host wired to the pins instead of USB (921600 baud). " +
          "The device answers whichever port last received data.",
      ),
      el(
        "p",
        {},
        "LED and WS2812 show the status on one LED each.",
      ),
      this.status,
      this.tabs,
      this.list,
      this.unsavedNote,
      el(
        "div",
        { class: "flash-buttons" },
        this.defaultsBtn,
        this.revertBtn,
        el("span", { class: "spacer" }),
        this.saveBtn,
        closeBtn,
      ),
    );
  }

  open() {
    this.pins = [];
    this.tab = 0;
    this.sync();
    this.el.showModal();
    void this.run([]);
  }

  /** Send the commands, then read the settings back. */
  private async run(cmds: string[]) {
    if (!this.app.device || this.app.readOnly) {
      this.pins = [];
      this.setStatus("Connect a device to configure its GPIOs.");
      return this.sync();
    }
    this.busy = true;
    this.sync();
    try {
      for (const c of cmds) if ((await this.app.send(c)) === null) throw new Error(`${c} failed`);
      const reply = await this.app.send("GPIO?");
      if (reply === null) throw new Error("This firmware has no GPIO configuration: flash a newer one.");
      this.pins = parseGpio(reply);
      this.setStatus("");
    } catch (e) {
      this.setStatus((e as Error).message, true);
    }
    this.busy = false;
    this.sync();
  }

  private setStatus(text: string, err = false) {
    this.status.textContent = text;
    this.status.classList.toggle("err", err);
    this.status.hidden = !text;
  }

  private sync() {
    const unsaved = gpioUnsaved(this.pins).length > 0;
    this.unsavedNote.hidden = !unsaved;
    this.saveBtn.disabled = this.busy || !unsaved;
    this.revertBtn.disabled = this.busy || !unsaved;
    this.defaultsBtn.disabled = this.busy || this.pins.length === 0;
    const ntabs = Math.ceil(this.pins.length / PINS_PER_TAB);
    this.tabs.replaceChildren(...Array.from({ length: ntabs }, (_, t) => this.tabButton(t)));
    const first = this.tab * PINS_PER_TAB;
    this.list.replaceChildren(...this.pins.slice(first, first + PINS_PER_TAB).map((p) => this.row(p)));
  }

  private tabButton(t: number): HTMLElement {
    const pins = this.pins.slice(t * PINS_PER_TAB, (t + 1) * PINS_PER_TAB);
    const on = t === this.tab;
    const b = el(
      "button",
      { class: `gpio-tab${on ? " on" : ""}${gpioUnsaved(pins).length ? " changed" : ""}`, role: "tab", "aria-selected": String(on) },
      `${pins[0].n}–${pins[pins.length - 1].n}`,
    );
    b.onclick = () => {
      this.tab = t;
      this.sync();
    };
    return b;
  }

  private row(p: GpioPin): HTMLElement {
    const name = el("span", { class: "gpio-name" }, el("span", { class: "gpio-prefix" }, "GPIO "), String(p.n));
    if (p.active === "RESERVED")
      return el("div", { class: "gpio-row reserved", title: "Used by the flash or USB" }, name, el("span", { class: "gpio-reserved" }, "Flash / USB"));
    const seg = el("span", { class: "seg" });
    for (const s of CHOICES) {
      const b = el("button", TITLE[s] ? { title: TITLE[s] } : {}, LABEL[s]) as HTMLButtonElement;
      b.classList.toggle("on", s === p.active);
      // The saved setting of a changed pin, in the unsaved note's colour.
      if (s === p.saved && s !== p.active) {
        b.classList.add("was");
        b.title = "Saved setting";
      }
      b.disabled = this.busy;
      b.onclick = () => {
        if (s !== p.active) void this.run([`GPIO ${p.n} ${s}`]);
      };
      seg.append(b);
    }
    const changed = p.active !== p.saved;
    return el(
      "div",
      { class: `gpio-row${changed ? " changed" : ""}${ROLE_CLASS[p.active] ? ` ${ROLE_CLASS[p.active]}` : ""}` },
      name,
      seg,
    );
  }
}
