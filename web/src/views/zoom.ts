// Selected-channel zoom: spectrum around the channel with metric markers.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { CHF } from "../proto/frames";
import { axes, channelColor, drawGrid, el, prepare, theme } from "../util/canvas";
import { composite } from "./panorama";

export class Zoom implements View {
  el: HTMLElement;
  private canvas = el("canvas");
  private title = el("span");
  private readout = el("div", { class: "readout" });

  constructor(private app: App) {
    const t = theme();
    this.el = el(
      "section",
      { class: "card span-6" },
      el("h2", {}, "Channel zoom", this.title),
      el("div", { class: "chart h-zoom" }, this.canvas),
      el(
        "div",
        { class: "legend" },
        el("span", {}, el("i", { style: `background:${t.ink}` }), "Live"),
        el("span", {}, el("i", { style: `background:${t.warning}` }), "Max hold"),
        el("span", {}, el("i", { style: `background:${t.accent}` }), "Average"),
        el("span", {}, el("i", { style: `background:${t.good}` }), "Most occupied · 99 % band"),
        el("span", {}, el("i", { style: `background:${t.critical}` }), "Center (video center)"),
        el("span", {}, el("i", { style: `background:${t.ink2}` }), "Nominal"),
      ),
      this.readout,
    );
  }

  render() {
    const app = this.app;
    const p = app.store.profile;
    const s = prepare(this.canvas);
    if (!p || !app.store.segments.size) return;
    const id = Math.min(app.settings.selected, p.channels.length - 1);
    const ch = p.channels[id];
    const st = app.store.status;
    const search = (st?.search_hz ?? p.search_hz) / 1000;
    const rssi = (st?.rssi_hz ?? p.rssi_hz) / 1000;
    const half = search * 1.6;
    const f0 = ch.khz - half;
    const f1 = ch.khz + half;
    this.title.textContent = `${app.channelName(id)} · ${(ch.khz / 1000).toFixed(3)} MHz`;
    const a = axes(s, f0, f1, app.settings.dbMin, app.settings.dbMax, { top: 16 });
    const ctx = s.ctx;
    const t = theme();
    const col = channelColor(id);
    ctx.fillStyle = col;
    ctx.globalAlpha = 0.14;
    ctx.fillRect(a.sx(ch.khz - rssi), a.top, a.sx(ch.khz + rssi) - a.sx(ch.khz - rssi), a.bottom - a.top);
    ctx.globalAlpha = 1;
    drawGrid(
      s,
      a,
      (v) => {
        const off = (v - ch.khz) / 1000;
        return `${off >= 0 ? "+" : ""}${off.toFixed(1)}`;
      },
      (v) => `${v}`,
      8,
      5,
    );
    const n = Math.max(64, Math.floor(a.right - a.left));
    const comp = composite(app, f0, f1, n);
    const line = (data: Float32Array, color: string, w: number, alpha = 1) => {
      ctx.strokeStyle = color;
      ctx.lineWidth = w;
      ctx.globalAlpha = alpha;
      ctx.beginPath();
      let pen = false;
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
    };
    if (app.settings.showAverage) line(comp.avg, t.accent, 1.5, 0.9);
    if (app.settings.showMaxHold) line(comp.max, t.warning, 1, 0.7);
    line(comp.live, t.ink, 1.5);
    const m = app.store.channels[id]?.latest;
    const vline = (khz: number, color: string, dash: number[] = []) => {
      const x = Math.round(a.sx(khz)) + 0.5;
      ctx.strokeStyle = color;
      ctx.setLineDash(dash);
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(x, a.top);
      ctx.lineTo(x, a.bottom);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.lineWidth = 1;
    };
    vline(ch.khz, t.ink2, [2, 3]);
    ctx.fillStyle = t.muted;
    ctx.font = "10px system-ui";
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    ctx.fillText("dBFS/bin · x: offset from nominal, MHz", 2, 1);
    if (!m) return;
    const floorBin = app.store.floorByLo.get(app.store.channels[id].loKhz);
    if (floorBin !== undefined) {
      ctx.strokeStyle = t.muted;
      ctx.setLineDash([4, 3]);
      ctx.beginPath();
      ctx.moveTo(a.left, a.sy(floorBin));
      ctx.lineTo(a.right, a.sy(floorBin));
      ctx.stroke();
      ctx.setLineDash([]);
    }
    const sig = (m.flags & CHF.SIGNAL) !== 0;
    // Occupancy is absent in METRICS RSSI|SNR.
    if (sig && m.peakOffHz !== null && m.centroidOffHz !== null && m.peak !== null && m.obwLoHz !== null && m.obwHiHz !== null) {
      vline(ch.khz + m.peakOffHz / 1000, t.good);
      if (!(m.flags & CHF.NO_CENTROID)) vline(ch.khz + m.centroidOffHz / 1000, t.critical);
      // 99 % occupied band (time-averaged) as a bracket 10 dB under this hop's peak.
      const y = a.sy(Math.min(a.y1, m.peak - 10));
      const xa = a.sx(ch.khz + m.obwLoHz / 1000);
      const xb = a.sx(ch.khz + m.obwHiHz / 1000);
      ctx.strokeStyle = t.good;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(xa, y - 4);
      ctx.lineTo(xa, y);
      ctx.lineTo(xb, y);
      ctx.lineTo(xb, y - 4);
      ctx.stroke();
      ctx.lineWidth = 1;
    }
    const off = (hz: number) => {
      const k = hz / 1000;
      return `${k >= 0 ? "+" : ""}${k.toFixed(0)} kHz`;
    };
    // Blank: not measured in this metrics mode; "—": measured, nothing there.
    const occ = sig && !(m.flags & CHF.NO_CENTROID);
    const items: [string, string][] = [
      ["RSSI", `${m.rssi.toFixed(1)} dBFS` + (m.rssiEma === null ? "" : ` (EMA ${m.rssiEma.toFixed(1)})`)],
      ["SNR", m.snr === null ? "" : `${m.snr.toFixed(1)} dB`],
      ["floor", m.floor === null ? "" : `${m.floor.toFixed(1)} dBFS`],
      ["center", m.centroidOffHz === null ? "" : occ ? off(m.centroidOffHz) : "—"],
      ["peak", m.peak === null ? "" : sig ? `${m.peak.toFixed(1)} dBFS/bin` : "—"],
      ["most occupied", m.peakOffHz === null ? "" : occ ? off(m.peakOffHz) : "—"],
      ["OBW 99 %", m.obwLoHz === null || m.obwHiHz === null ? "" : occ ? `${((m.obwHiHz - m.obwLoHz) / 1e6).toFixed(2)} MHz (${off(m.obwLoHz)} … ${off(m.obwHiHz)})` : "—"],
    ];
    this.readout.replaceChildren(
      ...items.map(([k, v]) => {
        const sEl = el("span", { title: `${k} ${v}` });
        const b = el("b");
        b.textContent = v;
        sEl.append(`${k} `, b);
        return sEl;
      }),
    );
  }
}
