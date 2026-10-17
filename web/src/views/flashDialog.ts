// "Flash device" dialog: a short walkthrough and the flash progress. GPL-3.0-or-later.
import { el } from "../util/canvas";
import { OfflineError, flashFirmware, loadManifest, type Manifest } from "../serial/flasher";
import { isInstalledApp, offlineStatus, setOfflineFirmware, type OfflineStatus } from "../util/pwa";
import type { Port } from "../serial/device";
import type { DeviceInfo } from "../model/store";

export interface FlashHost {
  /** The port to flash: the connected board (closed first) or one the user picks. */
  takePort(): Promise<Port>;
  /** Called when flashing ends; ok means the board is booting the new firmware. */
  finished(ok: boolean): void;
  /** The connected board's INFO, or null when no board is connected over Web Serial. */
  device(): DeviceInfo | null;
}

export class FlashDialog {
  readonly el: HTMLDialogElement;
  private boardVersion = el("td", {}, "…");
  private boardDate = el("td", {}, "");
  private newVersion = el("td", {}, "…");
  private newDate = el("td", {}, "");
  private bar = el("progress", { max: "1", value: "0" }) as HTMLProgressElement;
  private status = el("div", { class: "flash-status" });
  private logPre = el("pre", { class: "flash-log" });
  private flashBtn = el("button", { class: "primary", autofocus: "" }, "Flash");
  private closeBtn = el("button", {}, "Close");
  // "Make firmware available offline": installed app only (util/pwa.ts).
  private offlineBox = el("input", { type: "checkbox" }) as HTMLInputElement;
  private offlineHint = el("small", {});
  private offlineRow = el(
    "label",
    { class: "option", hidden: "" },
    this.offlineBox,
    el("span", {}, "Make firmware available offline", this.offlineHint),
  );
  private manifest: Promise<Manifest> | null = null;
  private busy = false;
  /** Offline, and the firmware is not on this device. */
  private noFirmware = false;

  constructor(private host: FlashHost) {
    this.el = el(
      "dialog",
      { class: "flash-dialog" },
      el("h2", {}, "Flash firmware"),
      el(
        "table",
        { class: "flash-versions" },
        el("thead", {}, el("tr", {}, el("th", {}), el("th", {}, "Version"), el("th", {}, "Committed"))),
        el(
          "tbody",
          {},
          el("tr", {}, el("th", {}, "On the board"), this.boardVersion, this.boardDate),
          el("tr", {}, el("th", {}, "To install"), this.newVersion, this.newDate),
        ),
      ),
      el(
        "ol",
        {},
        el("li", {}, "Use Chrome or Edge on a computer, or Chrome on Android with a USB OTG adapter."),
        el("li", {}, "Plug the board in with a USB data cable (some cables only carry power)."),
        el(
          "li",
          {},
          el("strong", {}, "New board, or one that will not connect: "),
          "hold BOOT, press and release RESET, then release BOOT. The board is now waiting to be flashed. " +
            "A board that already runs this firmware is switched over automatically.",
        ),
        el("li", {}, "Click Flash and pick the board (“USB JTAG/serial debug unit”) in the browser’s list."),
      ),
      this.offlineRow,
      el("div", { class: "flash-progress" }, this.bar, this.status),
      el("details", {}, el("summary", {}, "Details"), this.logPre),
      el("div", { class: "flash-buttons" }, this.closeBtn, this.flashBtn),
    );
    this.flashBtn.onclick = () => void this.flash();
    this.closeBtn.onclick = () => this.el.close();
    this.offlineBox.onchange = () => void this.toggleOffline();
    const recheck = () => {
      if (this.el.open && !this.busy) void this.refreshOffline();
    };
    window.addEventListener("online", recheck);
    window.addEventListener("offline", recheck);
    // Esc must not hide a flash in progress.
    this.el.addEventListener("cancel", (e) => {
      if (this.busy) e.preventDefault();
    });
  }

  open() {
    // A fresh manifest on every open (and again at Flash): a page left open
    // across a deploy would otherwise offer the old build, whose files and
    // hashes are gone.
    if (!this.busy || !this.manifest) this.manifest = loadManifest();
    this.manifest.then(
      (m) => {
        this.showManifest(m);
        this.flashBtn.disabled = this.busy || this.noFirmware;
      },
      (e: Error) => {
        this.newVersion.textContent = "(unavailable)";
        this.newDate.textContent = "";
        this.setStatus(e instanceof OfflineError ? this.offlineText() : `Cannot flash: ${e.message}.`, "err");
        this.flashBtn.disabled = true;
      },
    );
    if (!this.busy) {
      this.bar.value = 0;
      this.setStatus("");
      this.logPre.textContent = "";
    }
    this.el.showModal();
    this.refreshDevice(); // after showModal: it skips a closed dialog
    if (!this.busy) void this.refreshOffline();
  }

