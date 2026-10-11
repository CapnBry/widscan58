// Headless end-to-end check of the web UI against the simulator or real
// hardware through tools/serial_bridge.py. GPL-3.0-or-later.
//
//   node e2e/headless.mjs [--url http://localhost:5808/] [--bridge ws://127.0.0.1:5809 | --sim] [--out dir]
import puppeteer from "puppeteer-core";
import fs from "node:fs";
import path from "node:path";

const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : def;
};
const base = arg("--url", "http://localhost:5808/");
const sim = process.argv.includes("--sim");
const bridge = arg("--bridge", "ws://127.0.0.1:5809");
const out = arg("--out", "e2e-out");
const chrome = arg("--chrome", "/usr/bin/google-chrome-stable");
fs.mkdirSync(out, { recursive: true });

const url = sim ? `${base}?sim` : `${base}?bridge=${encodeURIComponent(bridge)}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = [];
const check = (name, ok, detail = "") => {
  results.push({ name, ok, detail });
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
};

const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ["--no-sandbox"] });
const page = await browser.newPage();
await page.setViewport({ width: 1440, height: 1000, deviceScaleFactor: 1 });
const consoleErrors = [];
page.on("console", (m) => m.type() === "error" && consoleErrors.push(m.text()));
page.on("pageerror", (e) => consoleErrors.push(String(e)));
await page.goto(url, { waitUntil: "networkidle0" });
await sleep(5000);

const snap = () =>
  page.evaluate(() => {
    const { app, store } = window.widscan58 ?? {};
    if (!store) return null;
    const dev = app.device;
    return {
      connected: !!dev,
      profile: store.profile?.name,
      nch: store.profile?.channels.length,
      status: store.status && { enabled: store.status.enabled, hops: store.status.plan.length, gain: store.status.gain, mode: store.status.mode },
      telemetry: store.telemetry && { rot: store.telemetry.rotationHz, hop: store.telemetry.hopUsAvg, drops: store.telemetry.drops },
      channelsWithData: store.channels.filter((c) => c.latest).length,
      rates: store.channels.map((c) => Math.round(c.rateHz)),
      segments: store.segments.size,
      spurs: store.spursKhz,
      stats: dev?.stats(),
      errors: store.log.filter((l) => l.dir === "error").map((l) => l.text),
      rawiq: store.rawiq && { n: store.rawiq.n, lo: store.rawiq.loKhz },
    };
  });

let s = await snap();
check("page exposes app hook", !!s);
check("connected", s?.connected, sim ? "simulator" : bridge);
check("profile loaded", !!s?.profile, s?.profile);
check("status loaded", !!s?.status, JSON.stringify(s?.status));
check("all channels updating", s?.channelsWithData === s?.nch, `${s?.channelsWithData}/${s?.nch}, rates ${s?.rates}`);
check("spectrum segments", s?.segments >= 1, `${s?.segments}`);
check("telemetry", !!s?.telemetry, JSON.stringify(s?.telemetry));
check("no CRC errors", (s?.stats?.crcErrors ?? 1) === 0, JSON.stringify(s?.stats));
await page.screenshot({ path: path.join(out, "1-overview.png"), fullPage: true });

// Interactions through the UI's own command path.
const send = (cmd) => page.evaluate((c) => window.widscan58.app.send(c).then(() => window.widscan58.app.refreshStatus()), cmd);
await send("ENABLE 15");
await sleep(1500);
s = await snap();
check("ENABLE 15 -> 2 pair hops", s.status?.enabled === 15 && s.status?.hops === 2, JSON.stringify(s.status));
await send("MODE SINGLE");
await sleep(800);
s = await snap();
check("MODE SINGLE -> 4 hops", s.status?.hops === 4 && s.status?.mode === "SINGLE", JSON.stringify(s.status));
await send("MODE GROUP");
await send("ENABLE 255");
await send("GAIN MANUAL 50");
await sleep(1500);
s = await snap();
check("restore + GAIN MANUAL 50", s.status?.enabled === 255 && s.status?.gain === 50, JSON.stringify(s.status));

// Reduced metrics: FRAME_LEVELS; what the mode does not measure is left blank.
for (const mode of ["SNR", "RSSI"]) {
  await send(`METRICS ${mode}`);
  await sleep(1500);
  const lv = await page.evaluate(() => {
    const st = window.widscan58.store;
    const m = st.lastMetrics;
    const c = m?.channels[0];
    return { status: st.status?.metrics, mode: m?.mode, rssi: c?.rssi, snr: c?.snr, ema: c?.rssiEma, center: c?.centroidOffHz };
  });
  const ok = lv.status === mode && lv.mode === mode && Number.isFinite(lv.rssi) && lv.ema === null && lv.center === null &&
    (mode === "SNR" ? Number.isFinite(lv.snr) : lv.snr === null);
  check(`METRICS ${mode} -> levels frames`, ok, JSON.stringify(lv));
  await page.screenshot({ path: path.join(out, `metrics-${mode.toLowerCase()}.png`), fullPage: true });
}
await send("METRICS FULL");
await sleep(1000);

// RotorHazard mode through the menu: the page polls the device as a
// RotorHazard server would; the simulator flies a pass every 5-9 s per seat.
const menuPick = async (label) => {
  await page.click(".menu-btn");
  const [item] = await page.$$(`xpath/.//button[contains(@class,'menu-item') and contains(., '${label}')]`);
  await item.click();
};
await menuPick("RotorHazard mode");
await sleep(sim ? 12000 : 6000);
const rhSnap = () =>
  page.evaluate(() => {
    const { app } = window.widscan58;
    const rh = app.rh;
    return {
      mode: app.settings.uiMode,
      rhLink: !!app.device?.rh,
      running: rh.running,
      error: rh.error,
      seatsOn: rh.seats.filter((x) => x.channel).length,
      withLevel: rh.seats.filter((x) => x.channel && x.level !== null).length,
      passes: rh.events.filter((e) => e.kind === "pass").length,
      link: app.device?.rh?.stats,
      cycleMs: Math.round(rh.cycleMs),
      pageShown: getComputedStyle(document.querySelector("main.rh-page")).display !== "none",
    };
  });
