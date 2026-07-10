#!/usr/bin/env python3
"""
Render benchmark charts from results/results.json (+ results_browser.csv).

Outputs (results/):
  chart_fps_<profile>.png  encode fps per contender (log scale)
  chart_scaling.png        our library's fps vs worker count

Plain working charts; restyle later.
"""
import csv
import json
import os
import tempfile

os.environ.setdefault("MPLCONFIGDIR", os.path.join(tempfile.gettempdir(), "mpl-cache"))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results")

with open(os.path.join(RESULTS, "results.json")) as f:
    data = json.load(f)
meta = data["meta"]
rows = list(data["results"])

browser_csv = os.path.join(RESULTS, "results_browser.csv")
if os.path.exists(browser_csv):
    with open(browser_csv) as f:
        for r in csv.DictReader(f):
            r["fps"] = float(r["fps"])
            r["realtimeX"] = float(r["realtimeX"])
            rows.append(r)

BLUE, GRAY, ORANGE1, ORANGE2, GREEN = "#2f7de1", "#9aa0a6", "#e8a33d", "#d9642c", "#4caf50"

# Each contender: (contender, threads, environment) -> label, colour.
ORDER = [
    ("ffmpeg.wasm", "1t", "wasm", "ffmpeg.wasm\n1 thread", GRAY),
    ("prores-wasm (ours)", "1t", "node", "ours\n1 thread", BLUE),
    ("FFmpeg (native)", "sw-1core", "native", "FFmpeg native\nsoftware, 1 core", ORANGE1),
    ("ffmpeg.wasm", "8t", "browser", "ffmpeg.wasm\n8 threads", GRAY),
    ("prores-wasm (ours)", "2t", "node", "ours\n2 threads", BLUE),
    ("prores-wasm (ours)", "4t", "node", "ours\n4 threads", BLUE),
    ("prores-wasm (ours)", "8t", "node", "ours\n8 threads", BLUE),
    ("FFmpeg (native)", "sw-allcores", "native", "FFmpeg native\nsoftware, all cores", ORANGE2),
    ("FFmpeg (native)", "hw-vt", "native", "FFmpeg native\nhardware (VT)", GREEN),
]


def env_of(r):
    e = r.get("environment", "")
    if e in ("native", "browser"):
        return e
    return "wasm" if r["contender"] == "ffmpeg.wasm" else "node"


def find(profile, contender, threads, env):
    for r in rows:
        if (r["profile"] == profile and r["contender"] == contender
                and r["threads"] == threads and env_of(r) == env):
            return r
    return None


PROFILES = [("422hq", "ProRes 422 HQ"), ("4444", "ProRes 4444 + alpha")]

# ---- 1) FPS bar chart per profile (log scale) ----
for pkey, plabel in PROFILES:
    bars = []
    for contender, threads, env, label, col in ORDER:
        r = find(pkey, contender, threads, env)
        if r:
            bars.append((label, r["fps"], col))
    if not bars:
        continue
    labels = [b[0] for b in bars]
    vals = [b[1] for b in bars]
    cols = [b[2] for b in bars]

    fig, ax = plt.subplots(figsize=(12.5, 5.4))
    xs = range(len(bars))
    ax.bar(xs, vals, color=cols)
    ax.set_yscale("log")
    ax.set_ylabel("encode speed (frames / second, log scale)")
    ax.set_title(f"{plabel} at {meta['width']}x{meta['height']}: encode throughput\n"
                 f"Big Buck Bunny, {meta['frames']} frames")
    ax.set_xticks(list(xs))
    ax.set_xticklabels(labels, rotation=0, ha="center", fontsize=8)
    ax.axhline(meta["fps"], color="#c0392b", ls="--", lw=1)
    ax.text(len(bars) - 0.4, meta["fps"] * 1.05, f"{meta['fps']} fps (real time)", color="#c0392b",
            ha="right", va="bottom", fontsize=8)
    for x, v in zip(xs, vals):
        ax.text(x, v * 1.05, f"{v:g}", ha="center", va="bottom", fontsize=8)
    ax.grid(axis="y", which="both", ls=":", alpha=0.4)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, f"chart_fps_{pkey}.png"), dpi=130)
    plt.close(fig)
    print(f"wrote chart_fps_{pkey}.png")

# ---- 2) Our-library scaling: fps vs worker count ----
fig, ax = plt.subplots(figsize=(8, 5))
for pkey, plabel in PROFILES:
    pts = []
    s1 = find(pkey, "prores-wasm (ours)", "1t", "node")
    if s1:
        pts.append((1, s1["fps"]))
    for w in (2, 4, 8):
        r = find(pkey, "prores-wasm (ours)", f"{w}t", "node")
        if r:
            pts.append((w, r["fps"]))
    if pts:
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ax.plot(xs, ys, "-o", label=plabel)
        for x, y in zip(xs, ys):
            ax.text(x, y, f" {y:g}", va="center", fontsize=8)
ax.set_xlabel("worker threads")
ax.set_ylabel("encode speed (fps)")
ax.set_title("prores-wasm (ours): frame-parallel scaling")
ax.set_xticks([1, 2, 4, 8])
ax.grid(ls=":", alpha=0.4)
ax.legend()
fig.tight_layout()
fig.savefig(os.path.join(RESULTS, "chart_scaling.png"), dpi=130)
plt.close(fig)
print("wrote chart_scaling.png")
print("done")
