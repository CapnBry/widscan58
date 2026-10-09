// Telemetry, hop timing, protocol log, command console, frame inspector.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { axes, drawGrid, el, prepare, theme } from "../util/canvas";

function stat(label: string): { root: HTMLElement; value: HTMLElement; sub: HTMLElement } {
  const value = el("div", { class: "value" });
  const sub = el("div", { class: "sub" });
  return { root: el("div", { class: "stat" }, el("div", { class: "label" }, label), value, sub), value, sub };
}

export class Telemetry implements View {
  el: HTMLElement;
  private stats: Record<string, ReturnType<typeof stat>> = {};
  private timing = el("canvas");
  private tip = el("div", { class: "tooltip" });
  private hoverX: number | null = null;
  private lastBytes = 0;
  private lastT = performance.now();
  private bps = 0;
  private dropHist: { t: number; drops: number }[] = [];

  constructor(private app: App) {
    const grid = el("div", { class: "stats" });
    for (const [k, label] of [
      ["rot", "Rotation rate"],
      ["hop", "Hop time"],
      ["tune", "Tune"],
      ["cap", "Capture"],
      ["dsp", "DSP"],
      ["link", "Device TX queue"],
      ["usb", "Link throughput"],
      ["err", "Errors"],
      ["heap", "Free heap"],
      ["radio", "Radio"],
    ]) {
      this.stats[k] = stat(label);
      grid.append(this.stats[k].root);
    }
    const t = theme();
    this.el = el(
      "section",
      { class: "card span-6" },
      el("h2", {}, "Scanner telemetry"),
      grid,
      el("h2", { style: "margin-top:10px" }, "Per-hop time breakdown", el("span", { class: "tools muted" }, "last 300 hops")),
      el("div", { class: "chart h-timing" }, this.timing, this.tip),
      el(
        "div",
        { class: "legend" },
        el("span", {}, el("i", { class: "box", style: `background:${t.series[0]}` }), "Tune"),
        el("span", {}, el("i", { class: "box", style: `background:${t.series[1]}` }), "Capture"),
        el("span", {}, el("i", { class: "box", style: `background:${t.series[2]}` }), "DSP + output"),
      ),
    );
    this.timing.addEventListener("pointermove", (e) => (this.hoverX = e.offsetX));
    this.timing.addEventListener("pointerleave", () => {
      this.hoverX = null;
      this.tip.style.display = "none";
    });
  }

  private set(k: string, value: string, unit: string, sub = "") {
    const s = this.stats[k];
    s.value.replaceChildren(value, el("small", {}, unit ? ` ${unit}` : ""));
    s.sub.textContent = sub;
  }

  render() {
    const store = this.app.store;
    const tel = store.telemetry;
    const dev = this.app.device;
    const now = performance.now();
    if (dev && now - this.lastT > 1000) {
      const b = dev.stats().bytes;
      this.bps = ((b - this.lastBytes) * 1000) / (now - this.lastT);
      this.lastBytes = b;
      this.lastT = now;
    }
    if (tel) {
      const hops = store.status?.plan.length ?? 0;
      this.set("rot", tel.rotationHz.toFixed(1), "/s", `${hops} hops · ${(tel.rotationHz * hops).toFixed(0)} hops/s`);
      this.set("hop", (tel.hopUsAvg / 1000).toFixed(2), "ms", `max ${(tel.hopUsMax / 1000).toFixed(2)} ms`);
      this.set("tune", (tel.tuneUsAvg / 1000).toFixed(2), "ms", `max ${(tel.tuneUsMax / 1000).toFixed(2)} ms`);
      this.set("cap", (tel.captureUsAvg / 1000).toFixed(2), "ms", `${store.status?.capture_samples ?? "?"} samples`);
      this.set("dsp", (tel.dspUsAvg / 1000).toFixed(2), "ms", `max ${(tel.dspUsMax / 1000).toFixed(2)} ms`);
      this.dropHist.push({ t: now, drops: tel.drops });
      while (this.dropHist.length > 1 && now - this.dropHist[0].t > 10000) this.dropHist.shift();
      const recent = tel.drops - this.dropHist[0].drops;
      this.set("link", (tel.linkFill * 100).toFixed(0), "%", `${recent} dropped in last 10 s · ${tel.drops} since boot`);
      this.set("heap", (tel.heapFree / 1024).toFixed(0), "KiB", "internal RAM");
      this.set(
        "radio",
        tel.gain === 255 ? "AGC" : `G${tel.gain}`,
        "",
        `filter ${tel.filter < 0 ? "auto" : tel.filter} · recal ${["every LO", "once", "per-LO cache"][tel.recalMode] ?? tel.recalMode}`,
      );
    }
    if (dev) {
      const st = dev.stats();
      this.set("usb", (this.bps / 1024).toFixed(1), "KiB/s", `${(st.bytes / 1048576).toFixed(1)} MiB total`);
      this.set("err", String(st.crcErrors), "CRC", `${tel?.captureErrors ?? 0} capture · ${st.resyncBytes} resync B`);
    }
    this.renderTiming();
  }

