// Raw I/Q snapshot tool: constellation, time trace, client-side FFT.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { axes, drawGrid, el, prepare, theme } from "../util/canvas";

function fft(re: Float64Array, im: Float64Array) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    for (let i = 0; i < n; i += len)
      for (let k = 0; k < len / 2; k++) {
        const wr = Math.cos(ang * k);
        const wi = Math.sin(ang * k);
        const a = i + k;
        const b = a + len / 2;
        const tr = re[b] * wr - im[b] * wi;
        const ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
  }
}

export class RawIqView implements View {
  el: HTMLElement;
  private constel = el("canvas");
  private trace = el("canvas");
  private spec = el("canvas");
  private info = el("span", { class: "muted" });
  private shownFor: unknown = null;
  private shownTheme = "";
  private lo = el("input", { type: "number", step: "1", style: "width:110px" }) as HTMLInputElement;
  private n = el("select") as HTMLSelectElement;

  constructor(private app: App) {
    for (const v of [1024, 4096, 8192, 16380]) this.n.append(new Option(`${v} samples`, String(v)));
    this.n.value = "8192";
    const fromSel = el("button", {}, "Use selected channel's LO");
    fromSel.onclick = () => {
      const cs = app.store.channels[app.settings.selected];
      if (cs?.loKhz) this.lo.value = String(cs.loKhz);
    };
    const cap = el("button", { class: "primary" }, "Capture");
    cap.onclick = async () => {
      if (!this.lo.value) fromSel.click();
      await app.send(`RAWIQ ${Number(this.lo.value)} ${this.n.value}`);
    };
    this.el = el(
      "section",
      { class: "card span-6" },
      el("h2", {}, "Raw I/Q snapshot", el("span", { class: "tools" }, el("span", { class: "muted" }, "LO kHz"), this.lo, this.n, fromSel, cap)),
      this.info,
      el(
        "div",
        { class: "grid-2" },
        el("div", { class: "chart h-iq" }, this.constel),
        el("div", { class: "chart h-iq" }, this.trace),
      ),
      el("div", { class: "chart h-iq", style: "margin-top:8px" }, this.spec),
    );
  }

  /* The C5's smaller link queue caps RAWIQ below the capture buffer size. */
  private limitSamples() {
    const max = this.app.store.status?.rawiq_max ?? Infinity;
    let best = "";
    for (const o of Array.from(this.n.options)) {
      o.disabled = Number(o.value) > max;
      if (!o.disabled) best = o.value;
    }
    if (this.n.selectedOptions[0]?.disabled && best) this.n.value = best;
  }

