#!/usr/bin/env python3
#!/usr/bin/env python3
"""Plot a capture.py run replayed through lap.c (replay): a timeline of all
passes, and a zoom on each with the reported pass (plateau N), its plateau
span, and the N=0 pass and span for comparison. GPL-3.0-or-later.

Usage: plot.py DIR --enter E --exit X [--n N] [--title T]
Reads DIR/laps.csv, DIR/rep.txt (replay ... 0 N), DIR/trace.csv; writes
DIR/timeline.png and DIR/pass_detail.png, and prints a table of the passes."""
import argparse
import csv
import os
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--enter", type=int, required=True)
ap.add_argument("--exit", type=int, required=True)
ap.add_argument("--n", type=int, default=2)
ap.add_argument("--title", default="")
args = ap.parse_args()
ENTER, EXIT, N = args.enter, args.exit, args.n
os.chdir(args.dir)
SURF, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, AQUA, RAW = "#2a78d6", "#eb6834", "#1baf7a", "#c9c8c2"
PAT = r" (\d+)\(pk(\d+),peak-run (\d+)\.\.(\d+),plat (\d+)\.\.(\d+)\)"

raw = [(int(r["t_ms"]), int(r["rssi"])) for r in csv.DictReader(open("laps.csv"))]
t0 = raw[0][0]
flt = [(int(r["t_ms"]), int(r["level"])) for r in csv.DictReader(open("trace.csv"))]
rows = {}
for line in open("rep.txt"):
    n, rest = line.split(":", 1)
    rows[int(n[2:])] = [tuple(map(int, m)) for m in re.findall(PAT, rest)]
P0, P4 = rows[0], rows[N]
assert len(P0) == len(P4), "N=0 and N found different passes"
NP = len(P4)

