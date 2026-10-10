// RotorHazard mode page: seat levels and thresholds, a level graph, the
// detection events, and the receiver settings this mode allows.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { RH_SEATS, type RhSeat } from "../model/rh";
import { levelDb, type RhMetric } from "../proto/rh";
import { axes, channelColor, drawGrid, el, prepare, theme } from "../util/canvas";
import { ChannelPlanDialog } from "./channelPlanDialog";

const fmtLevel = (v: number | null, m: RhMetric) =>
  v === null ? "—" : `${v} (${levelDb(v, m).toFixed(1)} ${m === "RSSI" ? "dBFS" : "dB"})`;
const unit = (m: RhMetric) => (m === "RSSI" ? "dBFS" : "dB SNR");

function segButtons<T extends string>(options: [T, string][], onPick: (v: T) => void) {
  const wrap = el("span", { class: "seg" });
  const buttons = options.map(([v, label]) => {
    const b = el("button", {}, label);
    b.onclick = () => onPick(v);
    wrap.append(b);
    return [v, b] as const;
  });
  return { el: wrap, sync: (cur: string) => buttons.forEach(([v, b]) => b.classList.toggle("on", v === cur)) };
}

interface SeatTile {
  root: HTMLElement;
  name: HTMLElement;
  freq: HTMLElement;
  big: HTMLElement;
  bar: HTMLElement;
  fill: HTMLElement;
  enterMark: HTMLElement;
  exitMark: HTMLElement;
  kv: HTMLElement;
  enter: HTMLInputElement;
  exit: HTMLInputElement;
}

export class RhSeats implements View {
  el: HTMLElement;
  private grid = el("div", { class: "tiles" });
  private status = el("span", { class: "muted" });
  private tiles: SeatTile[] = [];
  private metric;

  constructor(private app: App) {
    const dialog = new ChannelPlanDialog();
    document.body.append(dialog.el);
    const edit = el("button", { title: "Choose the seats' channels (seat order follows frequency)" }, "Edit channels");
    edit.onclick = () =>
      dialog.open(app.deviceSettings.channels, (channels) => {
        app.updateDeviceSettings({ channels });
        if (app.rh.running) void app.rh.writeSeats(channels);
      });
    this.metric = segButtons<RhMetric>([["RSSI", "RSSI"], ["SNR", "SNR"]], (m) => void app.rh.setMetric(m));
    for (let j = 0; j < RH_SEATS; j++) this.tiles.push(this.buildTile(j));
    this.grid.append(...this.tiles.map((t) => t.root));
    this.el = el(
      "section",
      { class: "card span-12" },
      el("h2", {}, "RotorHazard seats", this.status, el("span", { class: "tools" }, el("span", { class: "muted" }, "Level"), this.metric.el, edit)),
      this.grid,
    );
  }

  private buildTile(j: number): SeatTile {
    const app = this.app;
    const num = () => el("input", { type: "number", min: "1", max: "254", step: "1", style: "width:58px" }) as HTMLInputElement;
    const enter = num();
    const exit = num();
    enter.onchange = () => void app.rh.setThreshold(j, "enter", Number(enter.value));
    exit.onchange = () => void app.rh.setThreshold(j, "exit", Number(exit.value));
    // "Now": the seat's average level over the last second, as RotorHazard's
    // capture buttons do (with the quad on the gate for EnterAt, away for ExitAt).
    const capture = (which: "enter" | "exit") => {
      const b = el("button", { title: `Set ${which === "enter" ? "EnterAt" : "ExitAt"} to the average level of the last second` }, "now");
      b.onclick = (e) => {
        e.stopPropagation();
        const v = app.rh.recentLevel(j);
        if (v !== null) void app.rh.setThreshold(j, which, v);
      };
      return b;
    };
    const fill = el("div");
    const enterMark = el("i", { class: "mark enter", title: "EnterAt" });
    const exitMark = el("i", { class: "mark exit", title: "ExitAt" });
    const t: SeatTile = {
      root: el("div", { class: "tile rh-seat" }),
      name: el("span"),
      freq: el("div", { class: "freq" }),
      big: el("div", { class: "big" }),
      bar: el("div", { class: "bar rh-bar" }, fill, enterMark, exitMark),
      fill,
      enterMark,
      exitMark,
      kv: el("div", { class: "kv" }),
      enter,
      exit,
    };
    for (const i of [enter, exit]) i.onclick = (e) => e.stopPropagation();
    t.root.append(
      el("div", { class: "name" }, el("span", { class: "key", style: `background:${channelColor(j)}` }), t.name),
      t.freq,
      t.big,
      t.bar,
      t.kv,
      el("div", { class: "rh-th" }, el("label", {}, "Enter"), enter, capture("enter")),
      el("div", { class: "rh-th" }, el("label", {}, "Exit"), exit, capture("exit")),
    );
    t.root.onclick = () => {
      app.settings.selected = j;
      app.changed();
    };
    return t;
  }

