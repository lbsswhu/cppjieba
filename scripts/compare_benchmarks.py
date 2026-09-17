#!/usr/bin/env python3
"""Alternate fresh benchmark processes on one CPU and preserve every raw sample."""
import argparse
import datetime
import json
import os
import platform
import statistics
import subprocess
from pathlib import Path


def parse(output):
    metrics = {}
    for line in output.splitlines():
        if not line.startswith("BENCH "):
            continue
        fields = line.split()
        metrics[fields[1]] = {key: float(value) for key, value in
                              (field.split("=", 1) for field in fields[2:])}
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("optimized", type=Path)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("rounds must be positive")
    if args.cpu is None and hasattr(os, "sched_getaffinity"):
        args.cpu = min(os.sched_getaffinity(0))
    report = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "platform": platform.platform(), "cpu": args.cpu, "rounds": args.rounds,
              "binaries": {"baseline": str(args.baseline.resolve()),
                           "optimized": str(args.optimized.resolve())}, "samples": []}
    for iteration in range(args.rounds):
        order = ("baseline", "optimized") if iteration % 2 == 0 else ("optimized", "baseline")
        for name in order:
            command = [report["binaries"][name]]
            if args.cpu is not None:
                command = ["taskset", "-c", str(args.cpu)] + command
            process = subprocess.run(command, capture_output=True, text=True, check=True)
            report["samples"].append({"name": name, "round": iteration + 1,
                                      "metrics": parse(process.stdout),
                                      "stdout": process.stdout, "stderr": process.stderr})
            print(f"{name} {iteration + 1}/{args.rounds}", flush=True)
    medians = {}
    for name in ("baseline", "optimized"):
        samples = [sample["metrics"] for sample in report["samples"] if sample["name"] == name]
        medians[name] = {metric: {field: statistics.median(s[metric][field] for s in samples)
                                 for field in fields} for metric, fields in samples[0].items()}
    report["medians"] = medians
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(medians, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
