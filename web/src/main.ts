// Widscan58 web UI entry point. GPL-3.0-or-later.
import "./styles.css";
import { App, type View } from "./model/app";
import { Store, parseInfo } from "./model/store";
import {
  Device,
  WebSerialTransport,
  WebSocketTransport,
  grantedEspressifPort,
  requestEspressifPort,
  type Transport,
} from "./serial/device";
import { Recorder, ReplayTransport } from "./serial/recorder";
import { MockTransport, type SimTarget } from "./mock/mockDevice";
import { el, invalidateTheme } from "./util/canvas";
import { ChannelGrid } from "./views/channelGrid";
import { Panorama } from "./views/panorama";
import { Zoom } from "./views/zoom";
import { History } from "./views/history";
import { Telemetry, Console } from "./views/debug";
import { Controls } from "./views/controls";
import { RawIqView } from "./views/rawiq";
import { FlashDialog } from "./views/flashDialog";
import { GpioDialog } from "./views/gpioDialog";
import { ScreenWake } from "./util/wakeLock";
import { registerServiceWorker } from "./util/pwa";
import { lapCallout, speak } from "./util/speech";
import { voiceSettings } from "./views/timerEventsDialog";
import { TimerEvents, TimerGraph, TimerSeats, TimerSettings } from "./views/timer";
import { PowerChannels, PowerWarning } from "./views/power";
import { MONITOR_DEFAULTS, POWER_WIN_HZ, powerCommands } from "./model/power";

const store = new Store();
const app = new App(store);
const recorder = new Recorder();

