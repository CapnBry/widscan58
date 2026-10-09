// Per-channel tiles: enable toggle, RSSI bar, SNR, floor, centre offset, sparkline.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { CHF } from "../proto/frames";
import { channelColor, el, prepare, theme } from "../util/canvas";

interface Tile {
  root: HTMLDivElement;
  key: HTMLSpanElement;
  name: HTMLSpanElement;
  freq: HTMLDivElement;
  big: HTMLDivElement;
  bar: HTMLDivElement;
  kv: HTMLDivElement;
  badges: HTMLDivElement;
  spark: HTMLCanvasElement;
  toggle: HTMLInputElement;
}

export function badgesFor(flags: number, stale: boolean): [string, string][] {
  const out: [string, string][] = [];
  if (stale) out.push(["stale", "warning"]);
  if (flags & CHF.SIGNAL) out.push(["signal", "good"]);
  if (flags & CHF.SATURATED) out.push(["clipping", "critical"]);
  if (flags & CHF.SPUR) out.push(["spur masked", "serious"]);
  if (flags & CHF.EDGE) out.push(["filter edge", "warning"]);
  return out;
}

export class ChannelGrid implements View {
  el: HTMLElement;
  private tiles: Tile[] = [];
  private grid: HTMLDivElement;
  private profileName = "";
  private plan: HTMLSpanElement;

  constructor(private app: App) {
    this.grid = el("div", { class: "tiles" });
    this.plan = el("span", { class: "muted" });
    const all = el("button", {}, "All");
    const none = el("button", {}, "None");
    all.onclick = () => this.setMask((1 << (app.store.profile?.channels.length ?? 8)) - 1);
    none.onclick = () => this.setMask(0);
    this.el = el(
      "section",
      { class: "card span-12" },
      el("h2", {}, "Channels", this.plan, el("span", { class: "tools" }, all, none)),
      this.grid,
    );
  }

  private async setMask(mask: number) {
    await this.app.send(`ENABLE ${mask}`);
    await this.app.refreshStatus();
  }

  private build() {
    const p = this.app.store.profile!;
    this.grid.replaceChildren();
    this.tiles = p.channels.map((c) => {
      const key = el("span", { class: "key" });
      const name = el("span", {}, c.name);
      const toggle = el("input", { type: "checkbox", title: "Scan this channel" }) as HTMLInputElement;
      toggle.onclick = (ev) => ev.stopPropagation();
      toggle.onchange = () => {
        const st = this.app.store.status;
        if (!st) return;
        const mask = toggle.checked ? st.enabled | (1 << c.id) : st.enabled & ~(1 << c.id);
        void this.setMask(mask);
      };
      const t: Tile = {
        root: el("div", { class: "tile" }),
        key,
        name,
        freq: el("div", { class: "freq" }),
        big: el("div", { class: "big" }),
        bar: el("div", { class: "bar" }, el("div")),
        kv: el("div", { class: "kv" }),
        badges: el("div", { class: "badges" }),
        spark: el("canvas", { class: "spark" }),
        toggle,
      };
      t.root.append(
        el("div", { class: "name" }, key, name),
        el("label", { class: "switch" }, toggle),
        t.freq,
        t.big,
        t.bar,
        t.kv,
        t.badges,
        t.spark,
      );
      t.root.onclick = () => {
        this.app.settings.selected = c.id;
        this.app.changed();
      };
      this.grid.append(t.root);
      return t;
    });
    this.profileName = p.name;
  }