  render() {
    this.limitSamples();
    const r = this.app.store.rawiq;
    const themeKey = theme().ink;
    if (themeKey !== this.shownTheme) {
      this.shownTheme = themeKey;
      this.shownFor = null; // redraw in the new theme's colours
    }
    if (!r || r === this.shownFor) {
      if (!r) {
        const s = prepare(this.constel);
        s.ctx.fillStyle = theme().muted;
        s.ctx.font = "12px system-ui";
        s.ctx.fillText("Capture a snapshot to inspect raw samples.", 10, 24);
      }
      return;
    }
    this.shownFor = r;
    const t = theme();
    let mi = 0;
    let mq = 0;
    let peak = 0;
    for (let j = 0; j < r.n; j++) {
      mi += r.i[j];
      mq += r.q[j];
      peak = Math.max(peak, Math.abs(r.i[j]), Math.abs(r.q[j]));
    }
    mi /= r.n;
    mq /= r.n;
    let pw = 0;
    for (let j = 0; j < r.n; j++) pw += (r.i[j] - mi) ** 2 + (r.q[j] - mq) ** 2;
    const rms = Math.sqrt(pw / r.n);
    const lsb = 2 ** r.shift; // 10-bit ADC counts per transferred count
    this.info.textContent = `LO ${(r.loKhz / 1000).toFixed(3)} MHz · ${r.rateHz / 1e6} MS/s · ${r.n} samples · scale ×${lsb} (10-bit counts) · DC (${(mi * lsb).toFixed(1)}, ${(mq * lsb).toFixed(1)}) · RMS ${(rms * lsb).toFixed(1)} · peak ${peak * lsb}/512${r.conjugated ? " · conjugated" : ""}`;

    // Constellation (density-ish via low alpha points).
    {
      const s = prepare(this.constel);
      const size = Math.min(s.w, s.h);
      const ox = (s.w - size) / 2;
      // Scale to the data: at normal gains noise uses only a few ADC counts.
      const range = Math.max(8, Math.ceil(peak * 1.15));
      const a = axes({ ...s, w: size, h: size }, -range, range, -range, range, { left: 0, bottom: 0, top: 0, right: 0 });
      s.ctx.translate(ox, 0);
      s.ctx.strokeStyle = t.grid;
      s.ctx.strokeRect(0.5, 0.5, size - 1, size - 1);
      s.ctx.beginPath();
      s.ctx.moveTo(size / 2, 0);
      s.ctx.lineTo(size / 2, size);
      s.ctx.moveTo(0, size / 2);
      s.ctx.lineTo(size, size / 2);
      s.ctx.stroke();
      s.ctx.fillStyle = t.accent;
      s.ctx.globalAlpha = 0.25;
      for (let j = 0; j < r.n; j++) s.ctx.fillRect(a.sx(r.i[j]) - 1, a.sy(r.q[j]) - 1, 2, 2);
      s.ctx.globalAlpha = 1;
      s.ctx.fillStyle = t.muted;
      s.ctx.font = "10px system-ui";
      s.ctx.textAlign = "left";
      s.ctx.fillText(`I/Q constellation · ±${range} transferred counts`, 4, 12);
    }
    // Time trace of the first 400 samples.
    {
      const s = prepare(this.trace);
      const n = Math.min(400, r.n);
      const us = (n / r.rateHz) * 1e6;
      const range = Math.max(8, Math.ceil(peak * 1.15));
      const a = axes(s, 0, us, -range, range, { left: 32, top: 16 });
      drawGrid(s, a, (v) => `${v.toFixed(1)}µs`, (v) => `${v}`, 4, 4);
      const line = (arr: Int8Array, color: string) => {
        s.ctx.strokeStyle = color;
        s.ctx.lineWidth = 1.2;
        s.ctx.beginPath();
        for (let j = 0; j < n; j++) {
          const x = a.sx((j / r.rateHz) * 1e6);
          const y = a.sy(arr[j]);
          if (j) s.ctx.lineTo(x, y);
          else s.ctx.moveTo(x, y);
        }
        s.ctx.stroke();
      };
      line(r.i, t.series[0]);
      line(r.q, t.series[1]);
      s.ctx.fillStyle = t.muted;
      s.ctx.font = "10px system-ui";
      s.ctx.textBaseline = "top";
      s.ctx.textAlign = "left";
      s.ctx.fillText("I (blue) · Q (orange), transferred counts", 2, 1);
    }
    // Spectrum: averaged 1024-point Hann FFTs, absolute frequency axis.
    {
      const nfft = 1024;
      const segs = Math.floor(r.n / nfft);
      const acc = new Float64Array(nfft);
      const re = new Float64Array(nfft);
      const im = new Float64Array(nfft);
      for (let sg = 0; sg < segs; sg++) {
        for (let j = 0; j < nfft; j++) {
          const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * j) / nfft);
          re[j] = (r.i[sg * nfft + j] - mi) * w;
          im[j] = (r.q[sg * nfft + j] - mq) * w;
        }
        fft(re, im);
        for (let k = 0; k < nfft; k++) acc[(k + nfft / 2) % nfft] += re[k] * re[k] + im[k] * im[k];
      }
      // dBFS: full-scale 10-bit complex tone -> 0 dB in its bin (Hann sum = nfft/2).
      const fs = 512 / 2 ** r.shift;
      const norm = 1 / (Math.max(1, segs) * ((fs * nfft) / 2) ** 2);
      const s = prepare(this.spec);
      const f0 = r.loKhz - r.rateHz / 2000;
      const f1 = r.loKhz + r.rateHz / 2000;
      const dbs = Array.from(acc, (v) => 10 * Math.log10(v * norm + 1e-20));
      const lo = Math.floor(Math.min(...dbs) / 10) * 10;
      const hi = Math.ceil(Math.max(...dbs) / 10) * 10 + 5;
      const a = axes(s, f0, f1, lo, hi, { top: 16 });
      drawGrid(s, a, (v) => (v / 1000).toFixed(1), (v) => `${v}`, 8, 4);
      s.ctx.strokeStyle = t.ink;
      s.ctx.lineWidth = 1.2;
      s.ctx.beginPath();
      dbs.forEach((v, k) => {
        const x = a.sx(f0 + ((k + 0.5) / nfft) * (f1 - f0));
        if (k) s.ctx.lineTo(x, a.sy(v));
        else s.ctx.moveTo(x, a.sy(v));
      });
      s.ctx.stroke();
      s.ctx.fillStyle = t.muted;
      s.ctx.font = "10px system-ui";
      s.ctx.textBaseline = "top";
      s.ctx.textAlign = "left";
      s.ctx.fillText(`dBFS/bin · full ${r.rateHz / 1e6} MHz capture span · MHz`, 2, 1);
    }
  }
}