  private offlineText(): string {
    return (
      "No internet connection, and the firmware is not on this device. Connect to the internet to flash." +
      (this.offlineRow.hidden ? "" : " To flash without internet, turn on Make firmware available offline while connected.")
    );
  }

  /** The offline option, and whether flashing can work without a network. */
  private async refreshOffline() {
    const st = await offlineStatus().catch(() => null);
    this.offlineRow.hidden = !st || !isInstalledApp();
    if (st) this.showOffline(st);
    const was = this.noFirmware;
    this.noFirmware = !navigator.onLine && !!st && !st.firmware;
    if (this.noFirmware) {
      this.setStatus(this.offlineText(), "err");
      this.flashBtn.disabled = true;
    } else if (was) {
      this.setStatus("");
      this.flashBtn.disabled = false;
    }
  }

  private showOffline(st: OfflineStatus) {
    this.offlineBox.checked = st.wanted;
    setText(
      this.offlineHint,
      !st.wanted
        ? "Downloads the firmware (about 1 MB) now and with every app update, so you can flash without internet."
        : st.saved
          ? `Saved on this device (${st.version ?? "unknown version"}); every app update brings the new firmware.`
          : "Not downloaded yet; it is tried again with the next app update.",
    );
  }

  private async toggleOffline() {
    const on = this.offlineBox.checked;
    this.offlineBox.disabled = true;
    if (on) setText(this.offlineHint, "Downloading the firmware…");
    try {
      const st = await setOfflineFirmware(on);
      if (st) this.showOffline(st);
    } catch (e) {
      setText(this.offlineHint, `Download failed (${(e as Error).message}); it is tried again with the next app update.`);
    } finally {
      this.offlineBox.disabled = false;
      void this.refreshOffline();
    }
  }

  /** Shows the connected board's version; kept while flashing, when the board
   *  is disconnected, until it reconnects with the new firmware. */
  refreshDevice() {
    if (!this.el.open || this.busy) return;
    const d = this.host.device();
    setText(this.boardVersion, d ? d.version : "not connected");
    setText(this.boardDate, d ? (d.date ?? "unknown") : "");
  }

  private showManifest(m: Manifest) {
    setText(this.newVersion, m.version);
    setText(this.newDate, m.date ?? "unknown");
  }

  /** The firmware version this page offers, once known. */
  async offeredVersion(): Promise<string | null> {
    this.manifest ??= loadManifest();
    return this.manifest.then(
      (m) => m.version,
      () => null,
    );
  }

  private setStatus(text: string, kind: "" | "ok" | "err" = "") {
    this.status.textContent = text;
    this.status.className = `flash-status ${kind}`;
  }

  private log(line: string) {
    this.logPre.textContent += line + "\n";
    this.logPre.scrollTop = this.logPre.scrollHeight;
  }

  private async flash() {
    // Fetched now but awaited after takePort: the browser's port chooser
    // needs this click's user activation, which a slow fetch could outlast.
    const fresh = (this.manifest = loadManifest());
    fresh.catch(() => {});
    let port: Port;
    try {
      port = await this.host.takePort();
    } catch (e) {
      // Dismissing the browser's chooser lands here too.
      this.setStatus(`No board selected (${(e as Error).message}).`, "err");
      return;
    }
    this.busy = true;
    this.flashBtn.disabled = this.closeBtn.disabled = true;
    this.bar.value = 0;
    this.logPre.textContent = "";
    let ok = false;
    try {
      const manifest = await fresh;
      this.showManifest(manifest);
      await flashFirmware(port, manifest, {
        stage: (t) => {
          this.setStatus(t);
          this.log(`-- ${t}`);
        },
        progress: (f) => (this.bar.value = f),
        log: (l) => this.log(l),
      });
      ok = true;
      this.setStatus(`Done: ${manifest.version} is installed and the board is restarting. The scanner reconnects on its own.`, "ok");
    } catch (e) {
      const msg = (e as Error).message;
      this.log(`!! ${msg}`);
      // esptool-js loads its chip code when it connects, which fails offline too.
      if (e instanceof OfflineError || (e instanceof TypeError && /fetch|import/i.test(msg))) {
        this.setStatus(
          "Flashing failed: the firmware could not be downloaded (no internet connection?). " +
            "Connect to the internet and click Flash again; press RESET on the board to start the firmware it has.",
          "err",
        );
      } else {
        this.setStatus(`Flashing failed: ${msg}. Do step 3, then click Flash again.`, "err");
      }
    } finally {
      this.busy = false;
      this.flashBtn.disabled = this.closeBtn.disabled = false;
      this.host.finished(ok);
    }
  }
}

function setText(e: HTMLElement, text: string) {
  if (e.textContent !== text) e.textContent = text;
}
