// "Flash device" dialog: a short walkthrough and the flash progress. GPL-3.0-or-later.
import { el } from "../util/canvas";
import { flashFirmware, loadManifest, type Manifest } from "../serial/flasher";
import type { Port } from "../serial/device";

export interface FlashHost {
  /** The port to flash: the connected board (closed first) or one the user picks. */
  takePort(): Promise<Port>;
  /** Called when flashing ends; ok means the board is booting the new firmware. */
  finished(ok: boolean): void;
}

export class FlashDialog {
  readonly el: HTMLDialogElement;
  private versionText = el("span", {}, "…");
  private bar = el("progress", { max: "1", value: "0" }) as HTMLProgressElement;
  private status = el("div", { class: "flash-status" });
  private logPre = el("pre", { class: "flash-log" });
  private flashBtn = el("button", { class: "primary", autofocus: "" }, "Flash");
  private closeBtn = el("button", {}, "Close");
  private manifest: Promise<Manifest> | null = null;
  private busy = false;

  constructor(private host: FlashHost) {
    this.el = el(
      "dialog",
      { class: "flash-dialog" },
      el("h2", {}, "Flash firmware"),
      el("p", {}, "Installs esp-rx5808 ", this.versionText, " on an ESP32-C5 board."),
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
      el("div", { class: "flash-progress" }, this.bar, this.status),
      el("details", {}, el("summary", {}, "Details"), this.logPre),
      el("div", { class: "flash-buttons" }, this.closeBtn, this.flashBtn),
    );
    this.flashBtn.onclick = () => void this.flash();
    this.closeBtn.onclick = () => this.el.close();
    // Esc must not hide a flash in progress.
    this.el.addEventListener("cancel", (e) => {
      if (this.busy) e.preventDefault();
    });
  }

  open() {
    this.manifest ??= loadManifest();
    this.manifest.then(
      (m) => {
        this.versionText.textContent = m.version;
        this.flashBtn.disabled = this.busy;
      },
      (e: Error) => {
        this.versionText.textContent = "(unavailable)";
        this.setStatus(`Cannot flash: ${e.message}.`, "err");
        this.flashBtn.disabled = true;
      },
    );
    if (!this.busy) {
      this.bar.value = 0;
      this.setStatus("");
      this.logPre.textContent = "";
    }
    this.el.showModal();
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
    const manifest = await this.manifest!;
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
      this.log(`!! ${(e as Error).message}`);
      this.setStatus(`Flashing failed: ${(e as Error).message}. Do step 3, then click Flash again.`, "err");
    } finally {
      this.busy = false;
      this.flashBtn.disabled = this.closeBtn.disabled = false;
      this.host.finished(ok);
    }
  }
}