plt.rcParams.update({"font.size": 9, "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "axes.titlecolor": INK, "figure.facecolor": SURF, "axes.facecolor": SURF})


def style(ax, thresholds=True):
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    if thresholds:
        for y in (ENTER, EXIT):
            ax.axhline(y, color=INK2, linewidth=0.6, linestyle=":")


# Timeline
fig, ax = plt.subplots(figsize=(18, 5))
style(ax)
rw = raw[::20]
ax.plot([(t - t0) / 1000 for t, _ in rw], [v for _, v in rw], color=RAW, linewidth=0.5,
        label="Raw level (every 20th update)")
ax.plot([(t - t0) / 1000 for t, _ in flt], [v for _, v in flt], color=BLUE, linewidth=1.4,
        label="Filtered (255 ms median)")
for i, (p, pk, *_r) in enumerate(P4):
    ax.axvline((p - t0) / 1000, color=AQUA, linewidth=1, alpha=0.7)
    ax.annotate(f"{i + 1}", ((p - t0) / 1000, pk), xytext=(0, 5), textcoords="offset points", ha="center",
                fontweight="bold", color=INK)
ax.plot([], [], color=AQUA, linewidth=1.4, label=f"Reported pass (N={N})")
ax.text(1, ENTER + 2, f"EnterAt {ENTER}", color=INK2, fontsize=8)
ax.text(1, EXIT - 9, f"ExitAt {EXIT}", color=INK2, fontsize=8)
ax.set_xlim(0, (raw[-1][0] - t0) / 1000)
ax.set_ylim(0, 200)
ax.set_xlabel("Capture time (s)")
ax.set_ylabel("Level code (0.25 dB)")
ax.set_title(f"{args.title + ', ' if args.title else ''}{(raw[-1][0] - t0) / 1000:.0f} s, {NP} passes, "
             f"replayed through lap.c at PLATEAU {N} (EnterAt {ENTER}, ExitAt {EXIT})", loc="left",
             fontweight="bold")
ax.legend(loc="upper left", frameon=False, ncol=4)
fig.savefig("timeline.png", dpi=110, bbox_inches="tight")

# Per-pass zoom, x relative to the reported pass (plateau N)
W = 600
cols = 4
rws = (NP + cols - 1) // cols
fig, axs = plt.subplots(rws, cols, figsize=(18, 3.3 * rws + 2.0), sharex=True)
fig.subplots_adjust(hspace=0.5, wspace=0.16, top=1 - 1.75 / (3.3 * rws + 2.0))
for i, ax in enumerate(axs.flat):
    if i >= NP:
        ax.set_visible(False)
        continue
    p4, pk, _, _, pf, pl = P4[i]
    p0, _, f0, l0, _, _ = P0[i]
    style(ax, thresholds=False)
    ax.axvspan(pf - p4, pl - p4, color=AQUA, alpha=0.15, linewidth=0)
    ax.axvspan(f0 - p4, l0 - p4, color=ORANGE, alpha=0.15, linewidth=0)
    rr = [(t - p4, v) for t, v in raw if abs(t - p4) <= W]
    ax.plot([x for x, _ in rr], [v for _, v in rr], color=RAW, linewidth=0.4)
    z = [(t - p4, v) for t, v in flt if abs(t - p4) <= W]
    ax.step([x for x, _ in z], [v for _, v in z], where="post", color=BLUE, linewidth=1.6)
    ax.axhline(pk, color=ORANGE, linewidth=0.8, linestyle=":")
    ax.axhline(pk - N, color=AQUA, linewidth=0.8, linestyle=":")
    ax.axvline(p0 - p4, color=ORANGE, linewidth=1.4, linestyle="--")
    ax.axvline(0, color=AQUA, linewidth=1.8)
    ax.set_xlim(-W, W)
    ax.set_ylim(pk - 30, pk + 12)
    ax.set_title(f"Pass {i + 1} ({(p4 - t0) / 1000:.1f} s): peak {pk}, N=0 at {p0 - p4:+d} ms\n"
                 f"N={N} span {pl - pf} ms, N=0 span {l0 - f0} ms", loc="left", fontsize=9, fontweight="bold")
    if i // cols == rws - 1 or i + cols >= NP:
        ax.set_xlabel(f"ms from the reported pass (N={N})")
        ax.xaxis.set_tick_params(labelbottom=True)
    if i % cols == 0:
        ax.set_ylabel("Level code (0.25 dB)")
fig.suptitle("Pass detail: filtered level (step line), +/-600 ms around the reported pass", x=0.125,
             y=1 - 0.1 / (3.3 * rws + 2.0), ha="left", fontweight="bold", fontsize=12)
handles = [plt.Line2D([], [], color=RAW, linewidth=1.2, label="Raw level"),
           plt.Line2D([], [], color=BLUE, linewidth=1.6, label="Filtered level (what lap.c uses)"),
           plt.Line2D([], [], color=AQUA, linewidth=1.8, label=f"Reported pass (N={N})"),
           plt.Line2D([], [], color=ORANGE, linestyle="--", linewidth=1.4, label="Pass at N=0 (old rule)"),
           plt.Rectangle((0, 0), 1, 1, color=AQUA, alpha=0.3, label=f"N={N} span: first..last update >= peak-{N}"),
           plt.Rectangle((0, 0), 1, 1, color=ORANGE, alpha=0.3, label="N=0 span: first..last update at the peak"),
           plt.Line2D([], [], color=ORANGE, linestyle=":", linewidth=1, label="Peak level"),
           plt.Line2D([], [], color=AQUA, linestyle=":", linewidth=1, label=f"Peak - {N} ({N / 4:g} dB)")]
fig.legend(handles=handles, loc="upper left", bbox_to_anchor=(0.125, 1 - 0.4 / (3.3 * rws + 2.0)), ncol=4,
           frameon=False)
fig.savefig("pass_detail.png", dpi=100, bbox_inches="tight")

print(f"pass  t_s    peak  N0-N{N}_ms  N{N}_span  N0_span  lap_N{N}  lap_N0")
for i in range(NP):
    p4, pk, _, _, pf, pl = P4[i]
    p0, _, f0, l0, _, _ = P0[i]
    l4 = f"{(p4 - P4[i - 1][0]) / 1000:.3f}" if i else "-"
    lz = f"{(p0 - P0[i - 1][0]) / 1000:.3f}" if i else "-"
    print(f"{i + 1:4d} {(p4 - t0) / 1000:6.1f} {pk:5d} {p0 - p4:+9d} {pl - pf:8d} {l0 - f0:8d} {l4:>7} {lz:>7}")