  render() {
    const rh = this.app.rh;
    const m = rh.metric;
    this.metric.sync(m);
    const active = rh.seats.filter((s) => s.channel).length;
    const hz = rh.seats.find((s) => s.channel && s.loopUs)?.loopUs;
    this.status.textContent = !this.app.device
      ? "not connected"
      : rh.error
        ? `error: ${rh.error}`
        : !rh.running
          ? "not polling"
          : `${active} seat${active === 1 ? "" : "s"} on · poll cycle ${rh.cycleMs.toFixed(0)} ms` +
            (hz ? ` · ${(1e6 / hz).toFixed(0)} updates/s per seat` : "");
    rh.seats.forEach((s, j) => this.renderTile(this.tiles[j], s, m));
  }

  private renderTile(t: SeatTile, s: RhSeat, m: RhMetric) {
    t.root.classList.toggle("disabled", !s.channel);
    t.root.classList.toggle("crossing", s.crossing);
    t.root.classList.toggle("selected", this.app.settings.selected === s.index);
    t.name.textContent = `Seat ${s.index + 1}${s.channel ? ` · ${s.channel}` : ""}`;
    t.freq.textContent = s.channel ? `${s.mhz} MHz` : "off";
    if (s.level === null) t.big.replaceChildren("—");
    else t.big.replaceChildren(`${s.level}`, el("small", {}, ` ${levelDb(s.level, m).toFixed(1)} ${unit(m)}`));
    // Bar from below the lowest of nadir and ExitAt to above the highest of peak and EnterAt.
    const lo = Math.max(0, Math.min(s.nodeNadir ?? 255, s.exit) - 10);
    const hi = Math.min(255, Math.max(s.nodePeak ?? 0, s.enter) + 10);
    const pos = (v: number) => `${(((v - lo) / Math.max(1, hi - lo)) * 100).toFixed(1)}%`;
    t.fill.style.width = s.level === null ? "0" : pos(Math.max(lo, s.level));
    t.fill.style.background = s.crossing ? theme().good : channelColor(s.index);
    t.enterMark.style.left = pos(s.enter);
    t.exitMark.style.left = pos(s.exit);
    const rows: [string, string][] = [
      ["laps", `${s.laps}`],
      ["last lap", s.lastLapS === null ? "—" : `${s.lastLapS.toFixed(3)} s`],
      ["pass peak", s.passPeak === null ? "—" : `${s.passPeak}`],
      ["peak / nadir", s.nodePeak === null ? "—" : `${s.nodePeak} / ${s.nodeNadir ?? "—"}`],
    ];
    t.kv.replaceChildren(...rows.flatMap(([k, v]) => [el("span", {}, k), el("span", {}, v)]));
    t.root.title = `EnterAt ${fmtLevel(s.enter, m)}, ExitAt ${fmtLevel(s.exit, m)}`;
    // Leave the inputs alone while they are being edited.
    if (document.activeElement !== t.enter) t.enter.value = String(s.enter);
    if (document.activeElement !== t.exit) t.exit.value = String(s.exit);
  }
}

/** Polled levels of the active seats, with the selected seat's thresholds and passes. */
export class RhGraph implements View {
  el: HTMLElement;
  private canvas = el("canvas");
  private span = 30;

  constructor(private app: App) {
    const span = el("select") as HTMLSelectElement;
    for (const sec of [10, 30, 60, 120]) span.append(new Option(`${sec} s`, String(sec)));
    span.value = String(this.span);
    span.onchange = () => {
      this.span = Number(span.value);
      app.store.touch();
    };
    this.el = el(
      "section",
      { class: "card span-8" },
      el("h2", {}, "Levels", el("span", { class: "muted" }, "selected seat: EnterAt / ExitAt and passes"), el("span", { class: "tools" }, span)),
      el("div", { class: "chart h-hist" }, this.canvas),
    );
  }

