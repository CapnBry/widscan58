// Band panorama (stitched spectrum of every LO segment) and waterfall.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import type { Segment } from "../model/store";
import { axes, channelColor, drawGrid, el, prepare, seqLut, theme, type Axes } from "../util/canvas";

/** Composite of all segments on a pixel grid; each point comes from the segment
 *  whose LO is nearest (the analog filter rolls off at segment edges). */
export interface Composite {
  f0: number; // kHz
  f1: number;
  live: Float32Array;
  max: Float32Array;
  avg: Float32Array;
  floor: Float32Array;
}

/** The span of the LO segments scanned (stale ones drop out after 3 s,
 *  store.ts), or the profile's band before the first spectrum. */
export function bandRange(app: App): [number, number] {
  const p = app.store.profile!;
  if (!app.store.segments.size) return [p.band_khz[0], p.band_khz[1]];
  let lo = Infinity;
  let hi = -Infinity;
  for (const s of app.store.segments.values()) {
    const a = s.latest.loKhz + s.latest.startOffHz / 1000;
    const b = a + (s.latest.db.length * s.latest.binHz) / 1000;
    lo = Math.min(lo, a);
    hi = Math.max(hi, b);
  }
  return [lo, hi];
}

export function composite(app: App, f0: number, f1: number, n: number): Composite {
  const live = new Float32Array(n).fill(NaN);
  const max = new Float32Array(n).fill(NaN);
  const avg = new Float32Array(n).fill(NaN);
  const floor = new Float32Array(n).fill(NaN);
  const best = new Float32Array(n).fill(Infinity);
  const segs: Segment[] = [...app.store.segments.values()];
  for (const s of segs) {
    const sp = s.latest;
    const start = sp.loKhz + sp.startOffHz / 1000; // center of bin 0
    const bin = sp.binHz / 1000;
    const fl = app.store.floorByLo.get(sp.loKhz);
    for (let x = 0; x < n; x++) {
      const f = f0 + ((x + 0.5) / n) * (f1 - f0);
      const k = Math.round((f - start) / bin);
      if (k < 0 || k >= sp.db.length) continue;
      const d = Math.abs(f - sp.loKhz);
      if (d >= best[x]) continue;
      best[x] = d;
      live[x] = sp.db[k];
      max[x] = s.maxHold[k];
      avg[x] = 10 * Math.log10(s.avg[k] + 1e-30);
      floor[x] = fl ?? NaN;
    }
  }
  return { f0, f1, live, max, avg, floor };
}

function strokeSeries(ctx: CanvasRenderingContext2D, a: Axes, data: Float32Array, color: string, width: number, alpha = 1) {
  ctx.strokeStyle = color;
  ctx.globalAlpha = alpha;
  ctx.lineWidth = width;
  ctx.beginPath();
  let pen = false;
  const n = data.length;
  for (let x = 0; x < n; x++) {
    const v = data[x];
    if (Number.isNaN(v)) {
      pen = false;
      continue;
    }
    const px = a.left + ((x + 0.5) / n) * (a.right - a.left);
    const py = a.sy(Math.max(a.y0, Math.min(a.y1, v)));
    if (pen) ctx.lineTo(px, py);
    else ctx.moveTo(px, py);
    pen = true;
  }
  ctx.stroke();
  ctx.globalAlpha = 1;
}

export class Panorama implements View {
  el: HTMLElement;
  private canvas = el("canvas");
  private water = el("canvas");
  private tip = el("div", { class: "tooltip" });
  private hoverX: number | null = null;
  private lastAxes: Axes | null = null;
  private lastComp: Composite | null = null;
  // Waterfall ring of rows, each a composite "live" row at fixed width, stored
  // as colour-ramp indices fixed at capture time so history never recolours.
  private rows: Uint8Array[] = [];
  private rowTimes: number[] = [];
  private rowWidth = 600;
  private lastRowVersion = 0;
  private lastRowRange = "";
  private lut: Uint8ClampedArray | null = null;
  private lutKey = "";
  private img: ImageData | null = null;
  private wfFloor = NaN; // smoothed colour-scale reference (dBFS/bin)
  // Persistent CPU-backed scratch canvas holding the waterfall bitmap. A fresh
  // (GPU-backed) canvas per frame sometimes drew blank, flashing the waterfall.
  private scratch: OffscreenCanvas | null = null;
  private scratchCtx: OffscreenCanvasRenderingContext2D | null = null;