  private renderTiming() {
    const s = prepare(this.timing);
    const hops = this.app.store.hopTimes.slice(-300);
    if (!hops.length) return;
    let hi = 0;
    for (const h of hops) hi = Math.max(hi, (h.tune + h.capture + h.dsp) / 1000);
    hi = Math.ceil(hi + 0.5);
    const a = axes(s, 0, hops.length, 0, hi, { left: 36 });
    drawGrid(s, a, () => "", (v) => `${v}ms`, 1, 4);
    const t = theme();
    const ctx = s.ctx;
    const n = hops.length;
    const layers: [keyof (typeof hops)[0], string][] = [
      ["tune", t.series[0]],
      ["capture", t.series[1]],
      ["dsp", t.series[2]],
    ];
    const base = new Float32Array(n);
    for (const [k, color] of layers) {
      ctx.fillStyle = color;
      ctx.beginPath();
      for (let j = 0; j < n; j++) ctx.lineTo(a.sx(j + 0.5), a.sy((base[j] + hops[j][k]) / 1000));
      for (let j = n - 1; j >= 0; j--) ctx.lineTo(a.sx(j + 0.5), a.sy(base[j] / 1000));
      ctx.closePath();
      ctx.fill();
      for (let j = 0; j < n; j++) base[j] += hops[j][k];
    }
    if (this.hoverX === null || this.hoverX < a.left || this.hoverX > a.right) {
      this.tip.style.display = "none";
      return;
    }
    const j = Math.max(0, Math.min(n - 1, Math.floor(a.ix(this.hoverX))));
    const x = Math.round(a.sx(j + 0.5)) + 0.5;
    ctx.strokeStyle = t.ink;
    ctx.beginPath();
    ctx.moveTo(x, a.top);
    ctx.lineTo(x, a.bottom);
    ctx.stroke();
    const h = hops[j];
    const row = (color: string, v: number, label: string) => {
      const r = el("div", { class: "row" }, el("i", { style: `background:${color}` }), el("b"), el("span"));
      r.children[1].textContent = `${(v / 1000).toFixed(2)} ms`;
      r.children[2].textContent = label;
      return r;
    };
    const head = el("div", { class: "head" });
    head.textContent = `hop −${n - 1 - j}`;
    this.tip.replaceChildren(head, row(t.series[0], h.tune, "tune"), row(t.series[1], h.capture, "capture"), row(t.series[2], h.dsp, "DSP"), row(t.ink, h.tune + h.capture + h.dsp, "total"));
    this.tip.style.display = "block";
    const w = this.tip.offsetWidth;
    this.tip.style.left = `${this.hoverX + 12 + w > a.right ? this.hoverX - 12 - w : this.hoverX + 12}px`;
    this.tip.style.top = `${a.top}px`;
  }
}

export class Console implements View {
  el: HTMLElement;
  private log = el("div", { class: "log" });
  private inspector = el("div", { class: "inspector" });
  private shown = 0;
  private lastInspect = 0;
  private history: string[] = [];
  private hpos = 0;

  constructor(private app: App) {
    const input = el("input", { type: "text", placeholder: "Command, e.g. TIMING?  GAIN MANUAL 50  SPURS?" }) as HTMLInputElement;
    const send = el("button", { class: "primary" }, "Send");
    const go = async () => {
      const line = input.value.trim();
      if (!line) return;
      this.history.push(line);
      this.hpos = this.history.length;
      input.value = "";
      await app.send(line);
      if (!line.endsWith("?")) await app.refreshStatus();
    };
    send.onclick = () => void go();
    input.onkeydown = (e) => {
      if (e.key === "Enter") void go();
      else if (e.key === "ArrowUp" && this.hpos > 0) input.value = this.history[--this.hpos];
      else if (e.key === "ArrowDown" && this.hpos < this.history.length) input.value = this.history[++this.hpos] ?? "";
    };
    const quick = el("span", { class: "tools" });
    for (const cmd of ["INFO", "STATUS?", "TIMING?", "SPURS?", "GAIN?"]) {
      const b = el("button", {}, cmd);
      b.onclick = () => void app.send(cmd);
      quick.append(b);
    }
    this.el = el(
      "section",
      { class: "card span-6" },
      el("h2", {}, "Protocol console", quick),
      this.log,
      el("div", { class: "console-row" }, input, send),
      el("h2", { style: "margin-top:10px" }, "Last METRICS frame"),
      this.inspector,
    );
  }

  render() {
    const store = this.app.store;
    if (store.log.length !== this.shown || store.log.length === 500) {
      const atBottom = this.log.scrollTop + this.log.clientHeight >= this.log.scrollHeight - 4;
      this.log.replaceChildren(
        ...store.log.slice(-200).map((l) => {
          const d = el("div", { class: l.dir });
          d.textContent = `${l.t.toFixed(2).padStart(8)} ${l.dir === "tx" ? ">" : "<"} ${l.text}`;
          return d;
        }),
      );
      this.shown = store.log.length;
      if (atBottom) this.log.scrollTop = this.log.scrollHeight;
    }
    const now = performance.now();
    if (store.lastMetrics && now - this.lastInspect > 500) {
      this.lastInspect = now;
      const m = store.lastMetrics;
      const named = {
        ...m,
        channels: m.channels.map((c) => ({ name: this.app.channelName(c.id), ...c })),
      };
      this.inspector.textContent = JSON.stringify(named, null, 1);
    }
  }
}