  render() {
    const rh = this.app.rh;
    const store = this.app.store;
    const s = prepare(this.canvas);
    const now = store.now();
    const seats = rh.seats.filter((x) => x.channel);
    const sel = rh.seats[this.app.settings.selected] ?? rh.seats[0];
    let lo = Infinity;
    let hi = -Infinity;
    for (const seat of seats)
      seat.history.forEach((t, v) => {
        if (now - t > this.span) return;
        lo = Math.min(lo, v);
        hi = Math.max(hi, v);
      });
    if (sel.channel) {
      lo = Math.min(lo, sel.exit);
      hi = Math.max(hi, sel.enter);
    }
    if (!Number.isFinite(lo)) {
      lo = 140;
      hi = 220;
    }
    lo = Math.max(0, Math.floor((lo - 5) / 10) * 10);
    hi = Math.min(260, Math.ceil((hi + 5) / 10) * 10);
    const a = axes(s, -this.span, 0, lo, hi, { top: 16 });
    drawGrid(s, a, (v) => `${v}s`, (v) => `${v}`, 6, 5);
    const ctx = s.ctx;
    const t = theme();
    if (sel.channel) {
      ctx.setLineDash([5, 4]);
      for (const [v, c] of [[sel.enter, t.good], [sel.exit, t.warning]] as const) {
        ctx.strokeStyle = c;
        ctx.beginPath();
        ctx.moveTo(a.left, a.sy(v));
        ctx.lineTo(a.right, a.sy(v));
        ctx.stroke();
      }
      ctx.setLineDash([]);
    }
    for (const seat of seats) {
      const selected = seat === sel;
      ctx.strokeStyle = channelColor(seat.index);
      ctx.lineWidth = selected ? 2.5 : 1.25;
      ctx.globalAlpha = selected ? 1 : 0.7;
      ctx.beginPath();
      let pen = false;
      seat.history.forEach((tt, v) => {
        const age = now - tt;
        if (age > this.span) return;
        if (pen) ctx.lineTo(a.sx(-age), a.sy(v));
        else ctx.moveTo(a.sx(-age), a.sy(v));
        pen = true;
      });
      ctx.stroke();
      // Peaks between polls, as the device queued them.
      ctx.fillStyle = channelColor(seat.index);
      for (const p of seat.peaks) {
        if (!p.peak || now - p.t > this.span) continue;
        ctx.beginPath();
        ctx.arc(a.sx(p.t - now), a.sy(p.v), selected ? 2.5 : 1.5, 0, 2 * Math.PI);
        ctx.fill();
      }
    }
    ctx.globalAlpha = 1;
    ctx.lineWidth = 1;
    // Passes: a marker at the top, in the seat's colour.
    for (const e of rh.events) {
      if (e.kind !== "pass" || now - e.t > this.span) continue;
      const x = a.sx(e.t - now);
      ctx.fillStyle = channelColor(e.seat);
      ctx.beginPath();
      ctx.moveTo(x - 4, a.top - 8);
      ctx.lineTo(x + 4, a.top - 8);
      ctx.lineTo(x, a.top - 1);
      ctx.fill();
      if (e.seat === sel.index) {
        ctx.strokeStyle = channelColor(e.seat);
        ctx.globalAlpha = 0.4;
        ctx.beginPath();
        ctx.moveTo(x, a.top);
        ctx.lineTo(x, a.bottom);
        ctx.stroke();
        ctx.globalAlpha = 1;
      }
    }
    ctx.fillStyle = t.muted;
    ctx.font = "10px system-ui";
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    ctx.fillText(`level (${rh.metric === "RSSI" ? "dBFS = level / 2 − 127.5" : "dB = level / 2"})`, 2, 1);
  }
}

/** Crossings and passes, newest first. */
export class RhEvents implements View {
  el: HTMLElement;
  private list = el("div", { class: "rh-events" });
  private showCrossings: HTMLInputElement;
  private shown = -1;

  constructor(private app: App) {
    const clear = el("button", { title: "Clear the events and lap counts" }, "Clear");
    clear.onclick = () => app.rh.clearEvents();
    this.showCrossings = el("input", { type: "checkbox" }) as HTMLInputElement;
    this.showCrossings.onchange = () => {
      this.shown = -1;
      app.store.touch();
    };
    this.el = el(
      "section",
      { class: "card span-4" },
      el("h2", {}, "Detection events", el("span", { class: "tools" }, el("label", {}, this.showCrossings, " enter/exit"), clear)),
      this.list,
    );
  }

