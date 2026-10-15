#!/usr/bin/env python3
"""Require real AO-mapped meshes and measured PBR GPU frames in each run."""

import json
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_pbr_ao_report.py BENCHMARK_DIR", file=sys.stderr)
        return 2

    benchmark_dir = Path(sys.argv[1])
    reports = sorted(benchmark_dir.glob("run-*.json"))
    if not reports:
        print("no AO benchmark run reports found", file=sys.stderr)
        return 1

    for path in reports:
        try:
            report = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            print(f"could not read {path}: {error}", file=sys.stderr)
            return 1
        scene = report.get("scene") or {}
        frames = report.get("frames") or []
        if scene.get("ao_mesh_count", 0) < 1:
            print(f"{path.name} loaded no AO-mapped mesh: {scene}", file=sys.stderr)
            return 1
        if not any(
            (frame.get("gpu_pass_ns") or {}).get("main_forward") is not None
            for frame in frames
        ):
            print(f"{path.name} has no measured PBR forward pass", file=sys.stderr)
            return 1

    print(f"PASS · {len(reports)} AO-mapped PBR GPU benchmark run(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
