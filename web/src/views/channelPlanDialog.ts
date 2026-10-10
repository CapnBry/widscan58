// "Edit channels" dialog: pick up to 8 channels. Wide screens get the
// channels on a frequency axis (humps, like the usual FPV charts); narrow
// ones a band x channel grid. Both edit the same selection.
// GPL-3.0-or-later.
import { BANDS, MAX_CHANNELS, normalizePlan } from "../model/channelPlan";
import { el } from "../util/canvas";

// Spectrum view axis (MHz) and hump half-width (about an analog video channel).
const AXIS_LO = 5625;
const AXIS_HI = 5965;
const HUMP_HALF = 18;
const SVG = "http://www.w3.org/2000/svg";

const pct = (mhz: number) => ((mhz - AXIS_LO) / (AXIS_HI - AXIS_LO)) * 100;

export class ChannelPlanDialog {
  readonly el: HTMLDialogElement;
  private selected = new Set<string>();
  // Every channel control, grid and spectrum; mhz only on spectrum humps.
  private controls: { name: string; btn: HTMLButtonElement; mhz?: number }[] = [];
  private guides = el("div", { class: "spec-guides" });
  private gridScroll = el("div", { class: "plan-scroll" });
  private gridWrap = el("div", { class: "plan-scroll-wrap plan-grid-view" }, this.gridScroll);
  private count = el("span", { class: "plan-count" });
  private okBtn = el("button", { class: "primary", autofocus: "" }, "OK");
  private onOk: (channels: string[]) => void = () => {};

  constructor() {
    this.gridScroll.append(this.buildGrid());
    this.gridScroll.onscroll = () => this.syncScrollFade();
    new ResizeObserver(() => this.syncScrollFade()).observe(this.gridScroll);
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
      this.buildSpectrum(),
      this.gridWrap,
      el("div", { class: "plan-buttons" }, clearBtn, this.count, el("span", { class: "spacer" }), cancelBtn, this.okBtn),
    );
  }

  open(current: string[], onOk: (channels: string[]) => void) {
    this.selected = new Set(current);
    this.onOk = onOk;
    this.sync();
    this.el.showModal();
    this.syncScrollFade();
  }

  private bandButton(b: (typeof BANDS)[number]): HTMLButtonElement {
    const btn = el("button", { class: "plan-band", title: `Select all of ${b.name} (replaces the selection)` }, el("b", {}, b.letter), el("span", {}, b.name));
    btn.onclick = () => {
      this.selected = new Set(b.mhz.map((_, j) => `${b.letter}${j + 1}`));
      this.sync();
    };
    return btn;
  }

  private toggle(name: string) {
    if (this.selected.has(name)) this.selected.delete(name);
    else if (this.selected.size < MAX_CHANNELS) this.selected.add(name);
    this.sync();
  }

  private addControl(name: string, btn: HTMLButtonElement, mhz?: number) {
    btn.onclick = () => this.toggle(name);
    this.controls.push({ name, btn, mhz });
  }

  private buildGrid(): HTMLTableElement {
    const head = el("tr", {}, el("th"), ...BANDS[0].mhz.map((_, j) => el("th", {}, `CH${j + 1}`)));
    const rows = BANDS.map((b) => {
      const cells = b.mhz.map((mhz, j) => {
        const name = `${b.letter}${j + 1}`;
        const btn = el("button", { class: "plan-chan", title: `${name}: ${mhz} MHz` }, el("b", {}, name), el("span", {}, String(mhz)));
        this.addControl(name, btn);
        return el("td", {}, btn);
      });
      return el("tr", {}, el("th", {}, this.bandButton(b)), ...cells);
    });
    return el("table", { class: "plan-chart" }, el("thead", {}, head), el("tbody", {}, ...rows));
  }

  private buildSpectrum(): HTMLElement {
    // Hump fill for unselected channels; selected ones use the accent colour.
    const defs = document.createElementNS(SVG, "svg");
    defs.setAttribute("class", "spec-defs");
    defs.innerHTML =
      '<defs><linearGradient id="plan-hump-off" x1="0" y1="0" x2="0" y2="1">' +
      '<stop offset="0" stop-color="#6b6a65"/><stop offset="1" stop-color="#2a2a28" stop-opacity="0.6"/></linearGradient></defs>';
    const axis = el("div", { class: "spec-track" });
    for (let f = 5630; f <= 5960; f += 10) {
      const tick = el("div", { class: f % 50 ? "spec-tick" : "spec-tick major", style: `left:${pct(f)}%` });
      if (!(f % 50)) tick.append(el("span", {}, String(f)));
      axis.append(tick);
    }
    const rows = BANDS.map((b) => {
      const track = el("div", { class: "spec-track" });
      b.mhz.forEach((mhz, j) => {
        const name = `${b.letter}${j + 1}`;
        const hump = el("button", {
          class: "spec-hump",
          title: `${name}: ${mhz} MHz`,
          style: `left:${pct(mhz - HUMP_HALF)}%;width:${pct(AXIS_LO + 2 * HUMP_HALF)}%`,
        });
        const svg = document.createElementNS(SVG, "svg");
        svg.setAttribute("viewBox", "0 0 100 100");
        svg.setAttribute("preserveAspectRatio", "none");
        svg.innerHTML = '<path d="M0,100 C28,100 30,6 50,6 C70,6 72,100 100,100 Z"/>';
        hump.append(svg, el("span", { class: "spec-label" }, el("b", {}, name), el("small", {}, String(mhz))));
        this.addControl(name, hump, mhz);
        track.append(hump);
      });
      return el("div", { class: "spec-row" }, this.bandButton(b), track);
    });
    return el(
      "div",
      { class: "spec-view" },
      defs,
      el("div", { class: "spec" }, el("div", { class: "spec-row spec-axis" }, el("div"), axis), ...rows, this.guides),
    );
  }

  private sync() {
    const full = this.selected.size >= MAX_CHANNELS;
    for (const { name, btn } of this.controls) {
      const on = this.selected.has(name);
      btn.classList.toggle("on", on);
      btn.disabled = full && !on;
    }
    // Dashed lines through every band at each selected frequency: shows
    // near-collisions across bands (e.g. A8, R3, B1).
    const guides = this.controls.filter((c) => c.mhz !== undefined && this.selected.has(c.name));
    this.guides.replaceChildren(...guides.map((c) => el("div", { class: "spec-guide", style: `left:${pct(c.mhz!)}%` })));
    this.count.textContent = `${this.selected.size} of ${MAX_CHANNELS} selected` + (full ? " (maximum)" : "");
    this.okBtn.disabled = this.selected.size === 0;
  }

  /** Fade the grid's edges where there is more to scroll to. */
  private syncScrollFade() {
    const s = this.gridScroll;
    this.gridWrap.classList.toggle("more-left", s.scrollLeft > 1);
    this.gridWrap.classList.toggle("more-right", s.scrollLeft + s.clientWidth < s.scrollWidth - 1);
  }
}