  render() {
    const rh = this.app.rh;
    const key = rh.events.length * 2 + (this.showCrossings.checked ? 1 : 0);
    if (key === this.shown) return;
    this.shown = key;
    const t0 = performance.timeOrigin + this.app.store.t0;
    const rows: HTMLElement[] = [];
    for (let j = rh.events.length - 1; j >= 0 && rows.length < 200; j--) {
      const e = rh.events[j];
      if (e.kind !== "pass" && !this.showCrossings.checked) continue;
      const seat = rh.seats[e.seat];
      const time = new Date(t0 + e.t * 1000).toLocaleTimeString([], { hour12: false }) + `.${String(Math.floor((e.t * 1000) % 1000)).padStart(3, "0")}`;
      const what =
        e.kind === "pass"
          ? `pass${e.lapS !== undefined ? ` · lap ${e.lapS.toFixed(3)} s` : ""}${e.peak ? ` · peak ${e.peak}` : ""}`
          : e.kind === "enter"
            ? `enter · ${e.level ?? ""}`
            : `exit · ${e.level ?? ""}`;
      rows.push(
        el(
          "div",
          { class: `rh-event ${e.kind}` },
          el("span", { class: "muted" }, time),
          el("span", {}, el("i", { class: "key", style: `background:${channelColor(e.seat)}` }), `${e.seat + 1} ${seat.channel ?? ""}`),
          el("span", {}, what),
        ),
      );
    }
    this.list.replaceChildren(...(rows.length ? rows : [el("div", { class: "empty" }, "No passes yet")]));
  }
}

/** Receiver settings allowed in RotorHazard mode, and link statistics. */
export class RhSettings implements View {
  el: HTMLElement;
  private body = el("div", { class: "controls" });
  private stats = el("div", { class: "muted" });
  private fft = el("select") as HTMLSelectElement;
  private capn = el("select") as HTMLSelectElement;
  private gain = el("input", { type: "range", min: "0", max: "82", step: "1" }) as HTMLInputElement;
  private gainVal = el("span", { class: "val" });
  private mode;

  constructor(private app: App) {
    const rh = app.rh;
    for (const v of [128, 256, 512]) this.fft.append(new Option(`${v}-pt FFT`, String(v)));
    for (const v of [256, 512, 1024, 2048]) this.capn.append(new Option(`${v} samples`, String(v)));
    this.fft.onchange = () => void rh.command(`FFT ${this.fft.value}`);
    this.capn.onchange = () => void rh.command(`CAPN ${this.capn.value}`);
    this.gain.oninput = () => (this.gainVal.textContent = this.gain.value);
    this.gain.onchange = () => void rh.command(`GAIN MANUAL ${this.gain.value}`);
    this.mode = segButtons([["GROUP", "Grouped LOs"], ["SINGLE", "LO per channel"]], (v) => void rh.command(`MODE ${v}`));
    const ctl = (label: string, ...children: Node[]) =>
      this.body.append(el("div", { class: "ctl" }, el("label", {}, label), el("div", { class: "row" }, ...children)));
    ctl("RX gain (PHY table index)", this.gain, this.gainVal);
    ctl("FFT and capture (FFTs averaged = samples / FFT size, at most 4)", this.fft, this.capn);
    ctl("LO plan", this.mode.el);
    this.el = el("section", { class: "card span-12" }, el("h2", {}, "Receiver"), this.body, this.stats);
  }

  render() {
    const rh = this.app.rh;
    const c = rh.config as { nfft?: number; capture_samples?: number; gain?: number; gain_max?: number; mode?: string } | null;
    if (c) {
      // Show whatever the device reports, also values outside the lists.
      const pick = (sel: HTMLSelectElement, v: unknown, label: string) => {
        if (document.activeElement === sel || v === undefined) return;
        if (![...sel.options].some((o) => o.value === String(v))) sel.append(new Option(`${v} ${label}`, String(v)));
        sel.value = String(v);
      };
      pick(this.fft, c.nfft, "-pt FFT");
      pick(this.capn, c.capture_samples, "samples");
      if (document.activeElement !== this.gain) {
        this.gain.max = String(c.gain_max ?? 82);
        this.gain.value = String(c.gain);
        this.gainVal.textContent = String(c.gain);
      }
      this.mode.sync(String(c.mode));
    }
    const st = this.app.device?.rh?.stats;
    this.stats.textContent = st ? `Link: ${st.reads} reads, ${st.writes} writes, ${st.errors} failed reads` : "";
  }
}
