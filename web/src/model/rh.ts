// RotorHazard mode: polls the device as RotorHazard's server does
// (RHInterface.update: per seat, select it, read the pass stats and one
// queued peak/nadir), and turns that into levels, crossings and passes for
// the views. Seat frequencies come from the shared channel plan; thresholds
// are kept per metric in localStorage and written on every start, as the
// server writes its profile's. GPL-3.0-or-later.
import type { Device } from "../serial/device";
import type { RhLink } from "../serial/rhLink";
import { Series, type Store } from "./store";
import { RH, RH_API_MARKER, RH_FREQ_OFF, parseExtremums, parsePassStats, u16, validLevel, type RhMetric } from "../proto/rh";
import { channelMhz } from "./channelPlan";

export const RH_SEATS = 8;
const POLL_MS = 100; // RotorHazard's RH_UPDATE_INTERVAL
const HISTORY_SECONDS = 120;

export interface RhSeat {
  index: number;
  channel: string | null; // null: seat off
  mhz: number;
  enter: number;
  exit: number;
  level: number | null;
  nodePeak: number | null;
  nodeNadir: number | null;
  passPeak: number | null;
  crossing: boolean;
  lapId: number | null; // the device's count; null until first read
  laps: number; // passes seen since the last clear
  lastPass: number | null; // store.now() seconds
  lastLapS: number | null; // previous pass to the last one
  loopUs: number;
  history: Series; // polled levels (store.now(), code)
  peaks: { t: number; v: number; peak: boolean }[]; // queued extremums from the device
}

export interface RhEvent {
  t: number; // store.now() seconds
  seat: number;
  kind: "enter" | "exit" | "pass";
  level?: number;
  peak?: number;
  lapS?: number;
}

interface Thresholds {
  enter: number[];
  exit: number[];
}
const DEFAULTS: Record<RhMetric, Thresholds> = {
  RSSI: { enter: Array(RH_SEATS).fill(190), exit: Array(RH_SEATS).fill(180) }, // about -32.5 / -37.5 dBFS
  SNR: { enter: Array(RH_SEATS).fill(40), exit: Array(RH_SEATS).fill(30) }, // 20 / 15 dB
};
const KEY = "widscan58.rh";

export class RhModel {
  seats: RhSeat[] = [];
  events: RhEvent[] = [];
  metric: RhMetric = "RSSI";
  /** Device configuration in this mode (CONFIG? reply). */
  config: Record<string, unknown> | null = null;
  running = false;
  error: string | null = null;
  cycleMs = 0;
  private thresholds: Record<RhMetric, Thresholds> = structuredClone(DEFAULTS);
  private link: RhLink | null = null;
  private loop: Promise<void> | null = null;
  private stopFlag = false;
  private deviceMs = -1;

  constructor(private store: Store) {
    try {
      const saved = JSON.parse(localStorage.getItem(KEY) ?? "null");
      if (saved?.thresholds) Object.assign(this.thresholds, saved.thresholds);
      if (saved?.metric === "SNR") this.metric = "SNR";
    } catch {
      /* storage unavailable or corrupt */
    }
    for (let j = 0; j < RH_SEATS; j++)
      this.seats.push({
        index: j, channel: null, mhz: 0, enter: 0, exit: 0, level: null, nodePeak: null, nodeNadir: null,
        passPeak: null, crossing: false, lapId: null, laps: 0, lastPass: null, lastLapS: null, loopUs: 0,
        history: new Series(HISTORY_SECONDS * 15), peaks: [],
      });
    this.loadThresholds();
  }

  private save() {
    try {
      localStorage.setItem(KEY, JSON.stringify({ thresholds: this.thresholds, metric: this.metric }));
    } catch {
      /* storage unavailable */
    }
  }

  private loadThresholds() {
    const th = this.thresholds[this.metric];
    for (const s of this.seats) {
      s.enter = th.enter[s.index] ?? DEFAULTS[this.metric].enter[0];
      s.exit = th.exit[s.index] ?? DEFAULTS[this.metric].exit[0];
    }
  }

