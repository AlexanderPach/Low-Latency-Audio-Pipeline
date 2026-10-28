#!/usr/bin/env python3
"""Run the two benchmarks sequentially and retain CSV, logs, and machine metadata."""

import argparse
import datetime
import hashlib
import json
import os
import platform
from pathlib import Path
import subprocess
import sys


def command_output(command, cwd):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("out/build/benchmarks"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--simulation-trials", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=5_000_000)
    parser.add_argument("--latency-samples", type=int, default=20_000)
    parser.add_argument("--quick", action="store_true", help="correctness smoke run, not reportable measurements")
    args = parser.parse_args()
    if args.trials < 1 or args.simulation_trials < 1:
        parser.error("trial counts must be positive")

    root = Path(__file__).resolve().parents[1]
    build = (root / args.build_dir).resolve()
    output = (root / args.output_dir).resolve()
    # A new directory prevents mixing runs or accidentally replacing raw evidence.
    output.mkdir(parents=True, exist_ok=False)
    suffix = ".exe" if os.name == "nt" else ""
    binaries = [build / (name + suffix) for name in ("queue_benchmark", "audio_queue_simulation")]
    for binary in binaries:
        if not binary.is_file():
            parser.error(f"missing executable: {binary}; build benchmarks first")

    metadata = {
        "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "logical_cpu_count": os.cpu_count(),
        "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "affinity_policy": "inherited; no pinning or real-time priority",
        "git_commit": command_output(["git", "rev-parse", "HEAD"], root),
        "git_branch": command_output(["git", "branch", "--show-current"], root),
        "tracked_changes": command_output(["git", "diff", "--name-only", "HEAD"], root).splitlines(),
        "quick_smoke_only": args.quick,
        "source_sha256": {},
        "binary_sha256": {binary.name: sha256(binary) for binary in binaries},
        "runs": [],
        "notes": "Sequential workloads. No physical audio device used. Other system load is uncontrolled.",
    }
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        metadata["cpu_model"] = next((line.split(":", 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                                      if line.startswith("model name")), "unknown")
    for name in ("cpu.max", "cpuset.cpus.effective", "memory.max"):
        path = Path("/sys/fs/cgroup") / name
        if path.exists():
            metadata["cgroup_" + name] = path.read_text().strip()
    for path in [root / "SPSCDisruptor.cpp", root / "disruptor.h", root / "audio_engine.h",
                 root / "LLAP.cpp", root / "CMakeLists.txt", *sorted((root / "benchmarks").glob("*.cpp")),
                 *sorted((root / "benchmarks").glob("*.h")), Path(__file__).resolve(),
                 root / "benchmarks" / "summarize.py"]:
        metadata["source_sha256"][str(path.relative_to(root))] = sha256(path)
    compile_db = build / "compile_commands.json"
    if compile_db.exists():
        metadata["compile_commands"] = [entry for entry in json.loads(compile_db.read_text())
                                         if Path(entry["file"]).name in ("queue_benchmark.cpp", "audio_queue_simulation.cpp")]
        if metadata["compile_commands"]:
            first = metadata["compile_commands"][0]
            # CMake emits either a shell command or an argument list.
            import shlex
            compiler = (first.get("arguments") or shlex.split(first["command"], posix=os.name != "nt"))[0].strip('"')
            version_flag = "/Bv" if Path(compiler).name.lower() in ("cl", "cl.exe") else "--version"
            version = subprocess.run([compiler, version_flag], cwd=root, text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            metadata["compiler_version"] = version.stdout.strip()
    metadata_path = output / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    try:
        for binary, trials, name in zip(binaries, (args.trials, args.simulation_trials), ("queue", "simulation")):
            command = [str(binary), "--quick"] if args.quick else [str(binary), "--trials", str(trials)]
            if name == "queue" and not args.quick:
                command += ["--iterations", str(args.iterations), "--latency-samples", str(args.latency_samples)]
            print(f"Running {name}; retaining raw output in {output}", flush=True)
            run = {"command": command, "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
            metadata["runs"].append(run)
            with (output / (name + ".csv")).open("w") as csv_file, (output / (name + ".log")).open("w") as log_file:
                completed = subprocess.run(command, cwd=root, stdout=csv_file, stderr=log_file)
            run["returncode"] = completed.returncode
            run["completed_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
            if completed.returncode:
                print(f"{name} failed; see {output / (name + '.log')}", file=sys.stderr)
                return completed.returncode if completed.returncode > 0 else 1
    finally:
        metadata["completed_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
