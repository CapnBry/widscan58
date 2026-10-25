// Detection events options (the gear by Clear): what the event list shows,
// keeping the screen on while a device is connected, the lap rules
// (RhModel.lapRules) and voice callouts of lap times. Changes apply at once.
// GPL-3.0-or-later.
import type { App, UiSettings } from "../model/app";
import type { LapRules } from "../model/laps";
import { LAP_COUNT_MAX, LAP_COUNT_MIN, MIN_LAP_MAX_S } from "../model/rh";
import { el } from "../util/canvas";
import { lapCallout, onVoicesChanged, speak, speechAvailable } from "../util/speech";
import { ScreenWake } from "../util/wakeLock";

type Flag = "rhShowCrossings" | "rhShowFastest" | "keepScreenOn" | "voiceOn";
type Slider = "voiceVolume" | "voiceRate" | "voicePitch";
type RuleFlag = "minLapOn" | "fastestOn";
type RuleValue = "minLapS" | "count";

export class RhEventsDialog {
  readonly el: HTMLDialogElement;
  private boxes = new Map<Flag, HTMLInputElement>();
  private rules: { flag: RuleFlag; value: RuleValue; box: HTMLInputElement; input: HTMLInputElement }[] = [];
  private showFastest!: HTMLLabelElement;
  private sliders: { key: Slider; input: HTMLInputElement; out: HTMLElement; fmt: (v: number) => string }[] = [];
  private voiceControls = el("div", { class: "voice-controls" });
  private voiceOption!: HTMLLabelElement;
  private voiceError = el("div", { class: "options-error" }, "Speech is not available: this browser has no voices.");

  constructor(private app: App) {
    const closeBtn = el("button", { class: "primary", autofocus: "" }, "Close");
    closeBtn.onclick = () => this.el.close();
    const wake = ScreenWake.supported();
    const testBtn = el("button", { title: "Speak a callout for the selected seat" }, "Test");
    testBtn.onclick = () => this.testCallout();
    this.voiceControls.append(
      this.slider("voiceVolume", "Volume", 0, 100, 1, (v) => `${v}`),
      this.slider("voiceRate", "Rate", 0.1, 2, 0.1, (v) => `${v.toFixed(1)}×`),
      this.slider("voicePitch", "Pitch", 0, 2, 0.1, (v) => `${v.toFixed(1)}×`),
      el("div", { class: "voice-test" }, testBtn),
    );
    this.el = el(
      "dialog",
      { class: "options-dialog" },
      el("h2", {}, "Detection events"),
      el("h3", {}, "Display"),
      this.option("rhShowCrossings", "Split Enter/Exit events", "List each crossing's enter and exit, besides the passes."),
      (this.showFastest = this.option("rhShowFastest", "Fastest consecutive", "List each new fastest run. The seats show it either way.")),
      this.option(
        "keepScreenOn",
        "Keep screen on",
        wake ? "While a device is connected." : "Not available: this browser has no screen wake lock.",
        !wake,
      ),
      el("h3", {}, "Lap processing"),
      this.rule("minLapOn", "minLapS", "Minimum lap time", "s", 0, MIN_LAP_MAX_S, "Ignore passes sooner than this after the last one."),
      this.rule("fastestOn", "count", "Fastest consecutive laps", "laps", LAP_COUNT_MIN, LAP_COUNT_MAX, "Each seat's fastest run of this many laps."),
      el("h3", {}, "Voice callouts"),
      (this.voiceOption = this.option("voiceOn", "Announce laps", "Each lap's channel and time, e.g. “R4 13.7”.")),
      this.voiceControls,
      this.voiceError,
      el("div", { class: "flash-buttons" }, closeBtn),
    );
    this.boxes.get("voiceOn")!.addEventListener("change", () => this.syncVoice());
    onVoicesChanged(() => this.syncVoice());
  }

  open() {
    for (const [k, box] of this.boxes) box.checked = this.app.settings[k];
    this.syncRules();
    this.syncVoice();
    this.el.showModal();
  }

