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
const cards = await page.$$("section.card");
for (const [i, c] of cards.entries()) await c.screenshot({ path: path.join(out, `card-${i}.png`) });

// Light theme pass.
await page.click("button[title='Toggle light/dark']");
await sleep(800);
await page.screenshot({ path: path.join(out, "3-light.png"), fullPage: false });
// Phone width: no horizontal scroll.
await page.setViewport({ width: 390, height: 900 });
await sleep(800);
const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
check("no horizontal scroll at 390px", overflow <= 0, `overflow ${overflow}px`);
await page.screenshot({ path: path.join(out, "4-phone.png"), fullPage: false });
await page.click("button[title='Toggle light/dark']");

fs.writeFileSync(path.join(out, "results.json"), JSON.stringify({ url, results, final: s }, null, 1));
await browser.close();
const failed = results.filter((r) => !r.ok).length;
console.log(`${results.length - failed}/${results.length} checks passed`);
process.exit(failed ? 1 : 0);
