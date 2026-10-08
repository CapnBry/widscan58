// esp-rx5808 web UI entry point. GPL-3.0-or-later.
import "./styles.css";
import { App, type View } from "./model/app";
import { Store } from "./model/store";
import { Device, WebSerialTransport, WebSocketTransport, grantedEspressifPort, type Transport } from "./serial/device";
import { Recorder, ReplayTransport } from "./serial/recorder";
import { MockTransport } from "./mock/mockDevice";
import { el, invalidateTheme } from "./util/canvas";
import { ChannelGrid } from "./views/channelGrid";
import { Panorama } from "./views/panorama";
import { Zoom } from "./views/zoom";
import { History } from "./views/history";
import { Telemetry, Console } from "./views/debug";
import { Controls } from "./views/controls";
import { RawIqView } from "./views/rawiq";

const store = new Store();
const app = new App(store);
const recorder = new Recorder();

// ---- Theme ------------------------------------------------------------------
function applyTheme(t: string) {
  document.documentElement.dataset.theme = t;
  invalidateTheme();
  try {
    localStorage.setItem("rx5808.theme", t);
  } catch {
    /* storage unavailable */
  }
  store.touch();
}
let themeName = "dark";
try {
  themeName = localStorage.getItem("rx5808.theme") ?? "dark";
} catch {
  /* storage unavailable */
}
document.documentElement.dataset.theme = themeName;

// ---- Header -----------------------------------------------------------------
const dot = el("span", { class: "dot" });
const connText = el("span", {}, "Disconnected");
const connectBtn = el("button", { class: "primary" }, "Connect device");
const simBtn = el("button", {}, "Simulate");
const replayInput = el("input", { type: "file", accept: ".rx5808", style: "display:none" }) as HTMLInputElement;
const replayBtn = el("button", {}, "Replay…");
const recordBtn = el("button", { title: "Record the raw device stream to a .rx5808 file" }, "● Record");
const disconnectBtn = el("button", {}, "Disconnect");
const themeBtn = el("button", { title: "Toggle light/dark" }, "◐");
const header = el(
  "header",
  { class: "top" },
  el("h1", {}, "RX5808 Scanner"),
  el("span", { class: "conn-state" }, dot, connText),
  el("span", { class: "spacer" }),
  connectBtn,
  simBtn,
  replayBtn,
  replayInput,
  recordBtn,
  disconnectBtn,
  themeBtn,
);
const banner = el("div", { class: "banner", style: "display:none" });

const views: { view: View; minMs: number; last: number; drawn: number }[] = [];
function addView(view: View, minMs = 0) {
  views.push({ view, minMs, last: 0, drawn: -1 });
  return view.el;
}
const main = el(
  "main",
  {},
  addView(new ChannelGrid(app), 100),
  addView(new Panorama(app), 33),
  addView(new Zoom(app), 33),
  addView(new History(app), 50),
  addView(new Telemetry(app), 200),
  addView(new Controls(app), 100),
  addView(new RawIqView(app), 100),
  addView(new Console(app), 200),
);
document.getElementById("app")!.append(header, banner, main);

// ---- Connection ---------------------------------------------------------------
function setState(state: "off" | "ok" | "sim" | "err", text: string) {
  dot.className = `dot ${state === "off" ? "" : state}`;
  connText.textContent = text;
  const connected = state === "ok" || state === "sim";
  connectBtn.disabled = connected;
  simBtn.disabled = connected;
  replayBtn.disabled = connected;
  disconnectBtn.disabled = !connected;
  recordBtn.disabled = !connected || app.readOnly;
}

function updateBanner() {
  const p = store.profile;
  const parts: (Node | string)[] = [];
  const add = (strong: string, rest: string) => parts.push(el("strong", {}, strong), ` ${rest} `);
  if (app.device?.transport instanceof MockTransport) add("Simulated device.", "Signals, spurs and IMD products are synthetic.");
  if (app.readOnly) add("Replay.", "Controls are disabled.");
  if (p && p.scale !== 1)
    add(
      `${p.name}:`,
      `Raceband scaled into ${p.band_khz[0] / 1000}–${p.band_khz[1] / 1000} MHz (k = ${p.scale.toFixed(4)}). ` +
        "Widths and windows are scaled by k; use “5.8 GHz axis” to read frequencies as their Raceband equivalent.",
    );
  banner.replaceChildren(...parts);
  banner.style.display = parts.length ? "block" : "none";
}

async function syncDevice(device: Device) {
  for (const cmd of ["INFO", "PROFILE?", "STATUS?", "SPURS?"]) {
    try {
      await device.command(cmd);
    } catch (e) {
      store.addLog("error", (e as Error).message);
    }
  }
}

