// "Edit channels" dialog: pick up to 8 channels from an FPV frequency chart.
// GPL-3.0-or-later.
import { BANDS, MAX_CHANNELS, normalizePlan } from "../model/channelPlan";
import { el } from "../util/canvas";

export class ChannelPlanDialog {
  readonly el: HTMLDialogElement;
  private selected = new Set<string>();
  private buttons = new Map<string, HTMLButtonElement>();
  private count = el("span", { class: "plan-count" });
  private okBtn = el("button", { class: "primary", autofocus: "" }, "OK");
  private onOk: (channels: string[]) => void = () => {};

  constructor() {
    const head = el("tr", {}, el("th"), ...BANDS[0].mhz.map((_, j) => el("th", {}, `CH${j + 1}`)));
    const rows = BANDS.map((b) => {
      const bandBtn = el(
        "button",
        { class: "plan-band", title: `Select all of ${b.name} (replaces the selection)` },
        el("b", {}, b.letter),
        el("span", {}, b.name),
      );
      bandBtn.onclick = () => {
        this.selected = new Set(b.mhz.map((_, j) => `${b.letter}${j + 1}`));
        this.sync();
      };
      const cells = b.mhz.map((mhz, j) => {
        const name = `${b.letter}${j + 1}`;
        const btn = el("button", { class: "plan-chan", title: `${name}: ${mhz} MHz` }, el("b", {}, name), el("span", {}, String(mhz)));
        btn.onclick = () => {
          if (this.selected.has(name)) this.selected.delete(name);
          else if (this.selected.size < MAX_CHANNELS) this.selected.add(name);
          this.sync();
        };
        this.buttons.set(name, btn);
        return el("td", {}, btn);
      });
      return el("tr", {}, el("th", {}, bandBtn), ...cells);
    });
    const clearBtn = el("button", {}, "Clear");
    const cancelBtn = el("button", {}, "Cancel");
    clearBtn.onclick = () => {
      this.selected.clear();
      this.sync();
    };
    cancelBtn.onclick = () => this.el.close();
    this.okBtn.onclick = () => {
      this.el.close();
      this.onOk(normalizePlan([...this.selected]));
    };
    this.el = el(
      "dialog",
      { class: "plan-dialog" },
      el("h2", {}, "Channels to scan"),
      el("p", {}, `Select up to ${MAX_CHANNELS} channels. Click a band name to select all eight of its channels.`),
      el("div", { class: "plan-scroll" }, el("table", { class: "plan-chart" }, el("thead", {}, head), el("tbody", {}, ...rows))),
      el("div", { class: "plan-buttons" }, clearBtn, this.count, el("span", { class: "spacer" }), cancelBtn, this.okBtn),
    );
  }

  open(current: string[], onOk: (channels: string[]) => void) {
    this.selected = new Set(current);
    this.onOk = onOk;
    this.sync();
    this.el.showModal();
  }

  private sync() {
    const full = this.selected.size >= MAX_CHANNELS;
    for (const [name, btn] of this.buttons) {
      const on = this.selected.has(name);
      btn.classList.toggle("on", on);
      btn.disabled = full && !on;
    }
    this.count.textContent = `${this.selected.size} of ${MAX_CHANNELS} selected` + (full ? " (maximum)" : "");
    this.okBtn.disabled = this.selected.size === 0;
  }
}