  /** Greyed out (and shown off, the setting kept) while there is no voice. */
  private syncVoice() {
    const voice = speechAvailable();
    const box = this.boxes.get("voiceOn")!;
    box.disabled = !voice;
    box.checked = voice && this.app.settings.voiceOn;
    this.voiceOption.classList.toggle("disabled", !voice);
    this.voiceError.hidden = voice;
    this.voiceControls.hidden = !box.checked;
    for (const { key, input, out, fmt } of this.sliders) {
      input.value = String(this.app.settings[key]);
      out.textContent = fmt(this.app.settings[key]);
    }
  }

  private slider(key: Slider, label: string, lo: number, hi: number, step: number, fmt: (v: number) => string) {
    const input = el("input", { type: "range", min: String(lo), max: String(hi), step: String(step), "aria-label": label }) as HTMLInputElement;
    const out = el("span", { class: "slider-value" });
    input.oninput = () => {
      const v = Number(input.value);
      this.app.settings[key] = v;
      out.textContent = fmt(v);
      this.app.changed();
    };
    this.sliders.push({ key, input, out, fmt });
    return el("label", { class: "slider" }, el("span", {}, label), input, out);
  }

  /** The selected seat's channel (or the first one on) and a made-up lap. */
  private testCallout() {
    const seats = this.app.rh.seats;
    const s = seats.find((x) => x.index === this.app.settings.selected && x.channel) ?? seats.find((x) => x.channel);
    const lapS = 10 + Math.random() * 20;
    speak(lapCallout(s?.channel ?? "R1", lapS), voiceSettings(this.app.settings));
  }

  private syncRules() {
    const r = this.app.rh.lapRules;
    for (const { flag, value, box, input } of this.rules) {
      box.checked = r[flag];
      input.disabled = !r[flag];
      input.value = String(r[value]);
    }
    // Nothing to list while no fastest run is kept.
    const box = this.boxes.get("rhShowFastest")!;
    box.disabled = !r.fastestOn;
    this.showFastest.classList.toggle("disabled", !r.fastestOn);
  }

  private setRules(patch: Partial<LapRules>) {
    this.app.rh.setLapRules(patch);
    this.syncRules();
  }

  private option(key: Flag, label: string, hint: string, disabled = false) {
    const box = el("input", { type: "checkbox" }) as HTMLInputElement;
    box.disabled = disabled;
    box.onchange = () => {
      (this.app.settings as UiSettings)[key] = box.checked;
      this.app.changed();
    };
    this.boxes.set(key, box);
    return el("label", { class: `option${disabled ? " disabled" : ""}` }, box, el("span", {}, label, el("small", {}, hint)));
  }

  /** A rule's checkbox and whole-number value, lo..hi (as many digits as hi). */
  private rule(flag: RuleFlag, value: RuleValue, label: string, unit: string, lo: number, hi: number, hint: string) {
    const box = el("input", { type: "checkbox" }) as HTMLInputElement;
    const digits = String(hi).length;
    const input = el("input", {
      type: "text",
      inputmode: "numeric",
      maxlength: String(digits),
      class: "num",
      style: `width: calc(${digits}ch + 14px)`,
      "aria-label": label,
    }) as HTMLInputElement;
    box.onchange = () => this.setRules({ [flag]: box.checked });
    input.oninput = () => {
      const digitsOnly = input.value.replace(/\D/g, "");
      if (digitsOnly !== input.value) input.value = digitsOnly;
    };
    // Applied on Enter or leaving the box; empty puts the current value back.
    input.onchange = () => {
      if (input.value === "") return this.syncRules();
      this.setRules({ [value]: Math.max(lo, Math.min(hi, Number(input.value))) });
    };
    this.rules.push({ flag, value, box, input });
    return el(
      "div",
      { class: "rule" },
      el("label", { class: "option" }, box, el("span", {}, label, el("small", {}, hint))),
      el("span", { class: "rule-value" }, input, ` ${unit}`),
    );
  }
}

export function voiceSettings(s: UiSettings) {
  return { volume: s.voiceVolume, rate: s.voiceRate, pitch: s.voicePitch };
}