// ---- Theme ------------------------------------------------------------------
// The installed app's title bar follows the theme (--page).
function setThemeColor(t: string) {
  document.querySelector('meta[name="theme-color"]')?.setAttribute("content", t === "light" ? "#f9f9f7" : "#0d0d0d");
}
function applyTheme(t: string) {
  document.documentElement.dataset.theme = t;
  setThemeColor(t);
  darkSwitch.checked = t !== "light";
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
setThemeColor(themeName);

registerServiceWorker(); // installable, works offline

// ---- Header -----------------------------------------------------------------
const dot = el("span", { class: "dot" });
const connText = el("span", {}, "Disconnected");
const connectBtn = el("button", { class: "primary" }, "Connect"); // becomes Disconnect while connected
const flashBtn = el("button", { title: "Install the scanner firmware on a board" }, "Flash device");
const simBtn = el("button", {}, "Simulate");
const replayInput = el("input", { type: "file", accept: ".widscan58", style: "display:none" }) as HTMLInputElement;
const replayBtn = el("button", {}, "Replay…");
const recordBtn = el("button", { title: "Record the raw device stream to a .widscan58 file" }, "● Record");
// Menu: the page / device protocol (monitor, lap timer, or power meter on
// the monitor protocol), GPIO Configure,
// the theme, and about: the project link and the connected device's firmware
// version.
const menuBtn = el("button", { class: "menu-btn", title: "Menu", "aria-haspopup": "true", "aria-expanded": "false" }, "☰");
const modeItems = {
  monitor: el("button", { class: "menu-item", role: "menuitemradio" }, "Monitor mode"),
  timer: el("button", { class: "menu-item", role: "menuitemradio" }, "Lap timer mode"),
  power: el("button", { class: "menu-item", role: "menuitemradio" }, "Power meter mode"),
};
const timerNote = el("div", { class: "menu-note" });
const gpioItem = el("button", { class: "menu-item", role: "menuitem", title: "Pins the device sets at startup" }, "GPIO Configure");
const darkSwitch = el("input", { type: "checkbox", role: "switch" }) as HTMLInputElement;
darkSwitch.checked = themeName !== "light";
const fwValue = el("span", { class: "menu-value" }, "not connected");
const fwNote = el("div", { class: "menu-note", hidden: "" }); // a newer firmware this page can flash
const menu = el(
  "div",
  { class: "menu", role: "menu", hidden: "" },
  el("div", { class: "menu-label" }, "Mode"),
  modeItems.monitor,
  modeItems.timer,
  modeItems.power,
  timerNote,
  el("div", { class: "menu-sep" }),
  gpioItem,
  el("label", { class: "menu-item menu-switch" }, el("span", {}, "Dark mode"), darkSwitch),
  el("div", { class: "menu-sep" }),
  el("div", { class: "menu-label" }, "About"),
  el("a", { class: "menu-item", role: "menuitem", href: "https://github.com/CapnBry/widscan58", target: "_blank", rel: "noopener" }, "GitHub project ↗"),
  el("div", { class: "menu-info" }, el("span", {}, "Firmware"), fwValue),
  fwNote,
);
const header = el(
  "header",
  { class: "top" },
  el("span", { class: "brand" }, el("span", { class: "menu-wrap" }, menuBtn, menu), el("h1", {}, "Widscan58")),
  el("span", { class: "conn-state" }, dot, connText),
  el("span", { class: "actions" }, connectBtn, flashBtn),
);
// Session sources other than a real device, shown in the protocol console.
const sessionRow = el("div", { class: "session-row" }, simBtn, replayBtn, replayInput, recordBtn);
const banner = el("div", { class: "banner", style: "display:none" });

type Page = "monitor" | "timer" | "power" | "both";
const views: { view: View; page: Page; minMs: number; last: number; drawn: number }[] = [];
function addView(page: Page, view: View, minMs = 0) {
  views.push({ view, page, minMs, last: 0, drawn: -1 });
  return view.el;
}
const monitorMain = el(
  "main",
  {},
  addView("monitor", new ChannelGrid(app), 100),
  addView("monitor", new Panorama(app), 33),
  addView("monitor", new Zoom(app), 33),
  addView("monitor", new History(app), 50),
  addView("monitor", new Telemetry(app), 200),
  addView("monitor", new Controls(app), 100),
  addView("monitor", new RawIqView(app), 100),
);
const timerMain = el(
  "main",
  { class: "timer-page" },
  addView("timer", new TimerSeats(app), 100),
  addView("timer", new TimerGraph(app), 50),
  addView("timer", new TimerEvents(app), 200),
  addView("timer", new TimerSettings(app), 300),
);
const powerMain = el(
  "main",
  { class: "power-page" },
  addView("power", new PowerWarning()),
  addView("power", new PowerChannels(app), 100),
);
const sharedMain = el("main", { class: "shared" }, addView("both", new Console(app, sessionRow), 200));
document.getElementById("app")!.append(header, banner, monitorMain, timerMain, powerMain, sharedMain);

// ---- Pages and device protocol ---------------------------------------------------
// Firmware without the PROTOCOL command is always in monitor mode.
let legacyFirmware = false;
function showPage() {
  const mode = app.settings.uiMode;
  monitorMain.style.display = mode === "monitor" ? "" : "none";
  timerMain.style.display = mode === "timer" ? "" : "none";
  powerMain.style.display = mode === "power" ? "" : "none";
  for (const [m, item] of Object.entries(modeItems)) item.setAttribute("aria-checked", String(m === mode));
  const replay = app.readOnly && !!app.device;
  modeItems.timer.disabled = legacyFirmware || replay;
  timerNote.textContent = legacyFirmware
    ? "This firmware has no lap timer mode: flash a newer one."
    : replay
      ? "Replays are monitor mode only."
      : "";
  timerNote.hidden = !timerNote.textContent;
  for (const v of views) v.drawn = -1;
  updateBanner();
}

/** Put a connected device in the page's protocol and start the page on it. */
let starting: Promise<void> | null = null;
function startMode(device: Device): Promise<void> {
  const run = async () => {
    let proto: string | null = null;
    try {
      proto = await device.text("PROTOCOL?", 1500);
    } catch (e) {
      store.addLog("info", `no protocol switch (${(e as Error).message}); monitor mode only`);
    }
    legacyFirmware = proto === null;
    if (legacyFirmware && app.settings.uiMode === "timer") app.settings.uiMode = "monitor";
    showPage();
    if (app.settings.uiMode === "timer") {
      await app.send("PROTOCOL RH");
      device.enterRh();
      await app.send("INFO"); // the firmware version (menu, flash dialog); syncDevice has it in monitor mode
      await app.timer.start(device, app.deviceSettings.channels);
    } else {
      await app.timer.stop();
      if (device.rh) {
        await app.send("PROTOCOL MONITOR");
        device.exitRh();
      } else if (!legacyFirmware) await app.send("PROTOCOL MONITOR");
      await syncDevice(device);
      if (app.settings.uiMode === "power") await enterPower(device);
      else await leavePower(device);
    }
    lastData = performance.now();
  };
  // One at a time: a mode change while a start runs waits for it.
  const p = (starting ?? Promise.resolve()).then(run, run);
  const tracked: Promise<void> = p
    .catch(() => {})
    .then(() => {
      if (starting === tracked) starting = null;
    });
  starting = tracked;
  return p;
}

function setMode(mode: "monitor" | "timer" | "power") {
  closeMenu();
  if (mode === app.settings.uiMode) return;
  app.settings.uiMode = mode;
  app.changed();
  showPage();
  if (app.device && !app.readOnly) void startMode(app.device);
}
modeItems.monitor.onclick = () => setMode("monitor");
modeItems.timer.onclick = () => setMode("timer");
modeItems.power.onclick = () => setMode("power");
const gpioDialog = new GpioDialog(app);
document.body.append(gpioDialog.el);
gpioItem.onclick = () => {
  closeMenu();
  gpioDialog.open();
};

function closeMenu() {
  menu.hidden = true;
  menuBtn.setAttribute("aria-expanded", "false");
}
menuBtn.onclick = (e) => {
  e.stopPropagation();
  menu.hidden = !menu.hidden;
  menuBtn.setAttribute("aria-expanded", String(!menu.hidden));
};
menu.onclick = (e) => e.stopPropagation();
document.addEventListener("click", closeMenu);
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") closeMenu();
});
darkSwitch.onchange = () => applyTheme(darkSwitch.checked ? "dark" : "light");

