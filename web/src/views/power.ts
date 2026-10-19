// Power meter mode page: the attenuator warning, and per channel the power in
// dBm and mW with the board's calibration. GPL-3.0-or-later.
import type { App, View } from "../model/app";
import { channelsCommand } from "../model/channelPlan";
import {
  ATTENUATOR_TABLE,
  INPUT_MAX_DBM,
  POWER_GAIN,
  POWER_WIN_HZ,
  SMOOTHING,
  calOffset,
  formatMw,
  clippedIn,
  lessNoise,
  meanDb,
} from "../model/power";
import { CHF } from "../proto/frames";
import { channelColor, el } from "../util/canvas";
import { ChannelPlanDialog } from "./channelPlanDialog";

const dbmText = (v: number) => v.toFixed(0).replace("-", "−");

export class PowerWarning implements View {
  el: HTMLElement;

  constructor() {
    const rows = ATTENUATOR_TABLE.map((r) =>
      el(
        "tr",
        {},
        el("td", {}, `${r.vtx} (${r.dbm} dBm)`),
        el("td", {}, el("strong", {}, `${r.attenDb} dB`)),
        el("td", {}, `${dbmText(r.dbm - r.attenDb)} dBm`),
        el("td", {}, `${r.rating}+`),
      ),
    );
    this.el = el(
      "section",
      { class: "card span-12 power-warn" },
      el("div", { class: "power-warn-title" }, "⚠ Always connect the VTX through an attenuator"),
      el(
        "p",
        {},
        "Connecting a VTX directly to the board destroys its receiver. Fit the attenuator before powering the VTX, " +
          "and never power a VTX with nothing on its output. Use at least the attenuation below for the VTX's power; " +
          "a VTX often puts out more than its label, so when in doubt use the next row.",
      ),
      el(
        "table",
        { class: "power-table" },
        el(
          "thead",
          {},
          el("tr", {}, ...["VTX power", "Attenuator", "At the board", "Rating"].map((h) => el("th", {}, h))),
        ),
        el("tbody", {}, ...rows),
      ),
      el(
        "p",
        { class: "muted" },
        `The board reads from about −32 dBm to ${dbmText(INPUT_MAX_DBM)} dBm at its input. Rating: the power the attenuator ` +
          "nearest the VTX must be rated for. Enter the attenuation you use below. " +
          "Note that many attenuators misreport their power reduction-- take the reported power " +
          "as simply a relative power reading for your measurement setup.",
      ),
    );
  }

  render() {}
}

interface Tile {
  root: HTMLDivElement;
  key: HTMLSpanElement;
  name: HTMLSpanElement;
  freq: HTMLDivElement;
  dbm: HTMLDivElement;
  mw: HTMLDivElement;
  kv: HTMLDivElement;
  badges: HTMLDivElement;
  toggle: HTMLInputElement;
}

export class PowerChannels implements View {
  el: HTMLElement;
  private grid = el("div", { class: "tiles power-tiles" });
  private note = el("div", { class: "power-note", hidden: "" });
  private tiles: Tile[] = [];
  private planKey = "";
  private smoothSync: () => void;
  private atten: HTMLInputElement;

  constructor(private app: App) {
    const all = el("button", {}, "All");
    const none = el("button", {}, "None");
    const edit = el("button", { title: "Choose which channels to measure" }, "Edit");
    all.onclick = () => void this.setMask((1 << (app.store.profile?.channels.length ?? 8)) - 1);
    none.onclick = () => void this.setMask(0);
    const dialog = new ChannelPlanDialog();
    document.body.append(dialog.el);
    edit.onclick = () => dialog.open(app.deviceSettings.channels, (channels) => void this.setChannels(channels));

    this.atten = el("input", { type: "number", min: "0", max: "90", step: "1", style: "width:64px" }) as HTMLInputElement;
    this.atten.value = String(app.settings.powerAttenDb);
    this.atten.onchange = () => {
      const v = Number(this.atten.value);
      if (!Number.isFinite(v) || v < 0) {
        this.atten.value = String(app.settings.powerAttenDb);
        return;
      }
      app.settings.powerAttenDb = v;
      app.changed();
    };
    const seg = el("span", { class: "seg" });
    const buttons = SMOOTHING.map(([s, label]) => {
      const b = el("button", {}, label);
      b.onclick = () => {
        app.settings.powerSmoothS = s;
        app.changed();
        this.smoothSync();
      };
      seg.append(b);
      return [s, b] as const;
    });
    this.smoothSync = () => buttons.forEach(([s, b]) => b.classList.toggle("on", s === app.settings.powerSmoothS));
    this.smoothSync();

    this.el = el(
      "section",
      { class: "card span-12" },
      el("h2", {}, "Power", el("span", { class: "tools" }, edit, all, none)),
      el(
        "div",
        { class: "power-settings" },
        el("label", {}, "Attenuator ", this.atten, " dB"),
        el("span", { class: "power-smooth" }, el("span", { class: "muted" }, "Smoothing "), seg),
      ),
      this.note,
      this.grid,
      el("div", { class: "power-dev" }, "Power values are approximate from a calibration with a single C5."),
    );
  }

  private async setChannels(channels: string[]) {
    this.app.updateDeviceSettings({ channels });
    if (!this.app.device || this.app.readOnly) return;
    await this.app.send(channelsCommand(channels));
    await this.app.send("PROFILE?");
    await this.app.refreshStatus();
  }

