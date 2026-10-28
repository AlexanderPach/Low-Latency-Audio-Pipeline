#!/usr/bin/env python3
"""Optional shareable figure from retained raw trials (requires matplotlib)."""

import argparse
import csv
import os
from pathlib import Path
import statistics
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    cache = Path(tempfile.gettempdir()) / "llap-plot-cache"
    cache.mkdir(exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(cache / "matplotlib"))
    os.environ.setdefault("XDG_CACHE_HOME", str(cache))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        parser.error("matplotlib is optional; install it to generate this figure")

    with (args.directory / "queue.csv").open(newline="") as source:
        queue = [row for row in csv.DictReader(source) if row["phase"] == "measure"]
    with (args.directory / "simulation.csv").open(newline="") as source:
        simulation = list(csv.DictReader(source))

    panels = [
        (queue, "saturated_transfer", "units_per_second", 1e-6,
         "Saturated float transfers", "Million items / second"),
        (queue, "sparse_handoff", "p99_ns", 1e-3,
         "Instrumented active handoff: p99", "Microseconds"),
        (simulation, None, "age_p50_ns", 1e-6,
         "Paced simulation: median sample age", "Milliseconds (log scale)"),
    ]
    colors = {"spsc": "#2563eb", "mutex": "#d97706"}
    labels = {"spsc": "SPSC", "mutex": "Mutex ring"}
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.7))
    for axis, (rows, mode, metric, scale, title, ylabel) in zip(axes, panels):
        for candidate, offset in (("spsc", -0.19), ("mutex", 0.19)):
            positions, medians, lows, highs = [], [], [], []
            for x, capacity in enumerate((256, 65536)):
                values = [float(row[metric]) * scale for row in rows
                          if row["queue"] == candidate and int(row["capacity"]) == capacity
                          and (mode is None or row["mode"] == mode)]
                if not values:
                    parser.error("incomplete data; run summarize.py to validate first")
                median = statistics.median(values)
                positions.append(x + offset)
                medians.append(median)
                lows.append(median - min(values))
                highs.append(max(values) - median)
            axis.bar(positions, medians, width=0.34, color=colors[candidate], label=labels[candidate],
                     yerr=[lows, highs], capsize=4, error_kw={"linewidth": 1})
            for x, value in zip(positions, medians):
                axis.annotate(f"{value:.2f}", (x, value), xytext=(0, 9), textcoords="offset points",
                              ha="center", fontsize=8)
        axis.set_xticks([0, 1], ["256", "65,536"])
        axis.set_xlabel("Queue capacity (entries)")
        axis.set_ylabel(ylabel)
        axis.set_title(title, fontsize=10)
        axis.spines[["top", "right"]].set_visible(False)
        axis.grid(axis="y", alpha=0.2)
        axis.set_axisbelow(True)
    axes[0].legend(frameon=False)
    axes[2].set_yscale("log")
    lower, upper = axes[2].get_ylim()
    axes[2].set_ylim(bottom=max(0.1, min(1, lower)), top=upper * 1.8)
    fig.suptitle("Queue speed and buffered sample age are different measurements", fontsize=14, y=0.99)
    fig.text(0.5, 0.02,
             "Bars: medians of trial statistics; whiskers: trial min/max. Unpinned threads; see machine metadata.\n"
             "Timestamped handoff and paced consumption are instrumented experiments, not speaker latency.",
             ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.1, 1, 0.94))
    for extension in ("svg", "png"):
        path = args.directory / ("comparison." + extension)
        fig.savefig(path, dpi=180)
        if extension == "svg":
            # Matplotlib adds spaces at path-data line endings; keep exports
            # friendly to repository whitespace checks without changing geometry.
            path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines()) + "\n")
        print(path)


if __name__ == "__main__":
    main()
