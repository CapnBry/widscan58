// Screen wake lock (navigator.wakeLock) held while wanted. The browser drops
// it when the page is hidden, so it is taken again when the page is visible.
// GPL-3.0-or-later.

export class ScreenWake {
  static supported(): boolean {
    return typeof navigator !== "undefined" && "wakeLock" in navigator;
  }

  private wanted = false;
  private lock: WakeLockSentinel | null = null;
  private pending = false;

  constructor(private onError: (msg: string) => void) {
    if (typeof document !== "undefined") document.addEventListener("visibilitychange", () => void this.sync());
  }

  /** Cheap when unchanged: called on every store update. */
  set(wanted: boolean) {
    if (wanted === this.wanted) return;
    this.wanted = wanted;
    void this.sync();
  }

  private async sync() {
    if (!ScreenWake.supported() || this.pending) return;
    if (this.wanted && !this.lock && document.visibilityState === "visible") {
      this.pending = true;
      try {
        const lock = await navigator.wakeLock.request("screen");
        lock.addEventListener("release", () => {
          if (this.lock === lock) this.lock = null;
        });
        this.lock = lock;
      } catch (e) {
        this.onError(`keep screen on: ${(e as Error).message}`);
      } finally {
        this.pending = false;
      }
    }
    if (!this.wanted && this.lock) {
      const lock = this.lock;
      this.lock = null;
      await lock.release().catch(() => {});
    }
  }
}