  /** Start on a device already switched to PROTOCOL RH. */
  async start(device: Device, channels: string[]) {
    await this.stop();
    if (!device.rh) throw new Error("device is not in RotorHazard mode");
    this.link = device.rh;
    this.error = null;
    this.stopFlag = false;
    this.deviceMs = -1;
    for (const s of this.seats) {
      s.lapId = null;
      s.history.clear();
      s.peaks = [];
      s.level = s.nodePeak = s.nodeNadir = s.passPeak = null;
    }
    try {
      const rev = await this.link.read(RH.READ_REVISION_CODE, 2);
      if (rev[0] !== RH_API_MARKER) throw new Error(`unexpected revision code ${rev[0]}.${rev[1]}`);
      const count = (await this.link.read(RH.READ_MULTINODE_COUNT, 1))[0];
      if (count < RH_SEATS) throw new Error(`device reports ${count} seats`);
      await this.text(`METRICS ${this.metric}`);
      await this.refreshConfig();
      await this.writeSeats(channels);
    } catch (e) {
      this.error = (e as Error).message;
      this.store.addLog("error", `RotorHazard start: ${this.error}`);
      this.store.touch();
      return;
    }
    this.running = true;
    this.loop = this.run();
  }

  async stop() {
    this.stopFlag = true;
    await this.loop;
    this.loop = null;
    this.running = false;
    this.link = null;
  }

  private async text(line: string): Promise<string> {
    this.store.addLog("tx", line);
    const reply = await this.link!.text(line);
    this.store.addLog(reply.startsWith("ERR") ? "error" : "rx", reply.length > 300 ? reply.slice(0, 300) + " …" : reply);
    if (reply.startsWith("ERR")) throw new Error(`${line}: ${reply}`);
    return reply;
  }

  async refreshConfig() {
    const reply = await this.text("CONFIG?");
    this.config = JSON.parse(reply.slice(reply.indexOf(" ") + 1));
    this.store.touch();
  }

  /** Our command lines allowed in this mode (rh_allowed in commands.c). */
  async command(line: string) {
    if (!this.link) return;
    try {
      await this.text(line);
      await this.refreshConfig();
    } catch (e) {
      this.store.addLog("error", (e as Error).message);
    }
  }

  /** Seat frequencies from the channel plan (in order); the rest off. Thresholds too. */
  async writeSeats(channels: string[]) {
    const link = this.link!;
    for (const s of this.seats) {
      const name = channels[s.index] ?? null;
      const mhz = name ? channelMhz(name) ?? 0 : 0;
      s.channel = mhz ? name : null;
      s.mhz = mhz;
      await link.write(RH.WRITE_CURNODE_INDEX, [s.index]);
      await link.write(RH.WRITE_FREQUENCY, u16(mhz || RH_FREQ_OFF));
      await link.write(RH.WRITE_ENTER_AT_LEVEL, [s.enter]);
      await link.write(RH.WRITE_EXIT_AT_LEVEL, [s.exit]);
      const f = await link.read(RH.READ_FREQUENCY, 2);
      if (((f[0] << 8) | f[1]) !== (mhz || RH_FREQ_OFF)) this.store.addLog("error", `seat ${s.index + 1}: frequency not set`);
      if (mhz && !(await link.read(RH.TEST_RX_REGISTER, 1))[0])
        this.store.addLog("error", `seat ${s.index + 1}: ${mhz} MHz is outside this board's band`);
    }
    this.store.touch();
  }

  async setThreshold(seat: number, which: "enter" | "exit", value: number) {
    const v = Math.max(1, Math.min(254, Math.round(value)));
    const s = this.seats[seat];
    s[which] = v;
    this.thresholds[this.metric][which][seat] = v;
    this.save();
    if (!this.link) return;
    await this.link.write(RH.WRITE_CURNODE_INDEX, [seat]);
    await this.link.write(which === "enter" ? RH.WRITE_ENTER_AT_LEVEL : RH.WRITE_EXIT_AT_LEVEL, [v]);
    this.store.touch();
  }

  /** Average of the seat's polled levels over the last `seconds` (for "set from current"). */
  recentLevel(seat: number, seconds = 1): number | null {
    const now = this.store.now();
    let sum = 0;
    let n = 0;
    this.seats[seat].history.forEach((t, v) => {
      if (now - t <= seconds) {
        sum += v;
        n++;
      }
    });
    return n ? sum / n : null;
  }

