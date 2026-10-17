// GPIO Configure (menu): pins the device drives at startup, low or high, such
// as an antenna switch. A change takes effect at once (GPIO <n> ...); Save
// (GPIO SAVE) makes the settings in effect the ones the device starts with.
// Each pin shows its setting, not its live level. GPL-3.0-or-later.
import type { App } from "../model/app";
import { gpioUnsaved, parseGpio, type GpioPin, type GpioSetting } from "../model/gpio";
import { el } from "../util/canvas";

const LABEL: Record<GpioSetting, string> = { NONE: "Not used", LOW: "Low", HIGH: "High", RESERVED: "Reserved" };
const CHOICES: GpioSetting[] = ["NONE", "LOW", "HIGH"];

export class GpioDialog {
  readonly el: HTMLDialogElement;
  private list = el("div", { class: "gpio-list" });
  private status = el("div", { class: "gpio-status" });
  private unsavedNote = el(
    "div",
    { class: "gpio-unsaved", hidden: "" },
    "Unsaved changes: in effect now, but the device goes back to its saved settings when it restarts. Save to keep them.",
  );
  private saveBtn = el("button", { class: "primary" }, "Save") as HTMLButtonElement;
  private revertBtn = el("button", { title: "Put the saved settings back in effect" }, "Revert") as HTMLButtonElement;
  private pins: GpioPin[] = [];
  private busy = false;

  constructor(private app: App) {
    const closeBtn = el("button", { autofocus: "" }, "Close");
    closeBtn.onclick = () => this.el.close();
    this.saveBtn.onclick = () => void this.run(["GPIO SAVE"]);
    this.revertBtn.onclick = () => void this.run(gpioUnsaved(this.pins).map((p) => `GPIO ${p.n} ${p.saved}`));
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
      this.status,
      this.list,
      this.unsavedNote,
      el("div", { class: "flash-buttons" }, this.revertBtn, el("span", { class: "spacer" }), this.saveBtn, closeBtn),
    );
  }

  open() {
    this.pins = [];
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
    this.list.replaceChildren(...this.pins.map((p) => this.row(p)));
  }

  private row(p: GpioPin): HTMLElement {
    const name = el("span", { class: "gpio-name" }, `GPIO ${p.n}`);
    if (p.active === "RESERVED")
      return el("div", { class: "gpio-row reserved", title: "Used by the flash or USB" }, name, el("span", { class: "gpio-reserved" }, "Flash / USB"));
    const seg = el("span", { class: "seg" });
    for (const s of CHOICES) {
      const b = el("button", {}, LABEL[s]) as HTMLButtonElement;
      b.classList.toggle("on", s === p.active);
      b.disabled = this.busy;
      b.onclick = () => {
        if (s !== p.active) void this.run([`GPIO ${p.n} ${s}`]);
      };
      seg.append(b);
    }
    const changed = p.active !== p.saved;
    return el(
      "div",
      { class: `gpio-row${changed ? " changed" : ""}` },
      name,
      seg,
      el("span", { class: "gpio-saved" }, changed ? `saved: ${LABEL[p.saved]}` : ""),
    );
  }
}
