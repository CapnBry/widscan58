// Layout stability: channel tiles and the zoom card must keep their height
// whether or not channels show a signal. GPL-3.0-or-later.
//
//   node e2e/layout-stability.mjs [--url http://localhost:5808/] [--widths 1113,1300,1500]
import puppeteer from "puppeteer-core";

const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : def;
};
const base = arg("--url", "http://localhost:5808/");
const widths = arg("--widths", "700,1000,1113,1200,1300,1400,1500,1800").split(",").map(Number);
const chrome = arg("--chrome", "/usr/bin/google-chrome-stable");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ["--no-sandbox"] });
const page = await browser.newPage();
await page.setViewport({ width: 1440, height: 1000, deviceScaleFactor: 1 });
await page.goto(`${base}?sim`, { waitUntil: "networkidle0" });
await sleep(3000);
// Freeze live metrics so each state below is rendered exactly as set.
await page.evaluate(() => {
  window.widscan58.store.onMetrics = () => {};
});

// Worst cases per state: every channel quiet, or every channel showing a
// signal with the longest readouts and badge combinations.
const states = {
  quiet: { flags: 0, peak: -80, peakOffHz: 0, centroidOffHz: 0, obwLoHz: 0, obwHiHz: 0 },
  signal: { flags: 1 | 32, peak: -12.3, peakOffHz: -12345678, centroidOffHz: -12345678, obwLoHz: -12345000, obwHiHz: 12345000 },
  "signal+clip": { flags: 1 | 2 | 32, peak: -12.3, peakOffHz: -12345678, centroidOffHz: -12345678, obwLoHz: -12345000, obwHiHz: 12345000 },
};
const measure = (state) =>
  page.evaluate(async (st) => {
    const { app, store } = window.widscan58;
    for (const cs of store.channels) {
      if (!cs?.latest) continue;
      Object.assign(cs.latest, st, { rssiEma: -45.6, rssi: -45.6, snr: 34.5, floor: -98.7, clearFloor: -99.1 });
      cs.updated = performance.now() + 1e9; // never stale
    }
    app.settings.selected = 0;
    store.touch();
    await new Promise((r) => setTimeout(r, 400));
    const h = (q) => Math.round(document.querySelector(q).getBoundingClientRect().height);
    const zoom = [...document.querySelectorAll("section.card")].find((s) => s.textContent.startsWith("Channel zoom"));
    return {
      tiles: h(".tiles"),
      zoom: Math.round(zoom.getBoundingClientRect().height),
      body: document.body.scrollHeight,
    };
  }, state);

let fail = 0;
for (const w of widths) {
  await page.setViewport({ width: w, height: 1000, deviceScaleFactor: 1 });
  await sleep(300);
  const r = {};
  for (const [name, st] of Object.entries(states)) r[name] = await measure(st);
  const keys = ["tiles", "zoom", "body"];
  const bad = keys.filter((k) => new Set(Object.values(r).map((x) => x[k])).size > 1);
  if (bad.length) fail++;
  console.log(
    `${bad.length ? "FAIL" : "PASS"}  ${w}px  ` +
      keys.map((k) => `${k} ${[...new Set(Object.values(r).map((x) => x[k]))].join("/")}`).join("  "),
  );
}
await browser.close();
process.exit(fail ? 1 : 0);
