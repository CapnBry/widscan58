// Scanner configuration. Every control maps to one firmware command and
// re-reads STATUS? afterwards, so the UI always shows the device's state.
// GPL-3.0-or-later.
import type { App, View } from "../model/app";
import type { Status } from "../proto/frames";
import { el } from "../util/canvas";

type Sync = (st: Status) => void;

export class Controls implements View {
  el: HTMLElement;
  private syncs: Sync[] = [];
  private shownStatus: Status | null = null;
  private lastDisabled: boolean | null = null;
  private body = el("div", { class: "controls" });

  constructor(private app: App) {
    this.el = el("section", { class: "card span-6" }, el("h2", {}, "Scanner controls"), this.body);
    this.build();
  }

  private async apply(cmd: string) {
    await this.app.send(cmd);
    await this.app.refreshStatus();
  }

  private ctl(label: string, ...children: Node[]): HTMLElement {
    const c = el("div", { class: "ctl" }, el("label", {}, label), el("div", { class: "row" }, ...children));
    this.body.append(c);
    return c;
  }

  private seg(options: [string, string][], onPick: (v: string) => void, current: (st: Status) => string) {
    const wrap = el("span", { class: "seg" });
    const buttons = options.map(([v, label]) => {
      const b = el("button", {}, label);
      b.onclick = () => onPick(v);
      wrap.append(b);
      return [v, b] as const;
    });
    this.syncs.push((st) => {
      const cur = current(st);
      for (const [v, b] of buttons) b.classList.toggle("on", v === cur);
    });
    return wrap;
  }

  private num(min: number, max: number, step: number, width = 80): HTMLInputElement {
    return el("input", { type: "number", min: String(min), max: String(max), step: String(step), style: `width:${width}px` }) as HTMLInputElement;
  }

