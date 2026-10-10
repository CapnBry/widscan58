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
    // Scan + hop strategy
    this.ctl(
      "Scan",
      this.seg([["1", "Running"], ["0", "Stopped"]], (v) => void this.apply(v === "1" ? "SCAN START" : "SCAN STOP"), (st) => String(st.running)),
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
    this.ctl("Analog filter code (-1 = PHY auto; S3: 60 ≈ 13 MHz, 0 ≈ 69 MHz)", filter);
    this.syncs.push((st) => (filter.value = String(st.filter)));
    // FFT / capture
    const fftSel = el("select") as HTMLSelectElement;
    for (const v of [256, 512, 1024, 2048]) fftSel.append(new Option(`${v}-pt FFT`, String(v)));
    fftSel.onchange = () => void this.apply(`FFT ${fftSel.value}`);
    const capSel = el("select") as HTMLSelectElement;
    for (const v of [1024, 2048, 4096, 8192, 16380]) capSel.append(new Option(`${v} samples`, String(v)));
    capSel.onchange = () => void this.apply(`CAPN ${capSel.value}`);
    const segs = this.num(1, 64, 1, 56);
    segs.onchange = () => void this.apply(`SEGMENTS ${segs.value}`);
    this.ctl("Capture & Welch PSD (segments = max FFTs averaged)", capSel, fftSel, segs);
    this.syncs.push((st) => {
      fftSel.value = String(st.nfft);
      capSel.value = String(st.capture_samples);
      segs.value = String(st.segments);
    });
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
    // Smoothing
    const ema = this.num(0.001, 1, 0.01, 70);
    const clear = this.num(0.001, 1, 0.005, 70);
    const sm = () => void this.apply(`EMA ${Math.round(Number(ema.value) * 1000)} ${Math.round(Number(clear.value) * 1000)}`);
    ema.onchange = sm;
    clear.onchange = sm;
    this.ctl("RSSI EMA α · clear-floor EMA α (per update)", ema, clear);
    this.syncs.push((st) => {
      ema.value = st.ema.toFixed(3);
      clear.value = st.clear_ema.toFixed(3);
    });
    // Display spectrum
    const every = this.num(0, 255, 1, 56);
    const bins = el("select") as HTMLSelectElement;
    for (const v of [64, 128, 256, 512, 1024]) bins.append(new Option(`${v} bins`, String(v)));
    const span = this.num(100, 40000, 100, 80);
    const det = el("select") as HTMLSelectElement;
    det.append(new Option("max", "MAX"), new Option("mean", "MEAN"));
    const spec = () =>
      void this.apply(`SPEC ${every.value} ${bins.value} ${Math.round(Number(span.value) * 1000)} ${det.value}`);
    for (const c of [every, bins, span, det]) c.onchange = spec;
    this.ctl("Display spectrum: every N hops (0 off) · bins · ± span kHz · detector", every, bins, span, det);
    this.syncs.push((st) => {
      every.value = String(st.spec_every);
      bins.value = String(st.spec_bins);
      span.value = String(st.spec_span_hz / 1000);
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
    this.ctl(
      "Spectrum orientation",
      this.seg([["1", "Conjugate (default)"], ["0", "Raw"]], (v) => void this.apply(`CONJ ${v}`), (st) => String(st.conjugate)),
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