// ---- Connection ---------------------------------------------------------------
// Keep screen on (detection events options): only while a device is connected.
const screenWake = new ScreenWake((msg) => store.addLog("error", msg));
const syncWake = () => screenWake.set(app.settings.keepScreenOn && !!app.device);
store.subscribe(syncWake); // the option changing (app.changed touches the store)
// Voice callouts (detection events options): each lap's channel and time.
app.timer.onLap = (seat, lapS) => {
  if (app.settings.voiceOn && seat.channel) speak(lapCallout(seat.channel, lapS), voiceSettings(app.settings));
};

function setState(state: "off" | "ok" | "sim" | "err", text: string) {
  dot.className = `dot ${state === "off" ? "" : state}`;
  connText.textContent = connText.title = text; // the title shows it in full when cut short
  const connected = state === "ok" || state === "sim";
  connectBtn.textContent = connected ? "Disconnect" : "Connect";
  connectBtn.classList.toggle("primary", !connected);
  simBtn.disabled = connected;
  replayBtn.disabled = connected;
  recordBtn.disabled = !connected || app.readOnly;
  syncWake();
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

// The firmware version (INFO, parseInfo) is shown in the menu for any
// connected device; the offer of another version only over Web Serial, where
// the page can flash it.
let offeredFw: string | null = null;
function updateFirmwareText() {
  const fw = app.device ? parseInfo(store.info)?.version : undefined;
  const value = !app.device ? "not connected" : fw || "unknown";
  if (fwValue.textContent !== value) fwValue.textContent = value;
  const offer = fw && offeredFw && offeredFw !== fw && app.device?.transport instanceof WebSerialTransport;
  const note = offer ? `${offeredFw} available` : "";
  if (fwNote.textContent !== note) fwNote.textContent = note;
  fwNote.hidden = !note;
}

async function syncDevice(device: Device) {
  if (device.rh) return; /* lap timer mode: the page does its own setup */
  await app.restoreDevice(device);
  for (const cmd of ["INFO", "PROFILE?", "STATUS?", "SPURS?"]) {
    try {
      await device.command(cmd);
    } catch (e) {
      store.addLog("error", (e as Error).message);
    }
  }
}

// Power meter mode runs the monitor protocol with the scan its calibration
// was made with (model/power.ts). Monitor mode's own windows and spectrum are
// kept here and put back on leaving it; the channel plan, LO mode and gain
// come back with restoreDevice.
let monitorScan: { win: string; spec: string } | null = null;
const powerScan = (st: typeof store.status) => !!st && st.rssi_hz === POWER_WIN_HZ && st.spec_every === 0;
async function sendAll(device: Device, cmds: string[]) {
  for (const cmd of cmds) {
    store.addLog("tx", cmd);
    try {
      await device.command(cmd);
    } catch (e) {
      store.addLog("error", (e as Error).message);
    }
  }
}
async function enterPower(device: Device) {
  const st = store.status;
  if (st && !powerScan(st))
    monitorScan = {
      win: `WIN ${st.rssi_hz} ${st.search_hz}`,
      spec: `SPEC ${st.spec_every} ${st.spec_bins} ${st.spec_span_hz} ${st.spec_detector ? "MAX" : "MEAN"}`,
    };
  await sendAll(device, [...powerCommands(store.profile?.usable_hz ?? 40000000), "PROFILE?", "STATUS?"]);
}
async function leavePower(device: Device) {
  if (!powerScan(store.status)) return;
  const d = store.profile && MONITOR_DEFAULTS[store.profile.name];
  const scan = monitorScan ?? (d && { win: `WIN ${d.rssiHz} ${d.searchHz}`, spec: d.spec });
  if (scan) await sendAll(device, [scan.win, scan.spec, "PROFILE?", "STATUS?"]);
}

async function connect(transport: Transport, readOnly = false) {
  const device = new Device(transport);
  store.info = "";
  device.onItem((item) => {
    store.ingest(item);
    if (item.kind === "frame") lastData = performance.now();
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
  if (readOnly) {
    legacyFirmware = false;
    if (app.settings.uiMode === "timer") app.settings.uiMode = "monitor";
    showPage();
  } else await startMode(device);
  updateBanner();
}

async function disconnect() {
  userDisconnected = true;
  const d = app.device!;
  await app.timer.stop();
  // Leave the device silent, as a RotorHazard server expects to find it.
  if (!d.rh && !legacyFirmware && !app.readOnly && d.connected) await d.command("PROTOCOL RH", 500).catch(() => {});
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
// ?sim=s3 simulates the ESP32-S3's scaled 2.4 GHz band instead of the C5's.
const simTarget = (): SimTarget => (new URLSearchParams(location.search).get("sim") === "s3" ? "s3" : "c5");
simBtn.onclick = () => void connect(new MockTransport(simTarget()));
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
      if (!app.device!.rh) await app.device!.command("SCAN STOP", 500).catch(() => {});
      flashing = true;
      await disconnect();
      return port;
    }
    if (app.device) await disconnect(); // simulator or replay
    const port = await requestEspressifPort();
    flashing = true;
    return port;
  },
  device() {
    return app.device?.transport instanceof WebSerialTransport ? parseInfo(store.info) : null;
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
store.subscribe(() => flashDialog.refreshDevice()); // INFO after a (re)connect
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
// Web Serial (headless tests, remote viewing); ?sim starts the simulator
// (?sim=s3 for the S3 band).
const params = new URLSearchParams(location.search);
const bridge = params.get("bridge");
if (bridge) void connect(new WebSocketTransport(bridge));
else if (params.has("sim")) void connect(new MockTransport(simTarget()));
else if (WebSerialTransport.supported()) {
  WebSerialTransport.onConnectEvent(() => setTimeout(() => void autoConnect(), 500));
  void autoConnect();
}

store.subscribe(() => updateBanner());
setState("off", "Disconnected");
showPage();

// The firmware boots silent (lap timer mode). In monitor mode telemetry
// arrives at 4 Hz even when scanning is stopped, so a quiet link means the
// board restarted: switch it back.
let lastData = performance.now();
setInterval(() => {
  const d = app.device;
  if (!d || d.rh || app.readOnly || legacyFirmware || starting || flashing || app.settings.uiMode === "timer") return;
  if (performance.now() - lastData > 3000) {
    store.addLog("info", "no data for 3 s: switching the device to monitor mode again");
    lastData = performance.now();
    void startMode(d);
  }
}, 1000);

// ---- Render loop: each view redraws when data changed, at its own max rate ----
function frame(now: number) {
  const page = app.settings.uiMode;
  for (const v of views) {
    if (v.page !== "both" && v.page !== page) continue;
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
