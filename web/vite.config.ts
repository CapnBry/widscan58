import { createHash } from "node:crypto";
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join, relative, resolve } from "node:path";
import { defineConfig, type Plugin } from "vite";

// Writes dist/sw.js from sw.js with the build's files and a cache name from
// their hash. FILES is the page itself: the public files and the entry's static
// imports. FLASH_FILES is the rest: the firmware and the code only the flasher
// loads (dynamic imports), fetched when flashing, or with the page when the
// installed app keeps the firmware offline. Build only: the dev server has no
// service worker.
function pwaPlugin(): Plugin {
  let root = ".";
  let outDir = "dist";
  const shell = new Set<string>(); // the entry chunks' static imports and their CSS
  return {
    name: "widscan58-sw",
    apply: "build",
    configResolved(c) {
      root = c.root;
      outDir = resolve(c.root, c.build.outDir);
    },
    generateBundle(_, bundle) {
      const add = (name: string) => {
        const c = bundle[name];
        if (!c || shell.has(name)) return;
        shell.add(name);
        if (c.type !== "chunk") return;
        c.imports.forEach(add);
        c.viteMetadata?.importedCss.forEach((f) => shell.add(f));
        c.viteMetadata?.importedAssets.forEach((f) => shell.add(f));
      };
      for (const c of Object.values(bundle)) if (c.type === "chunk" && c.isEntry) add(c.fileName);
    },
    closeBundle() {
      const walk = (d: string): string[] =>
        readdirSync(d, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(join(d, e.name)) : [join(d, e.name)]));
      const all = walk(outDir)
        .map((f) => relative(outDir, f).split("\\").join("/"))
        .filter((f) => f !== "sw.js")
        .sort();
      const isPage = (f: string) => !f.startsWith("firmware/") && (!f.startsWith("assets/") || shell.has(f));
      const hash = createHash("sha256");
      for (const f of all) hash.update(f).update(readFileSync(join(outDir, f)));
      const head =
        `const CACHE = "widscan58-${hash.digest("hex").slice(0, 16)}";\n` +
        `const FILES = ${JSON.stringify(all.filter(isPage))};\n` +
        `const FLASH_FILES = ${JSON.stringify(all.filter((f) => !isPage(f)))};\n`;
      writeFileSync(join(outDir, "sw.js"), head + readFileSync(join(root, "sw.js"), "utf8"));
    },
  };
}

// Relative base so the static build can be opened from any path (GitHub Pages, file host).
export default defineConfig({ base: "./", plugins: [pwaPlugin()] });