let r = await rhSnap();
check("RotorHazard mode polls all seats", r.mode === "rh" && r.rhLink && r.running && r.pageShown && r.seatsOn === 8 && r.withLevel === 8, JSON.stringify(r));
check("RotorHazard link without failed reads", r.link && r.link.reads > 100 && r.link.errors === 0, JSON.stringify(r.link));
if (sim) check("RotorHazard passes detected", r.passes >= 1, `${r.passes} passes`);
const enterInput = await page.$(".rh-seat input");
await enterInput.click();
await page.keyboard.down("Control");
await page.keyboard.press("a");
await page.keyboard.up("Control");
await enterInput.type("201");
await page.keyboard.press("Tab");
await sleep(500);
const enter0 = await page.evaluate(() => window.widscan58.app.rh.seats[0].enter);
check("EnterAt edit reaches the seat", enter0 === 201, `seat 1 enter ${enter0}`);
await page.evaluate(() => window.widscan58.app.rh.setThreshold(0, "enter", 190));
await page.screenshot({ path: path.join(out, "rotorhazard.png"), fullPage: true });
// Saved defaults: the device applies them on every PROTOCOL RH, and METRICS
// FULL on PROTOCOL MONITOR. The device's own saved values are put back after.
const rhCmd = (c) => page.evaluate((c) => window.widscan58.app.rh.command(c), c);
const rhCfg = () => page.evaluate(() => ({ ...window.widscan58.app.rh.config, metric: window.widscan58.app.rh.metric }));
const saved0 = await rhCfg();
await rhCmd("FFT 256");
await page.evaluate(() => window.widscan58.app.rh.setMetric("SNR"));
const [saveBtn] = await page.$$("xpath/.//button[@title='Save these settings as default']");
await saveBtn.click();
await sleep(500);
await rhCmd("FFT 512"); // not saved: gone after the next switch
await menuPick("Monitor mode");
await sleep(3000);
const monMetrics = await page.evaluate(() => window.widscan58.store.status?.metrics);
check("PROTOCOL MONITOR applies METRICS FULL", monMetrics === "FULL", `${monMetrics}`);
await menuPick("RotorHazard mode");
await sleep(3000);
const reapplied = await rhCfg();
check(
  "PROTOCOL RH applies the saved settings",
  reapplied.nfft === 256 && reapplied.metrics === "SNR" && reapplied.metric === "SNR",
  JSON.stringify({ nfft: reapplied.nfft, metrics: reapplied.metrics, page: reapplied.metric }),
);
for (const c of [
  `FFT ${saved0.nfft}`,
  `CAPN ${saved0.capture_samples}`,
  `MODE ${saved0.mode}`,
  saved0.gain_mode === "MANUAL" ? `GAIN MANUAL ${saved0.gain}` : "GAIN HARDWARE",
])
  await rhCmd(c);
