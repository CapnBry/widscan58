// Voice callouts through the browser's speech synthesis (Web Speech API;
// Chrome's voices are Google's). A Google voice for the page's language is
// preferred when the browser has one. GPL-3.0-or-later.

export interface VoiceSettings {
  volume: number; // 0-100
  rate: number; // 0-2 (speech synthesis needs at least 0.1)
  pitch: number; // 0-2
}

export function speechSupported(): boolean {
  return typeof window !== "undefined" && "speechSynthesis" in window && "SpeechSynthesisUtterance" in window;
}

/** Speech that can be heard: the API and at least one voice. Firefox on Linux
 *  has the API but no voices without a speech service, and then speaks nothing
 *  without an error. Voices can arrive later (onVoicesChanged). */
export function speechAvailable(): boolean {
  return speechSupported() && speechSynthesis.getVoices().length > 0;
}

/** Calls fn whenever the browser's voice list changes. */
export function onVoicesChanged(fn: () => void) {
  if (speechSupported()) speechSynthesis.addEventListener("voiceschanged", fn);
}

/** A lap callout: the channel ("R4") and the lap in seconds to 0.1 s. */
export function lapCallout(channel: string, lapS: number): string {
  return `${channel} ${lapS.toFixed(1)}`;
}

let voice: SpeechSynthesisVoice | null | undefined; // undefined: not picked yet

function pickVoice(): SpeechSynthesisVoice | null {
  const voices = speechSynthesis.getVoices();
  if (!voices.length) return null; // still loading: the default voice for now
  const lang = (navigator.language || "en").toLowerCase();
  const base = lang.split("-")[0];
  const match = (v: SpeechSynthesisVoice, l: string) => v.lang.toLowerCase().replace("_", "-").startsWith(l);
  const google = voices.filter((v) => v.name.startsWith("Google"));
  return (
    google.find((v) => match(v, lang)) ??
    google.find((v) => match(v, base)) ??
    voices.find((v) => match(v, lang) && v.default) ??
    voices.find((v) => match(v, base)) ??
    null
  );
}

onVoicesChanged(() => (voice = undefined));
if (speechSupported()) speechSynthesis.getVoices(); // starts loading them (Chrome)

export function speak(text: string, s: VoiceSettings) {
  if (!speechSupported()) return;
  if (voice === undefined || voice === null) voice = pickVoice();
  const u = new SpeechSynthesisUtterance(text);
  if (voice) {
    u.voice = voice;
    u.lang = voice.lang;
  }
  u.volume = Math.max(0, Math.min(1, s.volume / 100));
  u.rate = Math.max(0.1, Math.min(2, s.rate));
  u.pitch = Math.max(0, Math.min(2, s.pitch));
  speechSynthesis.speak(u); // queued behind any callout still speaking
}