  constructor(private app: App) {
    const holdBtn = el("button", {}, "Reset holds");
    holdBtn.onclick = () => app.store.resetHolds();
    const maxBtn = el("button", {}, "Max hold");
    const avgBtn = el("button", {}, "Average");
    const sync = () => {
      maxBtn.classList.toggle("on", app.settings.showMaxHold);
      avgBtn.classList.toggle("on", app.settings.showAverage);
    };
    maxBtn.onclick = () => {
      app.settings.showMaxHold = !app.settings.showMaxHold;
      app.changed();
    };
    avgBtn.onclick = () => {
      app.settings.showAverage = !app.settings.showAverage;
      app.changed();
    };
    app.onChange(sync);
    sync();
    const autoBtn = el("button", { title: "Fit the dB range to the current spectrum" }, "Auto scale");
    autoBtn.onclick = () => this.autoScale();
    const t = theme();
    const legend = el(
      "div",
      { class: "legend" },
      el("span", {}, el("i", { style: `background:${t.ink}` }), "Live (max of each display bin)"),
      el("span", {}, el("i", { style: `background:${t.warning}` }), "Max hold"),
      el("span", {}, el("i", { style: `background:${t.accent}` }), "Average"),
      el("span", {}, el("i", { style: `background:${t.muted}` }), "Noise floor per FFT bin"),
      el("span", {}, el("i", { class: "box", style: `background:${t.muted};opacity:.35` }), "RSSI window"),
    );
    const chart = el("div", { class: "chart h-pano" }, this.canvas, this.tip);
    const waterChart = el("div", { class: "chart h-water" }, this.water);
    this.el = el(
      "section",
      { class: "card span-12" },
      el("h2", {}, "Band panorama", el("span", { class: "tools" }, maxBtn, avgBtn, autoBtn, holdBtn)),
      chart,
      legend,
      el("h2", { style: "margin-top:10px" }, "Waterfall", el("span", { class: "tools muted" }, "newest at top · live trace per rotation")),
      waterChart,
    );
    this.canvas.addEventListener("pointermove", (e) => {
      this.hoverX = e.offsetX;
    });
    this.canvas.addEventListener("pointerleave", () => {
      this.hoverX = null;
      this.tip.style.display = "none";
    });
    this.canvas.addEventListener("click", (e) => this.pick(e.offsetX));
    this.water.addEventListener("click", (e) => this.pick(e.offsetX));
  }

  private pick(px: number) {
    const a = this.lastAxes;
    const p = this.app.store.profile;
    if (!a || !p) return;
    const khz = a.ix(px);
    let best = 0;
    p.channels.forEach((c, i) => {
      if (Math.abs(c.khz - khz) < Math.abs(p.channels[best].khz - khz)) best = i;
    });
    this.app.settings.selected = best;
    this.app.changed();
  }

  private autoScale() {
    const c = this.lastComp;
    if (!c) return;
    let lo = Infinity;
    let hi = -Infinity;
    for (let j = 0; j < c.live.length; j++) {
      const v = c.live[j];
      if (Number.isNaN(v)) continue;
      lo = Math.min(lo, v);
      hi = Math.max(hi, Number.isNaN(c.max[j]) ? v : c.max[j]);
    }
    if (!Number.isFinite(lo)) return;
    this.app.settings.dbMin = Math.floor((lo - 5) / 5) * 5;
    this.app.settings.dbMax = Math.ceil((hi + 5) / 5) * 5;
    this.app.changed();
  }

