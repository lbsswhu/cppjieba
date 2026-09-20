#!/usr/bin/env python3
"""Run fresh-process A/B/C/D measurements and preserve every raw observation.

Example:
  python3 test/run_cpu_benchmark.py --binary build/cpu_benchmark \
    --diagnostics-binary build/cpu_diagnostics --repeats 5 --threads 1,4 \
    --output /tmp/cppjieba-cpu.jsonl

An independent original binary can be compiled from cpu_benchmark.cpp with
-DCPPJIEBA_CPU_BASELINE and -I/path/to/original/include using the same compiler
and optimization flags, then supplied with --baseline-binary. Diagnostic runs
are separate processes and their timings/RSS are never used for comparisons.
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import random
import statistics
import subprocess
import sys
import time
from collections import defaultdict


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_csv_int(value):
    result = [int(item) for item in value.split(",")]
    if not result or any(item < 0 for item in result):
        raise argparse.ArgumentTypeError("expected comma-separated nonnegative integers")
    return result


def summarize(rows):
    groups = defaultdict(list)
    for row in rows:
        key = (row["variant"], row["kind"], row.get("corpus", ""),
               row.get("api", ""), row["threads"], row["diagnostic"])
        groups[key].append(row)
    metrics = ("cold_ms", "model_rss_bytes", "model_rss_delta_bytes", "initialization_peak_rss_bytes",
               "dictionary_load_ms", "dat_build_ms", "dat_layout_ms", "dat_validation_ms",
               "bytes_per_second", "runes_per_second", "requests_per_second",
               "latency_p50_us", "latency_p95_us", "latency_p99_us", "allocations_per_request",
               "allocated_bytes_per_request", "max_request_live_allocation_bytes")
    result = []
    for key, observations in sorted(groups.items()):
        variant, kind, corpus, api, threads, diagnostic = key
        record = dict(variant=variant, kind=kind, corpus=corpus, api=api,
                      threads=threads, diagnostic=diagnostic, samples=len(observations), metrics={})
        for metric in metrics:
            values = [row[metric] for row in observations if metric in row]
            if values:
                record["metrics"][metric] = dict(median=statistics.median(values),
                    minimum=min(values), maximum=max(values),
                    stdev=statistics.stdev(values) if len(values) > 1 else 0.0)
        result.append(record)
    lookup = {(row["variant"], row["kind"], row["corpus"], row["api"], row["threads"], row["diagnostic"]): row
              for row in result}
    for row in result:
        key = ("A", row["kind"], row["corpus"], row["api"], row["threads"], row["diagnostic"])
        baseline = lookup.get(key)
        if baseline:
            row["ratios_to_A"] = {}
            for metric, value in row["metrics"].items():
                ref = baseline["metrics"].get(metric, {}).get("median")
                if ref:
                    row["ratios_to_A"][metric] = value["median"] / ref
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostics-binary", type=pathlib.Path)
    parser.add_argument("--baseline-binary", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("cpu_benchmark_results.jsonl"))
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--diagnostic-iterations", type=int, default=8)
    parser.add_argument("--threads", type=parse_csv_int, default=[1, 4])
    parser.add_argument("--cpus", type=parse_csv_int, help="explicit inherited Linux CPU affinity mask")
    parser.add_argument("--modes", default="A,B,C,D")
    parser.add_argument("--corpus", choices=["all", "short", "document", "long", "full_document"], default="all")
    parser.add_argument("--api", choices=["all", "mp", "cut", "search", "small", "keywords", "hmm"], default="all")
    parser.add_argument("--seed", type=int, default=20260918)
    parser.add_argument("--build-metadata", default="", help="compiler command/flags or build-directory description")
    args = parser.parse_args()
    if args.repeats < 1 or args.iterations < 1 or args.diagnostic_iterations < 1 or min(args.threads) < 1:
        parser.error("repeats, iterations and thread counts must be positive")
    modes = args.modes.split(",")
    if not modes or any(mode not in "ABCD" or len(mode) != 1 for mode in modes):
        parser.error("--modes must be a comma-separated selection of A,B,C,D")
    if args.cpus:
        if not hasattr(os, "sched_setaffinity"):
            parser.error("--cpus requires sched_setaffinity support")
        os.sched_setaffinity(0, set(args.cpus))
    repo = args.repo.resolve()
    binaries = {"performance": args.binary.resolve()}
    if args.diagnostics_binary:
        binaries["diagnostic"] = args.diagnostics_binary.resolve()
    if args.baseline_binary:
        binaries["original"] = args.baseline_binary.resolve()
    for binary in binaries.values():
        if not binary.is_file():
            parser.error("missing binary: " + str(binary))
    data_files = [repo / "dict" / name for name in
                  ("jieba.dict.utf8", "hmm_model.utf8", "user.dict.utf8", "idf.utf8", "stop_words.utf8")]
    data_files.append(repo / "test/testdata/synthetic_doc.utf8")
    revision = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False).stdout.strip()
    metadata = dict(kind="metadata", created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        platform=platform.platform(), machine=platform.machine(), python=sys.version,
        cpu_affinity=sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else "unavailable",
        revision=revision, build_metadata=args.build_metadata,
        binaries={name: dict(path=str(path), sha256=sha256(path)) for name, path in binaries.items()},
        input_sha256={str(path.relative_to(repo)): sha256(path) for path in data_files},
        benchmark_source_sha256=sha256(pathlib.Path(__file__).with_name("cpu_benchmark.cpp")),
        seed=args.seed, repeats=args.repeats, iterations=args.iterations,
        notes=["Each subprocess creates one fresh model; OS page cache is not evicted.",
               "Cold time includes all Jieba models and keyword dictionaries.",
               "MP uses predecoded full ranges; other APIs include decoding and output materialization.",
               "Short corpus runs 20 times the requested iterations.",
               "Optional --corpus full_document uses the complete file and max(1, iterations // 20) requests; all excludes it.",
               "Diagnostic allocation and MP counters are excluded from performance comparisons.",
               "Allocation bytes are requested C++ new/new[] bytes, not malloc metadata or stack storage.",
               "Workers share one model and inherit the recorded CPU affinity; no per-worker pinning."])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    all_rows = []
    checksums = {}
    rng = random.Random(args.seed)
    jobs = []
    for repeat in range(args.repeats):
        batch = [("performance", mode, threads, repeat) for mode in modes for threads in args.threads]
        if "original" in binaries:
            batch += [("original", "A", threads, repeat) for threads in args.threads]
        rng.shuffle(batch)
        jobs.extend(batch)
    if "diagnostic" in binaries:
        jobs.extend(("diagnostic", mode, threads, 0) for mode in modes for threads in args.threads)
    with args.output.open("w", encoding="utf-8") as raw:
        raw.write(json.dumps(metadata, ensure_ascii=False) + "\n")
        for job_index, (kind, mode, threads, repeat) in enumerate(jobs):
            iterations = args.diagnostic_iterations if kind == "diagnostic" else args.iterations
            command = [str(binaries[kind]), str(repo), "--mode", mode, "--threads", str(threads),
                       "--iterations", str(iterations), "--corpus", args.corpus, "--api", args.api]
            print(f"[{job_index + 1}/{len(jobs)}] {kind} {mode} threads={threads} repeat={repeat + 1}",
                  file=sys.stderr, flush=True)
            started = time.monotonic()
            completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if completed.returncode:
                raw.write(json.dumps(dict(kind="failed_process", command=command, exit_code=completed.returncode,
                                          stdout=completed.stdout, stderr=completed.stderr)) + "\n")
                raw.flush()
                raise RuntimeError(f"benchmark exited {completed.returncode}: {completed.stderr}")
            process_seconds = time.monotonic() - started
            rows = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
            if not rows or rows[0].get("kind") != "cold":
                raise RuntimeError("benchmark did not emit its cold-start record")
            expected = "LegacyDag" if mode == "A" else "PointerFused" if mode == "B" else "DatRawFused"
            if rows[0].get("actual_backend") != expected:
                raise RuntimeError(f"requested {mode}, got fallback: {rows[0]}")
            expected_hmm = mode == "D"
            if (rows[0].get("hmm_optimization_enabled") != expected_hmm or
                    rows[0].get("hmm_dense_emissions") != expected_hmm):
                raise RuntimeError(f"requested {mode}, got unexpected HMM backend: {rows[0]}")
            for row in rows:
                if row["diagnostic"] != (kind == "diagnostic"):
                    raise RuntimeError("performance/diagnostic executable mismatch")
                row.update(variant="original-A" if kind == "original" else mode, repeat=repeat,
                           process_seconds=process_seconds, process_index=job_index)
                if row["kind"] == "warm":
                    key = (row["corpus"], row["api"], row["requests"], row["threads"])
                    checksum = (row["input_bytes"], row["input_runes"], row["output_tokens"])
                    if key in checksums and checksums[key] != checksum:
                        raise RuntimeError(f"different input/output workload for {key}: {checksum} vs {checksums[key]}")
                    checksums[key] = checksum
                all_rows.append(row)
                raw.write(json.dumps(row, ensure_ascii=False) + "\n")
            if completed.stderr:
                raw.write(json.dumps(dict(kind="process_stderr", process_index=job_index,
                                          stderr=completed.stderr), ensure_ascii=False) + "\n")
            raw.flush()
    summary_path = args.output.with_suffix(".summary.json")
    summary = summarize(all_rows)
    summary_path.write_text(json.dumps(dict(metadata=metadata, results=summary), indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
    for row in summary:
        if row["kind"] == "cold" and not row["diagnostic"]:
            metrics = row["metrics"]
            print(f"{row['variant']} threads={row['threads']}: cold median={metrics['cold_ms']['median']:.3f} ms, "
                  f"model RSS={metrics['model_rss_bytes']['median'] / 1048576:.2f} MiB")
    print(f"Raw observations: {args.output}\nRepeated-run medians/ranges: {summary_path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print("cpu benchmark failed: " + str(error), file=sys.stderr)
        sys.exit(1)