  private build() {
    const app = this.app;
    // Hop strategy
    this.ctl(
      "LO plan",
      this.seg(
        [["GROUP", "Group LOs"], ["SINGLE", "One LO per channel"]],
        (v) => {
          app.updateDeviceSettings({ mode: v as "GROUP" | "SINGLE" });
          void this.apply(`MODE ${v}`);
        },
        (st) => st.mode,
      ),
    );
    // Gain
    const gain = el("input", { type: "range", min: "0", max: "82", step: "1" }) as HTMLInputElement;
    const gainVal = el("span", { class: "val" });
    gain.oninput = () => (gainVal.textContent = gain.value);
    gain.onchange = () => {
      app.updateDeviceSettings({ gainMode: "MANUAL", gain: Number(gain.value) });
      void this.apply(`GAIN MANUAL ${gain.value}`);
    };
    this.ctl(
      "RX gain (PHY table index, not dB)",
      this.seg(
        [["MANUAL", "Manual"], ["HARDWARE", "AGC"]],
        (v) => {
          app.updateDeviceSettings({ gainMode: v as "MANUAL" | "HARDWARE", gain: Number(gain.value) });
          void this.apply(v === "MANUAL" ? `GAIN MANUAL ${gain.value}` : "GAIN HARDWARE");
        },
        (st) => st.gain_mode,
      ),
      gain,
      gainVal,
    );
    this.syncs.push((st) => {
      gain.max = String(st.gain_max || 82);
      gain.value = String(st.gain);
      gainVal.textContent = String(st.gain);
      gain.disabled = st.gain_mode !== "MANUAL";
    });
    // Analog filter
    const filter = this.num(-1, 63, 1, 64);
    filter.onchange = () => void this.apply(Number(filter.value) < 0 ? "FILTER AUTO" : `FILTER ${filter.value}`);
    this.ctl("Analog filter code (-1 = PHY auto; 60 ≈ 13 MHz, 0 ≈ 69 MHz)", filter);
    this.syncs.push((st) => (filter.value = String(st.filter)));
    // FFT size; the device captures 4 FFTs' worth of samples and averages them (Welch), as in RotorHazard mode.
    const fftSel = el("select") as HTMLSelectElement;
    const fftOption = (v: number) => new Option(`${v}-pt FFT (${4 * v} samples)`, String(v));
    for (const v of [64, 128, 256, 512, 1024]) fftSel.append(fftOption(v));
    fftSel.onchange = () => void this.apply(`FFT ${fftSel.value}`);
    this.ctl("FFT size (finer bins with larger FFTs, more updates per second with smaller ones)", fftSel);
    this.syncs.push((st) => {
      if (![...fftSel.options].some((o) => o.value === String(st.nfft))) fftSel.append(fftOption(st.nfft));
      fftSel.value = String(st.nfft);
    });
    // Per-channel measurements
    this.ctl(
      "Metrics (RSSI / SNR: only what lap timing needs, faster; the rest is left blank)",
      this.seg([["FULL", "Full"], ["RSSI", "RSSI"], ["SNR", "SNR"]], (v) => void this.apply(`METRICS ${v}`), (st) => st.metrics ?? "FULL"),
    );
    // Windows
    const rssi = this.num(10, 20000, 10);
    const search = this.num(10, 20000, 10);
    const win = () => void this.apply(`WIN ${Math.round(Number(rssi.value) * 1000)} ${Math.round(Number(search.value) * 1000)}`);
    rssi.onchange = win;
    search.onchange = win;
    this.ctl("RSSI ± kHz · search ± kHz (device units)", rssi, search);
    this.syncs.push((st) => {
      rssi.value = String(st.rssi_hz / 1000);
      search.value = String(st.search_hz / 1000);
    });
    // Smoothing: time constants, the same at any update rate
    const ema = this.num(1, 60000, 10, 70);
    const clear = this.num(1, 60000, 100, 70);
    const sm = () => void this.apply(`EMA ${Math.round(Number(ema.value))} ${Math.round(Number(clear.value))}`);
    ema.onchange = sm;
    clear.onchange = sm;
    this.ctl("Smoothing RSSI · channel floor (ms)", ema, clear);
    this.syncs.push((st) => {
      ema.value = String(st.ema_ms);
      clear.value = String(st.clear_ms);
    });
    // Display spectrum
    const every = this.num(0, 255, 1, 56);
    const bins = el("select") as HTMLSelectElement;
    for (const v of [64, 128, 256, 512, 1024]) bins.append(new Option(`${v} bins`, String(v)));
    const det = el("select") as HTMLSelectElement;
    det.append(new Option("max", "MAX"), new Option("mean", "MEAN"));
    // The span isn't offered here: SPEC keeps the device's current one.
    const spec = () => {
      const span = this.app.store.status?.spec_span_hz;
      if (span !== undefined) void this.apply(`SPEC ${every.value} ${bins.value} ${span} ${det.value}`);
    };
    for (const c of [every, bins, det]) c.onchange = spec;
    this.ctl("Display spectrum: every N hops (0 off) · bins · detector", every, bins, det);
    this.syncs.push((st) => {
      every.value = String(st.spec_every);
      bins.value = String(st.spec_bins);
      det.value = st.spec_detector ? "MAX" : "MEAN";
    });
    // Calibration / tuning
    const settle = this.num(0, 20000, 50, 70);
    settle.onchange = () => void this.apply(`SETTLE ${settle.value}`);
    const recal = el("button", {}, "Recalibrate now");
    recal.onclick = () => void this.apply("RECAL");
    this.ctl(
      "RX calibration · settle µs after tune",
      this.seg([["CACHE", "Cache"], ["ONCE", "Once"], ["EVERY", "Every LO"]], (v) => void this.apply(`RECALMODE ${v}`), (st) => st.recal),
      recal,
      settle,
    );
    this.syncs.push((st) => (settle.value = String(st.settle_us)));
    // Tune skip: TUNESKIP bits, each a toggle (S3 default 7, C5 3)
    const tskip = el("span", { class: "seg" });
    const tskipBits = (
      [
        [1, "PLL only"],
        [2, "Skip RX path"],
        [4, "Skip gain"],
      ] as const
    ).map(([bit, label]) => {
      const b = el("button", {}, label);
      b.onclick = () => {
        const mask = this.app.store.status?.tskip;
        if (mask !== undefined) void this.apply(`TUNESKIP ${mask ^ bit}`);
      };
      tskip.append(b);
      return [bit, b] as const;
    });
    this.ctl("Tune skip between full tunes (debug; skipping gain is wrong on the C5)", tskip);
    this.syncs.push((st) => {
      for (const [bit, b] of tskipBits) {
        b.classList.toggle("on", st.tskip !== undefined && (st.tskip & bit) !== 0);
        b.disabled = st.tskip === undefined;
      }
    });
    this.ctl(
      "IQ correction",
      this.seg([["1", "On"], ["0", "Off"]], (v) => void this.apply(`IQCORR ${v}`), (st) => String(st.iq_corr)),
    );
    // Display range (UI only)
    const dmin = this.num(-160, 0, 5, 64);
    const dmax = this.num(-160, 0, 5, 64);
    const dr = () => {
      app.settings.dbMin = Number(dmin.value);
      app.settings.dbMax = Number(dmax.value);
      app.changed();
    };
    dmin.onchange = dr;
    dmax.onchange = dr;
    dmin.value = String(app.settings.dbMin);
    dmax.value = String(app.settings.dbMax);
    app.onChange(() => {
      dmin.value = String(app.settings.dbMin);
      dmax.value = String(app.settings.dbMax);
    });
    this.ctl("Display dB range (UI only)", dmin, dmax);
    // System
    const reboot = el("button", {}, "Reboot");
    reboot.onclick = () => {
      if (confirm("Reboot the device?")) void app.send("REBOOT");
    };
    const boot = el("button", { class: "danger" }, "Reboot to bootloader");
    boot.onclick = () => {
      if (confirm("Reboot into the ROM download mode? The scanner stops until the device is reflashed or reset."))
        void app.send("BOOTLOADER");
    };
    this.ctl("Device", reboot, boot);
  }

  render() {
    const st = this.app.store.status;
    const disabled = !this.app.device || this.app.readOnly;
    if (disabled !== this.lastDisabled) {
      this.lastDisabled = disabled;
      this.shownStatus = null; // re-sync per-control states after enabling
      this.body.querySelectorAll("button, input, select").forEach((e) => {
        const ctlLabel = e.closest(".ctl")?.querySelector("label")?.textContent ?? "";
        if (!ctlLabel.startsWith("Display dB")) (e as HTMLInputElement).disabled = disabled;
      });
    }
    if (!st || st === this.shownStatus || disabled) return;
    this.shownStatus = st;
    for (const s of this.syncs) s(st);
  }
}