  render() {
    const app = this.app;
    const store = app.store;
    const p = store.profile;
    const s = prepare(this.canvas);
    if (!p || !store.segments.size) {
      s.ctx.fillStyle = theme().muted;
      s.ctx.font = "12px system-ui";
      s.ctx.fillText("Waiting for spectrum frames…", 50, 40);
      return;
    }
    const t = theme();
    const [f0, f1] = bandRange(app);
    const a = axes(s, f0, f1, app.settings.dbMin, app.settings.dbMax, { left: 44, top: 16 });
    this.lastAxes = a;
    const ctx = s.ctx;
    const st = store.status;
    // Channel windows first, so traces draw over them. Clipped to the plot:
    // disabled channels can lie outside the scanned span.
    ctx.save();
    ctx.beginPath();
    ctx.rect(a.left, a.top, a.right - a.left, a.bottom - a.top);
    ctx.clip();
    p.channels.forEach((c, id) => {
      const enabled = st ? (st.enabled & (1 << id)) !== 0 : true;
      const rssi = (st?.rssi_hz ?? p.rssi_hz) / 1000;
      const search = (st?.search_hz ?? p.search_hz) / 1000;
      const col = channelColor(id);
      ctx.globalAlpha = enabled ? (app.settings.selected === id ? 0.3 : 0.16) : 0.05;
      ctx.fillStyle = col;
      ctx.fillRect(a.sx(c.khz - rssi), a.top, a.sx(c.khz + rssi) - a.sx(c.khz - rssi), a.bottom - a.top);
      ctx.globalAlpha = enabled ? 0.55 : 0.15;
      ctx.strokeStyle = col;
      ctx.setLineDash([3, 3]);
      ctx.strokeRect(a.sx(c.khz - search) + 0.5, a.top + 0.5, a.sx(c.khz + search) - a.sx(c.khz - search), a.bottom - a.top);
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    });
    ctx.restore();
    drawGrid(s, a, (v) => (v / 1000).toFixed(1), (v) => `${v}`, 10, 5);
    // Channel labels and the LO markers of the current plan.
    ctx.font = "600 11px system-ui";
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    p.channels.forEach((c, id) => {
      if (c.khz < f0 || c.khz > f1) return;
      ctx.fillStyle = t.ink2;
      ctx.fillText(app.channelName(id), a.sx(c.khz), a.top + 2);
    });
    if (st) {
      ctx.fillStyle = t.muted;
      for (const h of st.plan) {
        const x = a.sx(h.lo_khz);
        ctx.beginPath();
        ctx.moveTo(x, a.bottom);
        ctx.lineTo(x - 4, a.bottom - 6);
        ctx.lineTo(x + 4, a.bottom - 6);
        ctx.fill();
      }
    }
    // Known board spurs: small tick marks.
    ctx.strokeStyle = t.serious;
    for (const sp of store.spursKhz) {
      if (sp < f0 || sp > f1) continue;
      const x = Math.round(a.sx(sp)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, a.bottom - 10);
      ctx.lineTo(x, a.bottom);
      ctx.stroke();
    }
    const n = Math.max(64, Math.floor(a.right - a.left));
    const comp = composite(app, f0, f1, n);
    this.lastComp = comp;
    ctx.setLineDash([4, 3]);
    strokeSeries(ctx, a, comp.floor, t.muted, 1);
    ctx.setLineDash([]);
    if (app.settings.showAverage) strokeSeries(ctx, a, comp.avg, t.accent, 1.5, 0.9);
    if (app.settings.showMaxHold) strokeSeries(ctx, a, comp.max, t.warning, 1, 0.7);
    strokeSeries(ctx, a, comp.live, t.ink, 1.2, 0.9);
    ctx.fillStyle = t.muted;
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    ctx.font = "10px system-ui";
    ctx.fillText("dBFS/bin", 2, 1);
    this.hover(a, comp);
    this.renderWaterfall(a, comp);
  }

  private hover(a: Axes, comp: Composite) {
    if (this.hoverX === null || this.hoverX < a.left || this.hoverX > a.right) {
      this.tip.style.display = "none";
      return;
    }
    const t = theme();
    const ctx = (this.canvas.getContext("2d") as CanvasRenderingContext2D);
    const x = Math.round(this.hoverX) + 0.5;
    ctx.strokeStyle = t.ink2;
    ctx.globalAlpha = 0.5;
    ctx.beginPath();
    ctx.moveTo(x, a.top);
    ctx.lineTo(x, a.bottom);
    ctx.stroke();
    ctx.globalAlpha = 1;
    const n = comp.live.length;
    const i = Math.max(0, Math.min(n - 1, Math.floor(((this.hoverX - a.left) / (a.right - a.left)) * n)));
    const khz = a.ix(this.hoverX);
    const p = this.app.store.profile!;
    const chId = p.channels.findIndex((c) => Math.abs(c.khz - khz) <= p.search_hz / 1000);
    const fmt = (v: number) => (Number.isNaN(v) ? "—" : `${v.toFixed(1)} dB`);
    const row = (color: string, value: string, label: string) => {
      const r = el("div", { class: "row" }, el("i", { style: `background:${color}` }), el("b"), el("span"));
      r.children[1].textContent = value;
      r.children[2].textContent = label;
      return r;
    };
    const head = el("div", { class: "head" });
    head.textContent = `${(khz / 1000).toFixed(3)} MHz${chId >= 0 ? ` · ${this.app.channelName(chId)}` : ""}`;
    this.tip.replaceChildren(
      head,
      row(t.ink, fmt(comp.live[i]), "live"),
      row(t.warning, fmt(comp.max[i]), "max hold"),
      row(t.accent, fmt(comp.avg[i]), "average"),
      row(t.muted, fmt(comp.floor[i]), "floor / bin"),
    );
    this.tip.style.display = "block";
    const w = this.tip.offsetWidth;
    this.tip.style.left = `${this.hoverX + 12 + w > a.right ? this.hoverX - 12 - w : this.hoverX + 12}px`;
    this.tip.style.top = `${a.top + 8}px`;
  }