  render() {
    const store = this.app.store;
    const p = store.profile;
    if (!p) {
      if (!this.tiles.length) this.grid.replaceChildren(el("div", { class: "empty" }, "Connect a device or start the simulator."));
      return;
    }
    if (p.name !== this.profileName) this.build();
    const st = store.status;
    if (st) this.plan.textContent = `${st.plan.length} hop${st.plan.length === 1 ? "" : "s"} per rotation · ${st.mode.toLowerCase()} mode`;
    const now = performance.now();
    const t = theme();
    p.channels.forEach((c, id) => {
      const tile = this.tiles[id];
      const cs = store.channels[id];
      const enabled = st ? (st.enabled & (1 << id)) !== 0 : true;
      tile.toggle.checked = enabled;
      tile.root.classList.toggle("disabled", !enabled);
      tile.root.classList.toggle("selected", this.app.settings.selected === id);
      tile.key.style.background = channelColor(id);
      tile.name.textContent = this.app.channelName(id);
      tile.freq.textContent =
        `${this.app.displayMHz(c.khz).toFixed(3)} MHz` + (this.app.settings.equivAxis && p.scale !== 1 ? " (equiv)" : "");
      const m = cs?.latest;
      const stale = !!m && now - cs.updated > 1000;
      if (!m) {
        tile.big.replaceChildren("—");
        tile.kv.replaceChildren();
        tile.badges.replaceChildren();
        return;
      }
      tile.big.replaceChildren(`${m.rssiEma.toFixed(1)}`, el("small", {}, " dBFS"));
      // Bar spans floor-10 .. floor+40 dB so empty channels sit near the left.
      const frac = Math.max(0, Math.min(1, (m.rssiEma - (m.floor - 10)) / 50));
      const fill = tile.bar.firstElementChild as HTMLDivElement;
      fill.style.width = `${(frac * 100).toFixed(1)}%`;
      fill.style.background = channelColor(id);
      const sig = (m.flags & CHF.SIGNAL) !== 0;
      const rows: [string, string][] = [
        ["SNR", `${m.snr.toFixed(1)} dB`],
        ["floor", `${m.floor.toFixed(1)}`],
        ["clear", m.clearFloor === null ? "—" : m.clearFloor.toFixed(1)],
        ["centre", sig && !(m.flags & CHF.NO_CENTROID) ? `${this.app.displayOffsetKhz(m.centroidOffHz) >= 0 ? "+" : ""}${this.app.displayOffsetKhz(m.centroidOffHz).toFixed(0)} kHz` : "—"],
        ["OBW 99 %", sig && !(m.flags & CHF.NO_CENTROID) ? `${(this.app.displayOffsetKhz(m.obwHiHz - m.obwLoHz) / 1000).toFixed(2)} MHz` : "—"],
        ["rate", `${cs.rateHz.toFixed(0)} Hz`],
      ];
      tile.kv.replaceChildren(...rows.flatMap(([k, v]) => [el("span", {}, k), el("span", {}, v)]));
      tile.badges.replaceChildren(...badgesFor(m.flags, stale).map(([txt, cls]) => el("span", { class: `badge ${cls}` }, txt)));
      // Sparkline: last 10 s of smoothed RSSI against the channel's floor.
      const s = prepare(tile.spark);
      const now_s = store.now();
      const lo = m.floor - 5;
      const hi = Math.max(m.floor + 30, m.rssiEma + 3);
      s.ctx.strokeStyle = t.grid;
      s.ctx.beginPath();
      const fy = s.h - ((m.floor - lo) / (hi - lo)) * s.h;
      s.ctx.moveTo(0, fy);
      s.ctx.lineTo(s.w, fy);
      s.ctx.stroke();
      s.ctx.strokeStyle = channelColor(id);
      s.ctx.lineWidth = 1.5;
      s.ctx.beginPath();
      let first = true;
      cs.ema.forEach((tt, v) => {
        if (now_s - tt > 10) return;
        const x = s.w - ((now_s - tt) / 10) * s.w;
        const y = s.h - ((Math.max(lo, Math.min(hi, v)) - lo) / (hi - lo)) * s.h;
        if (first) s.ctx.moveTo(x, y);
        else s.ctx.lineTo(x, y);
        first = false;
      });
      s.ctx.stroke();
    });
  }
}