  private async setMask(mask: number) {
    await this.app.send(`ENABLE ${mask}`);
    await this.app.refreshStatus();
  }

  private build() {
    const p = this.app.store.profile!;
    this.tiles = p.channels.map((c) => {
      const toggle = el("input", { type: "checkbox", title: "Measure this channel" }) as HTMLInputElement;
      toggle.onchange = () => {
        const st = this.app.store.status;
        if (!st) return;
        void this.setMask(toggle.checked ? st.enabled | (1 << c.id) : st.enabled & ~(1 << c.id));
      };
      const t: Tile = {
        root: el("div", { class: "tile power-tile" }),
        key: el("span", { class: "key" }),
        name: el("span", {}),
        freq: el("div", { class: "freq" }),
        dbm: el("div", { class: "power-dbm" }),
        mw: el("div", { class: "power-mw" }),
        kv: el("div", { class: "kv" }),
        badges: el("div", { class: "badges" }),
        toggle,
      };
      t.root.append(el("div", { class: "name" }, t.key, t.name), el("label", { class: "switch" }, toggle), t.freq, t.dbm, t.mw, t.kv, t.badges);
      return t;
    });
    this.grid.replaceChildren(...this.tiles.map((t) => t.root));
    this.planKey = this.app.store.planKey;
  }

  private setNote(text: string) {
    if (this.note.textContent !== text) this.note.textContent = text;
    this.note.hidden = !text;
  }

  render() {
    const store = this.app.store;
    const p = store.profile;
    if (!p) {
      this.setNote("");
      if (!this.tiles.length) this.grid.replaceChildren(el("div", { class: "empty" }, "Connect a device or start the simulator."));
      return;
    }
    if (store.planKey !== this.planKey) this.build();
    if (document.activeElement !== this.atten) this.atten.value = String(this.app.settings.powerAttenDb);
    const st = store.status;
    // Readings only mean anything with the scan the calibration was made with.
    const ready = !!st && st.gain_mode === "MANUAL" && st.gain === POWER_GAIN && st.rssi_hz === POWER_WIN_HZ && st.mode === "SINGLE";
    const calibrated = calOffset(p.name, 5800) !== null;
    this.setNote(
      !calibrated
        ? `This board (${p.name}) has no power calibration: power meter mode needs an ESP32-C5 board.`
        : st && !ready
          ? "The device is not set up for power measurement (gain, window or LO mode changed): switch modes in the menu to set it up again."
          : "",
    );
    const atten = this.app.settings.powerAttenDb;
    const now = performance.now();
    const nowS = store.now();
    p.channels.forEach((c, id) => {
      const tile = this.tiles[id];
      const enabled = st ? (st.enabled & (1 << id)) !== 0 : true;
      tile.toggle.checked = enabled;
      tile.root.classList.toggle("disabled", !enabled);
      tile.key.style.background = channelColor(id);
      tile.name.textContent = this.app.channelName(id);
      const mhz = Math.round(c.khz / 1000);
      tile.freq.textContent = `${mhz} MHz`;
      const cs = store.channels[id];
      const m = cs?.latest;
      const off = calOffset(p.name, mhz);
      const blank = () => {
        tile.dbm.replaceChildren("—");
        tile.mw.textContent = "";
      };
      if (!m || !enabled || !off || !ready) {
        blank();
        tile.kv.replaceChildren();
        tile.badges.replaceChildren();
        return;
      }
      // Averaging leaves out clipped captures: a powered VTX's setup can clip
      // single captures far above its level (PLAN.md, 2026-10-19). Off, the
      // latest capture shows as it is.
      const smoothS = this.app.settings.powerSmoothS;
      const clip = smoothS > 0 ? clippedIn(cs.clipped, nowS, smoothS) : null;
      const over = clip ? clip.share > 0.5 : (m.flags & CHF.SATURATED) !== 0;
      const rssi = meanDb(cs.rssi, nowS, smoothS, clip?.times)!;
      const noise = meanDb(cs.floor, nowS, smoothS, clip?.times);
      const sig = lessNoise(rssi, noise);
      const badges: [string, string][] = [];
      if (now - cs.updated > 1000) badges.push(["stale", "warning"]);
      let input: number | null = null;
      if (over) {
        // The ADC clips (most of the window when averaging): the level means nothing.
        tile.dbm.replaceChildren("OVER");
        tile.mw.textContent = "";
        badges.push(["clipping", "critical"]);
      } else if (sig === null) {
        blank();
        badges.push(["no signal", "warning"]);
      } else {
        input = sig + off.db;
        const vtx = input + atten;
        tile.dbm.replaceChildren(vtx.toFixed(1).replace("-", "−"), el("small", {}, " dBm"));
        tile.mw.textContent = formatMw(vtx);
        if (input > INPUT_MAX_DBM) badges.push(["over range", "critical"]);
      }
      if (!off.calibrated) badges.push(["uncalibrated", "serious"]);
      const kv: [string, string][] = [
        ["at the board", input === null ? "—" : `${input.toFixed(1).replace("-", "−")} dBm`],
        ["level", `${rssi.toFixed(1)} dBFS`],
        ["noise", noise === null ? "—" : `${noise.toFixed(1)} dBFS`],
      ];
      tile.kv.replaceChildren(...kv.flatMap(([k, v]) => [el("span", {}, k), el("span", {}, v)]));
      tile.badges.replaceChildren(...badges.map(([txt, cls]) => el("span", { class: `badge ${cls}` }, txt)));
    });
  }
}