  private renderWaterfall(a: Axes, comp: Composite) {
    const store = this.app.store;
    const hops = store.status?.plan.length || 1;
    const range = `${comp.f0}:${comp.f1}`;
    if (range !== this.lastRowRange) {
      this.rows = [];
      this.rowTimes = [];
      this.lastRowRange = range;
    }
    if (store.waterfallVersion - this.lastRowVersion >= hops) {
      this.lastRowVersion = store.waterfallVersion;
      // Colour range follows the noise floor so empty spectrum sits near the
      // surface colour and signals rise through the ramp (floor-5 .. floor+40 dB).
      // Median over LOs, slowly smoothed, so one hop's outlier floor can't
      // shift the scale.
      const floors = [...store.floorByLo.values()].filter(Number.isFinite).sort((x, y) => x - y);
      if (floors.length) {
        const med = floors[floors.length >> 1];
        this.wfFloor = Number.isNaN(this.wfFloor) ? med : this.wfFloor + 0.1 * (med - this.wfFloor);
      }
      const floor = Number.isNaN(this.wfFloor) ? this.app.settings.dbMin : this.wfFloor;
      const lo = floor - 5;
      const span = 45;
      // Resample the composite to the fixed row width and quantise to the ramp.
      const row = new Uint8Array(this.rowWidth);
      for (let j = 0; j < this.rowWidth; j++) {
        const v = comp.live[Math.floor((j / this.rowWidth) * comp.live.length)];
        row[j] = Number.isNaN(v) ? 0 : Math.max(0, Math.min(255, Math.round(((v - lo) / span) * 255)));
      }
      this.rows.unshift(row);
      this.rowTimes.unshift(store.now());
    }
    const s = prepare(this.water);
    const wa = axes(s, a.x0, a.x1, 0, 1, { left: a.left, right: s.w - a.right, bottom: 16, top: 0 });
    const plotW = Math.max(1, Math.floor(wa.right - wa.left));
    const plotH = Math.max(1, Math.floor(wa.bottom - wa.top));
    if (this.rows.length > plotH) {
      this.rows.length = plotH;
      this.rowTimes.length = plotH;
    }
    const key = theme().seq.join();
    if (!this.lut || key !== this.lutKey) {
      this.lut = seqLut(); // the ramp differs per theme
      this.lutKey = key;
    }
    if (!this.img || this.img.width !== this.rowWidth || this.img.height !== plotH)
      this.img = new ImageData(this.rowWidth, plotH);
    const data = this.img.data;
    const bg = this.lut;
    for (let y = 0; y < plotH; y++) {
      const row = this.rows[y];
      for (let x = 0; x < this.rowWidth; x++) {
        const o = (y * this.rowWidth + x) * 4;
        const idx = row ? row[x] : 0;
        data[o] = bg[idx * 3];
        data[o + 1] = bg[idx * 3 + 1];
        data[o + 2] = bg[idx * 3 + 2];
        data[o + 3] = 255;
      }
    }
    // Scale the fixed-width image to the plot area.
    if (!this.scratch || !this.scratchCtx) {
      this.scratch = new OffscreenCanvas(this.rowWidth, plotH);
      this.scratchCtx = this.scratch.getContext("2d", { willReadFrequently: true })!;
    } else if (this.scratch.width !== this.rowWidth || this.scratch.height !== plotH) {
      this.scratch.width = this.rowWidth;
      this.scratch.height = plotH;
    }
    this.scratchCtx.putImageData(this.img, 0, 0);
    s.ctx.imageSmoothingEnabled = false;
    s.ctx.drawImage(this.scratch, wa.left, wa.top, plotW, plotH);
    // Time labels on the left.
    const t = theme();
    s.ctx.fillStyle = t.muted;
    s.ctx.font = "10px system-ui";
    s.ctx.textAlign = "right";
    s.ctx.textBaseline = "middle";
    const now = store.now();
    for (const y of [0, Math.floor(plotH / 2), plotH - 1]) {
      const tt = this.rowTimes[y];
      if (tt === undefined) continue;
      s.ctx.textBaseline = y === 0 ? "top" : y === plotH - 1 ? "bottom" : "middle";
      s.ctx.fillText(`-${(now - tt).toFixed(1)}s`, wa.left - 6, wa.top + y);
    }
    s.ctx.textBaseline = "top";
    // Channel center ticks along the bottom.
    const p = store.profile!;
    s.ctx.textAlign = "center";
    s.ctx.textBaseline = "top";
    p.channels.forEach((c, id) => {
      const x = wa.sx(c.khz);
      s.ctx.fillStyle = channelColor(id);
      s.ctx.fillRect(x - 1, wa.bottom, 2, 4);
      s.ctx.fillStyle = t.ink2;
      s.ctx.fillText(this.app.channelName(id), x, wa.bottom + 4);
    });
  }
}
