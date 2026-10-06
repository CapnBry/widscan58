// Canvas helpers: DPR-correct sizing, theme colours, axes. GPL-3.0-or-later.

export interface Theme {
  surface: string;
  surface2: string;
  ink: string;
  ink2: string;
  muted: string;
  grid: string;
  axis: string;
  accent: string;
  series: string[];
  good: string;
  warning: string;
  serious: string;
  critical: string;
  seq: string[];
}

let cached: Theme | null = null;
export function theme(): Theme {
  if (cached) return cached;
  const cs = getComputedStyle(document.documentElement);
  const v = (n: string) => cs.getPropertyValue(n).trim();
  cached = {
    surface: v("--surface"),
    surface2: v("--surface-2"),
    ink: v("--ink"),
    ink2: v("--ink-2"),
    muted: v("--muted"),
    grid: v("--grid"),
    axis: v("--axis"),
    accent: v("--accent"),
    series: [1, 2, 3, 4, 5, 6, 7, 8].map((j) => v(`--s${j}`)),
    good: v("--good"),
    warning: v("--warning"),
    serious: v("--serious"),
    critical: v("--critical"),
    seq: [0, 1, 2, 3, 4, 5, 6].map((j) => v(`--seq-${j}`)),
  };
  return cached;
}
export function invalidateTheme() {
  cached = null;
}

/** Channel colour: categorical slot by channel index, never cycled past 8. */
export function channelColor(id: number): string {
  const s = theme().series;
  return id < s.length ? s[id] : theme().muted;
}

export interface Surface {
  ctx: CanvasRenderingContext2D;
  w: number; // CSS pixels
  h: number;
}

/** Resize the backing store to the element size x DPR; returns a CSS-pixel context. */
export function prepare(canvas: HTMLCanvasElement): Surface {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  const bw = Math.max(1, Math.round(w * dpr));
  const bh = Math.max(1, Math.round(h * dpr));
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw;
    canvas.height = bh;
  }
  const ctx = canvas.getContext("2d")!;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

export function niceStep(range: number, target: number): number {
  const raw = range / Math.max(1, target);
  const mag = 10 ** Math.floor(Math.log10(raw));
  const n = raw / mag;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

export interface Axes {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
  left: number;
  right: number;
  top: number;
  bottom: number;
  sx: (v: number) => number;
  sy: (v: number) => number;
  ix: (px: number) => number;
}

export function axes(
  s: Surface,
  x0: number,
  x1: number,
  y0: number,
  y1: number,
  opts: { left?: number; bottom?: number; top?: number; right?: number } = {},
): Axes {
  const left = opts.left ?? 44;
  const right = s.w - (opts.right ?? 8);
  const top = opts.top ?? 8;
  const bottom = s.h - (opts.bottom ?? 20);
  return {
    x0,
    x1,
    y0,
    y1,
    left,
    right,
    top,
    bottom,
    sx: (v) => left + ((v - x0) / (x1 - x0)) * (right - left),
    sy: (v) => bottom - ((v - y0) / (y1 - y0)) * (bottom - top),
    ix: (px) => x0 + ((px - left) / (right - left)) * (x1 - x0),
  };
}

export function drawGrid(
  s: Surface,
  a: Axes,
  fmtX: (v: number) => string,
  fmtY: (v: number) => string,
  xTarget = 8,
  yTarget = 5,
) {
  const t = theme();
  const { ctx } = s;
  ctx.font = "10px system-ui, sans-serif";
  ctx.lineWidth = 1;
  const ys = niceStep(a.y1 - a.y0, yTarget);
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let v = Math.ceil(a.y0 / ys) * ys; v <= a.y1 + 1e-9; v += ys) {
    const y = Math.round(a.sy(v)) + 0.5;
    ctx.strokeStyle = t.grid;
    ctx.beginPath();
    ctx.moveTo(a.left, y);
    ctx.lineTo(a.right, y);
    ctx.stroke();
    ctx.fillStyle = t.muted;
    ctx.fillText(fmtY(v), a.left - 6, y);
  }
  const xs = niceStep(a.x1 - a.x0, xTarget);
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let v = Math.ceil(a.x0 / xs) * xs; v <= a.x1 + 1e-9; v += xs) {
    const x = Math.round(a.sx(v)) + 0.5;
    ctx.strokeStyle = t.grid;
    ctx.beginPath();
    ctx.moveTo(x, a.top);
    ctx.lineTo(x, a.bottom);
    ctx.stroke();
    ctx.fillStyle = t.muted;
    ctx.fillText(fmtX(v), x, a.bottom + 4);
  }
  ctx.strokeStyle = t.axis;
  ctx.beginPath();
  ctx.moveTo(a.left, a.bottom + 0.5);
  ctx.lineTo(a.right, a.bottom + 0.5);
  ctx.stroke();
}

export function hexToRgb(hex: string): [number, number, number] {
  const h = hex.replace("#", "");
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}

/** 256-entry RGB lookup over the sequential ramp, for the waterfall. */
export function seqLut(): Uint8ClampedArray {
  const stops = theme().seq.map(hexToRgb);
  const lut = new Uint8ClampedArray(256 * 3);
  for (let j = 0; j < 256; j++) {
    const x = (j / 255) * (stops.length - 1);
    const k = Math.min(stops.length - 2, Math.floor(x));
    const f = x - k;
    for (let c = 0; c < 3; c++) lut[j * 3 + c] = stops[k][c] + (stops[k + 1][c] - stops[k][c]) * f;
  }
  return lut;
}

export function fmtMHz(v: number, digits = 1): string {
  return v.toFixed(digits);
}

export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Record<string, string> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else e.setAttribute(k, v);
  }
  for (const c of children) e.append(c);
  return e;
}
