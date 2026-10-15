#!/usr/bin/env python3
"""Prove zero-sun headless renders bypass directional and cascaded-shadow work."""

import json
from pathlib import Path
import sys


def load_runs(directory: Path) -> list[dict]:
    paths = sorted(directory.glob("run-*.json"))
    if not paths:
        raise ValueError(f"no benchmark reports in {directory}")
    reports = []
    for path in paths:
        try:
            reports.append(json.loads(path.read_text()))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError(f"could not read {path}: {error}") from error
    return reports


def check_mode(directory: Path, enabled: bool) -> dict:
    reports = load_runs(directory)
    expected_intensity = 3.0 if enabled else 0.0
    for index, report in enumerate(reports, 1):
        features = report.get("render_features") or {}
        scene = report.get("scene") or {}
        frames = report.get("frames") or []
        counters = report.get("work_counters_by_stage") or {}
        shadow_work = (
            (counters.get("stages") or {}).get("shadows") or {}
        ).get("mesh_task_workgroups")
        shadow_samples = [
            (frame.get("gpu_pass_ns") or {}).get("shadows")
            for frame in frames
        ]
        if features.get("directional_sun_intensity") != expected_intensity:
            raise ValueError(
                f"{directory.name} run {index}: wrong sun intensity {features}"
            )
        if features.get("directional_shadows_enabled") is not enabled:
            raise ValueError(
                f"{directory.name} run {index}: wrong shadow feature state {features}"
            )
        if scene.get("path") != "tests/fixtures/async-material.gltf" or \
                scene.get("mesh_count") != 1:
            raise ValueError(
                f"{directory.name} run {index}: benchmark scene changed: {scene}"
            )
        if len(frames) != 60 or not any(
            (frame.get("gpu_pass_ns") or {}).get("main_forward") is not None
            for frame in frames
        ):
            raise ValueError(
                f"{directory.name} run {index}: missing measured forward frames"
            )
        if enabled:
            if not shadow_work or not any(value is not None for value in shadow_samples):
                raise ValueError(
                    f"{directory.name} run {index}: sun-enabled scene did no shadow work"
                )
        elif shadow_work != 0 or any(value is not None for value in shadow_samples):
            raise ValueError(
                f"{directory.name} run {index}: zero-sun scene retained shadow work"
            )
    return json.loads((directory / "summary.json").read_text())


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: test_pbr_lighting_report.py SUN_BENCHMARK_DIR "
            "NO_SUN_BENCHMARK_DIR",
            file=sys.stderr,
        )
        return 2
    try:
        sun = check_mode(Path(sys.argv[1]), True)
        no_sun = check_mode(Path(sys.argv[2]), False)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1

    sun_ns = (sun.get("gpu_passes") or {}).get("main_forward", {}).get("median_ns")
    no_sun_ns = (no_sun.get("gpu_passes") or {}).get("main_forward", {}).get("median_ns")
    print(
        "PASS · one-mesh lighting A/B; zero sun bypasses cascaded draws and "
        f"PCF shader branch · main_forward p50 sun={sun_ns}ns no-sun={no_sun_ns}ns"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