async function connect(transport: Transport, readOnly = false) {
  const device = new Device(transport);
  device.onItem((item) => {
    store.ingest(item);
    // The firmware announces READY after every boot; its settings are back
    // to defaults, so re-read them (a software reset keeps USB connected).
    if (item.kind === "text" && item.line.startsWith("READY") && app.device === device && !readOnly)
      void syncDevice(device);
  });
  device.onRaw((chunk) => recorder.add(chunk));
  if (transport instanceof WebSerialTransport) transport.onWarning = (msg) => store.addLog("info", `serial: ${msg}`);
  device.onClose = (why) => {
    store.addLog("info", `connection closed: ${why}`);
    if (app.device === device) {
      app.device = null;
      setState(why === "replay finished" ? "off" : "err", why === "replay finished" ? "Replay finished" : `Disconnected (${why})`);
      stopRecording();
    }
  };
  try {
    await device.open();
  } catch (e) {
    store.addLog("error", `open failed: ${(e as Error).message}`);
    setState("err", `Connection failed (${(e as Error).message})`);
    return;
  }
  app.device = device;
  app.readOnly = readOnly;
  setState(transport instanceof MockTransport ? "sim" : "ok", transport.name);
  (window as unknown as { rx5808: unknown }).rx5808 = { app, store }; // test/debug hook
  store.addLog("info", `connected via ${transport.name}`);
  if (!readOnly) await syncDevice(device);
  updateBanner();
}

connectBtn.onclick = () => {
  if (!WebSerialTransport.supported()) {
    store.addLog("error", "Web Serial is not available: use Chrome, Edge or Opera over https or localhost.");
    setState("err", "Web Serial unsupported in this browser");
    return;
  }
  userDisconnected = false;
  void connect(new WebSerialTransport());
};
simBtn.onclick = () => void connect(new MockTransport());
replayBtn.onclick = () => replayInput.click();
replayInput.onchange = async () => {
  const f = replayInput.files?.[0];
  if (!f) return;
  void connect(new ReplayTransport(await f.arrayBuffer(), f.name), true);
  replayInput.value = "";
};
disconnectBtn.onclick = async () => {
  userDisconnected = true;
  const d = app.device;
  app.device = null;
  stopRecording();
  await d?.close();
  setState("off", "Disconnected");
};
themeBtn.onclick = () => applyTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light");

function stopRecording() {
  if (!recorder.active) return;
  const blob = recorder.end();
  const a = el("a", { href: URL.createObjectURL(blob), download: `session-${new Date().toISOString().replace(/[:.]/g, "-")}.rx5808` });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  recordBtn.textContent = "● Record";
  recordBtn.classList.remove("on");
  store.addLog("info", "recording saved");
}
recordBtn.onclick = () => {
  if (recorder.active) return stopRecording();
  const pre: string[] = [];
  if (store.profile) pre.push(`PROFILE ${JSON.stringify(store.profile)}`);
  if (store.status) pre.push(`STATUS ${JSON.stringify(store.status)}`);
  if (store.spursKhz.length) pre.push(`SPURS ${store.spursKhz.length} ${store.spursKhz.join(" ")}`);
  recorder.begin(pre);
  recordBtn.textContent = "■ Stop recording";
  recordBtn.classList.add("on");
};

// Auto-reconnect: once this origin has been granted the board, reconnect
// without the chooser on page load and whenever the board re-enumerates
// (reset, reflash, replug).
let userDisconnected = false;
async function autoConnect() {
  if (app.device || userDisconnected) return;
  const port = await grantedEspressifPort();
  if (port && !app.device) {
    store.addLog("info", "reconnecting to previously granted port");
    await connect(new WebSerialTransport(port));
  }
}
// ?bridge=ws://host:port connects through tools/serial_bridge.py instead of
// Web Serial (headless tests, remote viewing); ?sim starts the simulator.
const params = new URLSearchParams(location.search);
const bridge = params.get("bridge");
if (bridge) void connect(new WebSocketTransport(bridge));
else if (params.has("sim")) void connect(new MockTransport());
else if (WebSerialTransport.supported()) {
  navigator.serial.addEventListener("connect", () => setTimeout(() => void autoConnect(), 500));
  void autoConnect();
}

store.subscribe(() => updateBanner());
setState("off", "Disconnected");

// ---- Render loop: each view redraws when data changed, at its own max rate ----
function frame(now: number) {
  for (const v of views) {
    if (v.drawn === store.version || now - v.last < v.minMs) continue;
    v.last = now;
    v.drawn = store.version;
    try {
      v.view.render();
    } catch (e) {
      console.error(e);
    }
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
window.addEventListener("resize", () => store.touch());