  async setMetric(m: RhMetric) {
    if (m === this.metric) return;
    this.metric = m;
    this.save();
    this.loadThresholds();
    for (const s of this.seats) {
      s.history.clear();
      s.peaks = [];
    }
    if (!this.link) return;
    await this.command(`METRICS ${m}`);
    for (const s of this.seats) {
      await this.link.write(RH.WRITE_CURNODE_INDEX, [s.index]);
      await this.link.write(RH.WRITE_ENTER_AT_LEVEL, [s.enter]);
      await this.link.write(RH.WRITE_EXIT_AT_LEVEL, [s.exit]);
    }
  }

  clearEvents() {
    this.events = [];
    for (const s of this.seats) {
      s.laps = 0;
      s.lastPass = s.lastLapS = null;
    }
    this.store.touch();
  }

  private addEvent(e: RhEvent) {
    this.events.push(e);
    if (this.events.length > 500) this.events.splice(0, this.events.length - 500);
  }

  private async run() {
    const link = this.link!;
    while (!this.stopFlag && !link.closed) {
      const start = performance.now();
      try {
        await this.pollOnce(link);
        this.error = null;
      } catch (e) {
        this.error = (e as Error).message;
      }
      this.cycleMs = performance.now() - start;
      this.store.touch();
      const wait = POLL_MS - this.cycleMs;
      if (wait > 0) await new Promise((r) => setTimeout(r, wait));
    }
    this.running = false;
  }

  private async pollOnce(link: RhLink) {
    // A device that restarted has forgotten the seats: set them up again.
    const t = await link.read(RH.READ_TIME_MILLIS, 4);
    const ms = ((t[0] << 24) | (t[1] << 16) | (t[2] << 8) | t[3]) >>> 0;
    if (this.deviceMs >= 0 && ms < this.deviceMs) {
      this.store.addLog("info", "RotorHazard: device restarted; setting the seats up again");
      await this.text(`METRICS ${this.metric}`);
      await this.writeSeats(this.seats.map((s) => s.channel ?? ""));
      for (const s of this.seats) s.lapId = null;
    }
    this.deviceMs = ms;
    for (const s of this.seats) {
      if (!s.channel) continue;
      await link.write(RH.WRITE_CURNODE_INDEX, [s.index]);
      const ps = parsePassStats(await link.read(RH.READ_LAP_PASS_STATS, 8));
      // RotorHazard's read time: the midpoint of request and response.
      const readTime = this.store.now() - (link.lastResponse - link.lastRequest) / 2000;
      const ex = parseExtremums(await link.read(RH.READ_LAP_EXTREMUMS, 8));
      this.update(s, readTime, ps, ex);
    }
  }

  private update(s: RhSeat, now: number, ps: ReturnType<typeof parsePassStats>, ex: ReturnType<typeof parseExtremums>) {
    if (validLevel(ps.level)) {
      s.level = ps.level;
      s.history.push(now, ps.level);
    }
    s.nodePeak = validLevel(ps.nodePeak) ? ps.nodePeak : null;
    s.nodeNadir = validLevel(ex.nodeNadir) ? ex.nodeNadir : null;
    s.loopUs = ps.loopUs;
    if (ex.extremum) {
      s.peaks.push({ t: now - ex.extremum.msAgo / 1000 + ex.extremum.durationMs / 2000, v: ex.extremum.level, peak: ex.extremum.peak });
      while (s.peaks.length && now - s.peaks[0].t > HISTORY_SECONDS) s.peaks.shift();
    }
    if (ex.crossing !== s.crossing) {
      s.crossing = ex.crossing;
      if (s.lapId !== null) this.addEvent({ t: now, seat: s.index, kind: ex.crossing ? "enter" : "exit", level: ps.level });
    }
    if (s.lapId !== null && ps.lapId !== s.lapId) {
      const t = now - ps.msSinceLap / 1000;
      const lapS = s.lastPass !== null ? t - s.lastPass : undefined;
      s.lastLapS = lapS ?? null;
      s.lastPass = t;
      s.passPeak = validLevel(ps.passPeak) ? ps.passPeak : null;
      s.laps++;
      this.addEvent({ t, seat: s.index, kind: "pass", peak: ps.passPeak, lapS });
    }
    s.lapId = ps.lapId;
  }
}
