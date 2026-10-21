// Widscan58 service worker: makes the page installable and usable offline (at
// a race field without internet). GPL-3.0-or-later.
//
// The build (vite.config.ts, pwaPlugin) prepends CACHE, named after a hash of
// the build's files, FILES, the page itself, and FLASH_FILES, the firmware and
// the flasher's code, and writes the result to dist/sw.js. A deploy changes
// CACHE, so the browser installs the new worker in the background; it takes
// over at once. Page loads go to the network first (the fetch handler), so a
// load after a deploy runs the new version; the cached page is the fallback
// offline. No prompts either way.
//
// FLASH_FILES are not installed with the page: those who flash download them
// then, and they are kept (RUNTIME) for flashing again offline, until the next
// deploy. With "Make firmware available offline" (the flash dialog, installed
// app only; the PREFS cache remembers it) they are installed with the page too
// (OFFLINE), and every update brings the new firmware.
/* global CACHE, FILES, FLASH_FILES */
const RUNTIME = CACHE + "-rt";
const OFFLINE = CACHE + "-fw";
const PREFS = "widscan58-prefs";
const PREF_OFFLINE = "./offline-firmware";

// caches.match(…, {cacheName}), not caches.open(): that would create the cache.
const offlineWanted = async () => !!(await caches.match(PREF_OFFLINE, { cacheName: PREFS }));

/** All FLASH_FILES into OFFLINE, or none of them. */
async function saveOffline(reload) {
  const c = await caches.open(OFFLINE);
  try {
    await c.addAll(FLASH_FILES.map((f) => new Request(f, { cache: reload ? "reload" : "no-cache" })));
  } catch (err) {
    await caches.delete(OFFLINE);
    throw err;
  }
}

self.addEventListener("install", (e) => {
  e.waitUntil(
    (async () => {
      const c = await caches.open(CACHE);
      await c.addAll(FILES.map((f) => new Request(f, { cache: "reload" })));
      // A failed firmware download does not hold back the update: flashing
      // then fetches it, as without the option.
      if (await offlineWanted()) await saveOffline(true).catch(() => {});
      await self.skipWaiting();
    })(),
  );
});

self.addEventListener("activate", (e) => {
  const keep = [CACHE, RUNTIME, OFFLINE, PREFS];
  e.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k.startsWith("widscan58-") && !keep.includes(k)).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

/** A page load from the network, revalidated past the HTTP cache (GitHub
 *  Pages: max-age=600); null when it fails or takes over NAV_TIMEOUT_MS. */
const NAV_TIMEOUT_MS = 3000;
async function navigateFromNetwork(req) {
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), NAV_TIMEOUT_MS);
  try {
    const r = await fetch(req.url, { cache: "no-cache", credentials: "same-origin", signal: abort.signal });
    return r.ok ? r : null;
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

// Page loads (any query: ?sim, ?bridge=) from the network first, so a deploy
// shows at once; its assets have new hashed names, so they come from the
// network too. Offline (or slow), the cached index.html. The saved firmware
// and the rest of the page from their caches. Everything else from the
// network, kept in RUNTIME for when it is offline: the firmware manifest and
// its parts are fetched together when flashing, so the cached ones match each
// other.
self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.method !== "GET" || new URL(req.url).origin !== self.location.origin) return;
  const opts = { ignoreSearch: true, ignoreVary: true };
  e.respondWith(
    (async () => {
      if (req.mode === "navigate") {
        const page = (await navigateFromNetwork(req)) ?? (await caches.match("index.html", { cacheName: CACHE }));
        if (page) return page;
      }
      for (const cacheName of [CACHE, OFFLINE]) {
        const hit = await caches.match(req, { ...opts, cacheName });
        if (hit) return hit;
      }
      try {
        const r = await fetch(req);
        if (r.ok) await (await caches.open(RUNTIME)).put(req, r.clone());
        return r;
      } catch (err) {
        return (await caches.match(req, { ...opts, cacheName: RUNTIME })) ?? Promise.reject(err);
      }
    })(),
  );
});

/** What the flash dialog shows: whether the firmware is kept offline (wanted,
 *  saved and its version) and whether the firmware files are on this device
 *  at all (saved, or kept from flashing). */
async function status() {
  const wanted = await offlineWanted();
  const has = async (cacheName, f) => !!(await caches.match(f, { ignoreVary: true, cacheName }));
  let saved = true;
  for (const f of FLASH_FILES) saved &&= await has(OFFLINE, f);
  let firmware = true;
  for (const f of FLASH_FILES.filter((f) => f.startsWith("firmware/"))) firmware &&= (await has(OFFLINE, f)) || (await has(RUNTIME, f));
  let version = null;
  if (saved) {
    const m = await caches.match("firmware/manifest.json", { cacheName: OFFLINE });
    version = (await m?.json().catch(() => null))?.version ?? null;
  }
  return { wanted, saved, version, firmware };
}

// Requests from the page (util/pwa.ts), answered on the message's port:
// {cmd: "status"} and {cmd: "offline", on}, both with status() or {error}.
self.addEventListener("message", (e) => {
  const reply = e.ports[0];
  const { cmd, on } = e.data ?? {};
  if (!reply || (cmd !== "status" && cmd !== "offline")) return;
  e.waitUntil(
    (async () => {
      if (cmd === "offline") {
        const prefs = await caches.open(PREFS);
        if (on) {
          await prefs.put(PREF_OFFLINE, new Response("1"));
          await saveOffline(false);
        } else {
          await prefs.delete(PREF_OFFLINE);
          await caches.delete(OFFLINE);
        }
      }
      reply.postMessage(await status());
    })().catch((err) => reply.postMessage({ error: String(err?.message ?? err) })),
  );
});
