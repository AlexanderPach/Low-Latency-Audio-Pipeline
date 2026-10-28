#!/usr/bin/env python3
"""Summarize complete, non-smoke benchmark runs without pooling trial quantiles."""

import argparse
import csv
import json
from pathlib import Path
import statistics


def load_csv(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory
    metadata = json.loads((directory / "metadata.json").read_text())
    if metadata["quick_smoke_only"]:
        parser.error("a quick run is not reportable performance data")
    runs = metadata["runs"]
    if len(runs) != 2 or any(run.get("returncode") != 0 for run in runs):
        parser.error("both benchmark processes must have completed successfully")
    queue = [row for row in load_csv(directory / "queue.csv") if row["phase"] == "measure"]
    simulation = load_csv(directory / "simulation.csv")
    expected_queue_trials = int(runs[0]["command"][runs[0]["command"].index("--trials") + 1])
    expected_simulation_trials = int(runs[1]["command"][runs[1]["command"].index("--trials") + 1])
    summary = []

    def group(rows, kind, capacity, candidate, mode, expected_trials):
        selected = [row for row in rows if int(row["capacity"]) == capacity and row["queue"] == candidate
                    and (kind == "simulation" or row["mode"] == mode)]
        if (len(selected) != expected_trials or
                {int(row["trial"]) for row in selected} != set(range(1, expected_trials + 1))):
            parser.error(f"incomplete or duplicate trial grid: {kind}/{capacity}/{candidate}/{mode}")
        return selected

    def metric(rows, experiment, capacity, candidate, name, unit, scale=1):
        values = [float(row[name]) * scale for row in rows]
        value = statistics.median(values)
        summary.append(dict(experiment=experiment, capacity=capacity, queue=candidate, metric=name,
                            unit=unit, trials=len(values), median=value, minimum=min(values), maximum=max(values)))
        return f"{value:.3f} [{min(values):.3f}, {max(values):.3f}]"

    lines = ["# Trial summaries", "",
             "Each cell is the median [minimum, maximum] across trials. Latency cells summarize",
             "per-trial quantiles; they are not quantiles of pooled samples. Maxima are observed,",
             "not guaranteed bounds. Warmup rows are excluded.", "",
             "## Queue operation costs and throughput", "",
             "| Capacity | Queue | Trials | Serial pair (ns/pair) | Saturated transfer (million items/s) |",
             "| ---: | --- | ---: | ---: | ---: |"]
    for capacity in (256, 65536):
        for candidate in ("spsc", "mutex"):
            serial = group(queue, "queue", capacity, candidate, "serial_pair", expected_queue_trials)
            saturated = group(queue, "queue", capacity, candidate, "saturated_transfer", expected_queue_trials)
            pair = metric(serial, "serial_pair", capacity, candidate, "ns_per_unit", "ns/pair")
            rate = metric(saturated, "saturated_transfer", capacity, candidate, "units_per_second", "million items/s", 1e-6)
            lines.append(f"| {capacity} | {candidate} | {len(serial)} | {pair} | {rate} |")

    lines += ["", "## Instrumented sparse handoff", "",
              "| Capacity | Queue | Samples/trial | p50 (µs) | p95 (µs) | p99 (µs) | Largest observed (µs) |",
              "| ---: | --- | ---: | ---: | ---: | ---: | ---: |"]
    for capacity in (256, 65536):
        for candidate in ("spsc", "mutex"):
            rows = group(queue, "queue", capacity, candidate, "sparse_handoff", expected_queue_trials)
            cells = [metric(rows, "sparse_handoff", capacity, candidate, key, "us", 1e-3)
                     for key in ("p50_ns", "p95_ns", "p99_ns")]
            metric(rows, "sparse_handoff", capacity, candidate, "max_ns", "us", 1e-3)
            maximum = max(float(row["max_ns"]) for row in rows) * 1e-3
            lines.append(f"| {capacity} | {candidate} | {rows[0]['count']} | " + " | ".join(cells) + f" | {maximum:.3f} |")

    lines += ["", "## Paced simulation: generated-to-callback-completion age", "",
              "| Capacity | Queue | Trials | p50 (ms) | p95 (ms) | p99 (ms) | Underruns / scheduled samples | Final occupancy (min–max) |",
              "| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for capacity in (256, 65536):
        for candidate in ("spsc", "mutex"):
            rows = group(simulation, "simulation", capacity, candidate, "", expected_simulation_trials)
            for row in rows:
                if int(row["consumed_samples"]) + int(row["underrun_samples"]) != int(row["measured_callbacks"]) * int(row["block_frames"]):
                    parser.error("invalid simulation sample accounting")
            cells = [metric(rows, "simulation", capacity, candidate, key, "ms", 1e-6)
                     for key in ("age_p50_ns", "age_p95_ns", "age_p99_ns")]
            for key in ("age_max_ns", "callback_p50_ns", "callback_p95_ns", "callback_p99_ns", "callback_max_ns",
                        "wake_lateness_p99_ns", "wake_lateness_max_ns"):
                metric(rows, "simulation", capacity, candidate, key, "us", 1e-3)
            underruns = sum(int(row["underrun_samples"]) for row in rows)
            scheduled = sum(int(row["measured_callbacks"]) * int(row["block_frames"]) for row in rows)
            occupancy = [int(row["final_queue_occupancy"]) for row in rows]
            lines.append(f"| {capacity} | {candidate} | {len(rows)} | " + " | ".join(cells) +
                         f" | {underruns} / {scheduled} | {min(occupancy)}–{max(occupancy)} |")

    lines += ["", "## Instrumented callback execution and scheduling", "",
              "256-frame budget = 5.805 ms; callback cost includes validation and metadata copying.", "",
              "| Capacity | Queue | Callback p99 (µs) | Wake lateness p99 (ms) | Callbacks over budget | Completions past next deadline |",
              "| ---: | --- | ---: | ---: | ---: | ---: |"]
    for capacity in (256, 65536):
        for candidate in ("spsc", "mutex"):
            rows = group(simulation, "simulation", capacity, candidate, "", expected_simulation_trials)
            def cell(key, scale):
                values = [float(row[key]) * scale for row in rows]
                return f"{statistics.median(values):.3f} [{min(values):.3f}, {max(values):.3f}]"
            over_budget = sum(int(row["callback_over_budget"]) for row in rows)
            late = sum(int(row["missed_next_deadline"]) for row in rows)
            blocks = sum(int(row["measured_callbacks"]) for row in rows)
            lines.append(f"| {capacity} | {candidate} | {cell('callback_p99_ns', 1e-3)} | "
                         f"{cell('wake_lateness_p99_ns', 1e-6)} | {over_budget}/{blocks} | {late}/{blocks} |")

    with (directory / "summary.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    (directory / "SUMMARY.md").write_text("\n".join(lines) + "\n")
    print(f"Wrote {directory / 'SUMMARY.md'} and summary.csv")


if __name__ == "__main__":
    main()
