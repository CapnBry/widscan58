// Widscan58 web UI entry point. GPL-3.0-or-later.
import "./styles.css";
import { App, type View } from "./model/app";
import { Store } from "./model/store";
import {
  Device,
  WebSerialTransport,
  WebSocketTransport,
  grantedEspressifPort,
  requestEspressifPort,
  type Transport,
} from "./serial/device";
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
import { FlashDialog } from "./views/flashDialog";

const store = new Store();
const app = new App(store);
const recorder = new Recorder();

// ---- Theme ------------------------------------------------------------------
function applyTheme(t: string) {
  document.documentElement.dataset.theme = t;
  invalidateTheme();
  try {
    localStorage.setItem("widscan58.theme", t);
  } catch {
    /* storage unavailable */
  }
  store.touch();
}
let themeName = "dark";
try {
  themeName = localStorage.getItem("widscan58.theme") ?? "dark";
} catch {
  /* storage unavailable */
}
document.documentElement.dataset.theme = themeName;

// ---- Header -----------------------------------------------------------------
const dot = el("span", { class: "dot" });
const connText = el("span", {}, "Disconnected");
const fwText = el("span", { class: "fw" }); // firmware version, and whether this page offers another
const connectBtn = el("button", { class: "primary" }, "Connect"); // becomes Disconnect while connected
const flashBtn = el("button", { title: "Install the scanner firmware on a board" }, "Flash device");
const simBtn = el("button", {}, "Simulate");
const replayInput = el("input", { type: "file", accept: ".widscan58", style: "display:none" }) as HTMLInputElement;
const replayBtn = el("button", {}, "Replay…");
const recordBtn = el("button", { title: "Record the raw device stream to a .widscan58 file" }, "● Record");
const themeBtn = el("button", { title: "Toggle light/dark" }, "◐");
const header = el(
  "header",
  { class: "top" },
  el("span", { class: "brand" }, themeBtn, el("h1", {}, "Widscan58")),
  el("span", { class: "conn-state" }, dot, connText, fwText),
  el("span", { class: "spacer" }),
  connectBtn,
  flashBtn,
);
// Session sources other than a real device, shown in the protocol console.
const sessionRow = el("div", { class: "session-row" }, simBtn, replayBtn, replayInput, recordBtn);
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
  addView(new Console(app, sessionRow), 200),
);
document.getElementById("app")!.append(header, banner, main);

// ---- Connection ---------------------------------------------------------------
function setState(state: "off" | "ok" | "sim" | "err", text: string) {
  dot.className = `dot ${state === "off" ? "" : state}`;
  connText.textContent = text;
  const connected = state === "ok" || state === "sim";
  connectBtn.textContent = connected ? "Disconnect" : "Connect";
  connectBtn.classList.toggle("primary", !connected);
  simBtn.disabled = connected;
  replayBtn.disabled = connected;
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
        "Widths and windows are scaled by k.",
    );
  banner.replaceChildren(...parts);
  banner.style.display = parts.length ? "block" : "none";
  updateFirmwareText();
}

// INFO is "widscan58 <target> <proto_ver> <fw_ver> <phy_ver>".
let offeredFw: string | null = null;
function updateFirmwareText() {
  const fw = app.device?.transport instanceof WebSerialTransport ? store.info.split(" ")[3] : undefined;
  const text = !fw ? "" : offeredFw && offeredFw !== fw ? `· firmware ${fw} (${offeredFw} available)` : `· firmware ${fw}`;
  if (fwText.textContent !== text) fwText.textContent = text;
}

async function syncDevice(device: Device) {
  await app.restoreDevice(device);
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
  store.info = "";
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
  (window as unknown as { widscan58: unknown }).widscan58 = { app, store }; // test/debug hook
  store.addLog("info", `connected via ${transport.name}`);
  if (!readOnly) await syncDevice(device);
  updateBanner();
}

async function disconnect() {
  userDisconnected = true;
  const d = app.device!;
  app.device = null;
  stopRecording();
  await d.close();
  setState("off", "Disconnected");
}

connectBtn.onclick = async () => {
  if (app.device) return disconnect();
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
// ---- Flashing -------------------------------------------------------------------
let flashing = false; // keeps auto-reconnect off the port meanwhile
const flashDialog = new FlashDialog({
  async takePort() {
    const t = app.device?.transport;
    if (t instanceof WebSerialTransport && t.currentPort) {
      // Flash the connected board. Quiet its stream first, so the reset into
      // download mode does not race it (tools/flash.sh does the same).
      const port = t.currentPort;
      await app.device!.command("SCAN STOP", 500).catch(() => {});
      flashing = true;
      await disconnect();
      return port;
    }
    if (app.device) await disconnect(); // simulator or replay
    const port = await requestEspressifPort();
    flashing = true;
    return port;
  },
  finished(ok) {
    flashing = false;
    if (!ok) return;
    // The board re-enumerates after its reset; autoConnect also runs on that event.
    userDisconnected = false;
    setTimeout(() => void autoConnect(), 1500);
  },
});
document.body.append(flashDialog.el);
flashBtn.onclick = () => {
  if (!WebSerialTransport.supported()) {
    store.addLog("error", "Flashing needs Web Serial (Chrome, Edge or Opera on a computer) or WebUSB (Chrome on Android).");
    setState("err", "Web Serial unsupported in this browser");
    return;
  }
  flashDialog.open();
};
void flashDialog.offeredVersion().then((v) => {
  offeredFw = v;
  updateFirmwareText();
});

themeBtn.onclick = () => applyTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light");

function stopRecording() {
  if (!recorder.active) return;
  const blob = recorder.end();
  const a = el("a", { href: URL.createObjectURL(blob), download: `session-${new Date().toISOString().replace(/[:.]/g, "-")}.widscan58` });
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
  if (app.device || userDisconnected || flashing) return;
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
  WebSerialTransport.onConnectEvent(() => setTimeout(() => void autoConnect(), 500));
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
