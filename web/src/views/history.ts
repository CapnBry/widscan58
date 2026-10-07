// Lap-timer style RSSI history for all channels, with crosshair tooltip.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { axes, channelColor, drawGrid, el, prepare, theme } from "../util/canvas";

export class History implements View {
  el: HTMLElement;
  private canvas = el("canvas");
  private tip = el("div", { class: "tooltip" });
  private legend = el("div", { class: "legend" });
  private hoverX: number | null = null;
  private legendFor = "";

  constructor(private app: App) {
    const metric = el("select") as HTMLSelectElement;
    for (const [v, label] of [
      ["ema", "Smoothed RSSI"],
      ["rssi", "Raw RSSI"],
    ])
      metric.append(new Option(label, v));
    metric.value = app.settings.historyMetric;
    metric.onchange = () => {
      app.settings.historyMetric = metric.value as "ema" | "rssi";
      app.changed();
    };
    const span = el("select") as HTMLSelectElement;
    for (const sec of [10, 30, 60, 120]) span.append(new Option(`${sec} s`, String(sec)));
    span.value = String(app.settings.historySeconds);
    span.onchange = () => {
      app.settings.historySeconds = Number(span.value);
      app.changed();
    };
    this.el = el(
      "section",
      { class: "card span-6" },
      el("h2", {}, "RSSI history", el("span", { class: "tools" }, metric, span)),
      el("div", { class: "chart h-hist" }, this.canvas, this.tip),
      this.legend,
    );
    this.canvas.addEventListener("pointermove", (e) => (this.hoverX = e.offsetX));
    this.canvas.addEventListener("pointerleave", () => {
      this.hoverX = null;
      this.tip.style.display = "none";
    });
  }

  render() {
    const app = this.app;
    const store = app.store;
    const p = store.profile;
    const s = prepare(this.canvas);
    if (!p) return;
    const legendKey = `${p.name}/${app.settings.equivAxis}`;
    if (this.legendFor !== legendKey) {
      this.legendFor = legendKey;
      this.legend.replaceChildren(
        ...p.channels.map((_, id) => el("span", {}, el("i", { style: `background:${channelColor(id)}` }), app.channelName(id))),
      );
    }
    const now = store.now();
    const span = app.settings.historySeconds;
    const st = store.status;
    // Y range from the data in view, padded.
    let lo = Infinity;
    let hi = -Infinity;
    const key = app.settings.historyMetric;
    store.channels.forEach((cs, id) => {
      if (st && !(st.enabled & (1 << id))) return;
      cs[key].forEach((t, v) => {
        if (now - t > span) return;
        lo = Math.min(lo, v);
        hi = Math.max(hi, v);
      });
    });
    if (!Number.isFinite(lo)) {
      lo = -70;
      hi = -30;
    }
    lo = Math.floor((lo - 2) / 5) * 5;
    hi = Math.ceil((hi + 2) / 5) * 5;
    const a = axes(s, -span, 0, lo, hi, { top: 16 });
    drawGrid(s, a, (v) => `${v}s`, (v) => `${v}`, 6, 5);
    const ctx = s.ctx;
    store.channels.forEach((cs, id) => {
      if (st && !(st.enabled & (1 << id))) return;
      ctx.strokeStyle = channelColor(id);
      ctx.lineWidth = app.settings.selected === id ? 2.5 : 1.5;
      ctx.globalAlpha = app.settings.selected === id ? 1 : 0.8;
      ctx.beginPath();
      let pen = false;
      let lastX = -1;
      cs[key].forEach((t, v) => {
        const age = now - t;
        if (age > span) return;
        const x = a.sx(-age);
        if (pen && x - lastX < 0.5) return; // decimate to ~2 points per pixel
        const y = a.sy(v);
        if (pen) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
        pen = true;
        lastX = x;
      });
      ctx.stroke();
    });
    ctx.globalAlpha = 1;
    const t = theme();
    ctx.fillStyle = t.muted;
    ctx.font = "10px system-ui";
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    ctx.fillText("dBFS", 2, 1);
    // Crosshair: snap to time, list every channel at that time.
    if (this.hoverX === null || this.hoverX < a.left || this.hoverX > a.right) {
      this.tip.style.display = "none";
      return;
    }
    const tx = a.ix(this.hoverX);
    const x = Math.round(this.hoverX) + 0.5;
    ctx.strokeStyle = t.ink2;
    ctx.globalAlpha = 0.5;
    ctx.beginPath();
    ctx.moveTo(x, a.top);
    ctx.lineTo(x, a.bottom);
    ctx.stroke();
    ctx.globalAlpha = 1;
    const target = now + tx;
    const rows: HTMLElement[] = [];
    const head = el("div", { class: "head" });
    head.textContent = `${tx.toFixed(2)} s`;
    rows.push(head);
    store.channels.forEach((cs, id) => {
      if (st && !(st.enabled & (1 << id))) return;
      let best = Infinity;
      let val = NaN;
      cs[key].forEach((tt, v) => {
        const d = Math.abs(tt - target);
        if (d < best) {
          best = d;
          val = v;
        }
      });
      if (best > 0.5) return;
      const r = el("div", { class: "row" }, el("i", { style: `background:${channelColor(id)}` }), el("b"), el("span"));
      r.children[1].textContent = `${val.toFixed(1)} dBFS`;
      r.children[2].textContent = app.channelName(id);
      rows.push(r);
    });
    this.tip.replaceChildren(...rows);
    this.tip.style.display = "block";
    const w = this.tip.offsetWidth;
    this.tip.style.left = `${this.hoverX + 12 + w > a.right ? this.hoverX - 12 - w : this.hoverX + 12}px`;
    this.tip.style.top = `${a.top + 8}px`;
  }
}
