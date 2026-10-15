#!/usr/bin/env python3
"""Alternating paired fresh-process startup captures, with isolated pipeline caches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys


def paired_statistics(baseline, candidate):
    if len(baseline) != len(candidate) or len(baseline) < 5:
        raise ValueError("at least five equal-sized pairs required")
    differences = [b - a for a, b in zip(baseline, candidate)]
    rng = random.Random(0)
    samples = sorted(statistics.median(rng.choices(differences, k=len(differences)))
                     for _ in range(10000))
    return {"pairs": len(differences), "baseline_median_ns": statistics.median(baseline),
            "candidate_median_ns": statistics.median(candidate),
            "paired_median_difference_ns": statistics.median(differences),
            "paired_difference_ci_95_ns": [samples[249], samples[9749]],
            "paired_differences_ns": differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--pipeline-cache", choices=("cold", "warm"), default="warm")
    parser.add_argument("--scene", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.pairs < 5:
        parser.error("--pairs must be at least five")
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    # Never overwrite cache/report evidence from a previous experiment.
    output.mkdir(parents=True, exist_ok=False)
    executables = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    cache_paths = {name: output / f"{name}-warm.bin" for name in executables}
    captures = []

    def capture(name, label, prime=False):
        target = output / label
        env = os.environ.copy()
        cache = cache_paths[name] if args.pipeline_cache == "warm" else output / f"{label}-cold.bin"
        env["OBSIDIAN_PIPELINE_CACHE_PATH"] = str(cache)
        command = [sys.executable, str(root / "tools/bench/engine_bench.py"),
                   "--executable", str(executables[name]), "--runs", "1",
                   "--warmup-frames", "1", "--frames", "2",
                   "--gpu-timestamps", "off", "--debug-labels", "off",
                   "--work-counters", "off", "--output", str(target), "--color", "never"]
        if args.scene:
            command += ["--headless", "--scene", str(args.scene.resolve()),
                        "--width", "640", "--height", "360"]
        with (output / f"{label}.log").open("w") as log:
            subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=300)
        summary = json.loads((target / "summary.json").read_text())
        if not cache.is_file():
            raise RuntimeError("engine did not write isolated pipeline cache")
        if not prime:
            captures.append({"name": name, "label": label, "pipeline_cache": str(cache),
                             "summary": str(target / "summary.json")})
        return summary

    if args.pipeline_cache == "warm":
        for name in executables:
            capture(name, f"prime-{name}", prime=True)
    runs = {name: [] for name in executables}
    identity = None
    for pair in range(args.pairs):
        order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
        for name in order:
            print(f"pair {pair + 1}/{args.pairs}: {name}", flush=True)
            run = capture(name, f"pair-{pair + 1:02}-{name}")
            current_identity = run["comparison_identity"]["sha256"]
            if identity is not None and identity != current_identity:
                raise ValueError("host/device/workload/instrumentation identity changed")
            identity = current_identity
            runs[name].append(run)
    names = runs["baseline"][0]["startup_milestones"]
    timings = {}
    for milestone in names:
        values = {name: [run["startup_milestones"][milestone]["median_ns"] for run in group]
                  for name, group in runs.items()}
        if all(value is not None for group in values.values() for value in group):
            timings[milestone] = paired_statistics(values["baseline"], values["candidate"])
    report = {"comparison_identity": identity,
              "pipeline_cache_mode": args.pipeline_cache,
              "cache_scope": "engine VkPipelineCache only; OS pages and driver caches uncontrolled",
              "ordering": "alternating AB/BA fresh-process pairs",
              "caveat": "pairing reduces drift; other desktop/GPU activity remains uncontrolled",
              "executables": {name: {"path": str(path),
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                  for name, path in executables.items()},
              "captures": captures, "startup_milestones": timings}
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, stats in timings.items():
        lower, upper = stats["paired_difference_ci_95_ns"]
        print(f"{name:24} paired delta {stats['paired_median_difference_ns']/1e6:+.3f} ms"
              f"  95% CI [{lower/1e6:+.3f}, {upper/1e6:+.3f}] ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