await page.evaluate((m) => window.widscan58.app.rh.setMetric(m), saved0.metrics);
await page.evaluate(() => window.widscan58.app.rh.saveDefaults());
await menuPick("Monitor mode");
await sleep(3000);
s = await snap();
const proto = await page.evaluate(() => window.widscan58.store.status?.protocol);
const backOk = [
  proto === "MONITOR" && s.channelsWithData === s.nch && !(await page.evaluate(() => !!window.widscan58.app.device?.rh)),
  `protocol ${proto}, ${s.channelsWithData}/${s.nch} channels`,
];
check("back to monitor mode", backOk[0], backOk[1]);
// A restart boots the device silent (RotorHazard mode); the page notices the
// missing telemetry and switches it back to monitor mode.
await page.evaluate(() => window.widscan58.app.send("REBOOT"));
await sleep(9000);
const after = await page.evaluate(() => {
  const { store } = window.widscan58;
  return { proto: store.status?.protocol, fresh: store.channels.filter((c) => performance.now() - c.updated < 1000).length };
});
check("monitor mode resumes after REBOOT", after.proto === "MONITOR" && after.fresh === 8, JSON.stringify(after));

// Click a channel tile and the panorama, capture raw I/Q via the button.
const tiles = await page.$$(".tile");
await tiles[5].click();
const selected = await page.evaluate(() => window.widscan58.app.settings.selected);
check("tile click selects channel", selected === 5, `selected ${selected}`);
const [rawBtn] = await page.$$("xpath/.//button[contains(., 'Capture')]");
await rawBtn.click();
await sleep(2500);
s = await snap();
check("raw I/Q capture", !!s.rawiq, JSON.stringify(s.rawiq));
const sizes = await page.evaluate(() => {
  const max = window.widscan58.store.status?.rawiq_max;
  const card = [...document.querySelectorAll("section")].find((e) => e.querySelector("h2")?.textContent.startsWith("Raw I/Q"));
  const opts = [...(card?.querySelectorAll("select option") ?? [])];
  return { max, n: opts.length, bad: opts.filter((o) => o.disabled !== Number(o.value) > (max ?? Infinity)).map((o) => o.value) };
});
check("raw I/Q sizes respect rawiq_max", sizes.n > 0 && sizes.bad.length === 0, JSON.stringify(sizes));
await send("GAIN MANUAL 60");
await sleep(3000);
s = await snap();
check("no command errors", s.errors.length === 0, s.errors.join(" | "));
check("no console errors", consoleErrors.length === 0, consoleErrors.slice(0, 5).join(" | "));
await page.screenshot({ path: path.join(out, "2-after.png"), fullPage: true });
// Crops of the main panels for review.
const cards = (await page.$$("section.card")).filter(Boolean);
for (let j = cards.length - 1; j >= 0; j--) if (!(await cards[j].isVisible())) cards.splice(j, 1);
for (const [i, c] of cards.entries()) await c.screenshot({ path: path.join(out, `card-${i}.png`) });

// Light theme pass.
await page.click(".menu-btn");
await page.click(".menu-switch input");
await page.keyboard.press("Escape");
await sleep(800);
await page.screenshot({ path: path.join(out, "3-light.png"), fullPage: false });
// Phone width: no horizontal scroll.
await page.setViewport({ width: 390, height: 900 });
await sleep(800);
const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
check("no horizontal scroll at 390px", overflow <= 0, `overflow ${overflow}px`);
await page.screenshot({ path: path.join(out, "4-phone.png"), fullPage: false });
await page.click(".menu-btn");
await page.click(".menu-switch input");
await page.keyboard.press("Escape");

fs.writeFileSync(path.join(out, "results.json"), JSON.stringify({ url, results, final: s }, null, 1));
await browser.close();
const failed = results.filter((r) => !r.ok).length;
console.log(`${results.length - failed}/${results.length} checks passed`);
process.exit(failed ? 1 : 0);
