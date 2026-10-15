#!/usr/bin/env python3
"""Exercise cold and cached geometry growth under the isolated ASan binary."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
executable = Path(sys.argv[1]).resolve()
scene = root / "tests/fixtures/async-meshlet-growth.gltf"

with tempfile.TemporaryDirectory(prefix="obsidian-asan-growth-") as home:
    env = os.environ.copy()
    env["HOME"] = home
    env.pop("OBSIDIAN_BENCH_SCENE_CACHE", None)
    for run in range(2):
        report_path = Path(home) / f"growth-{run}.json"
        result = subprocess.run(
            [str(executable), "--headless-benchmark", str(report_path),
             "--scene", str(scene), "--width", "256", "--height", "256"],
            cwd=root, env=env, text=True, capture_output=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        report = json.loads(report_path.read_text())
        geometry = report["gpu_geometry_buffers"]
        scene_report = report["scene"]
        assert scene_report["vertex_count"] == 9000
        assert scene_report["index_count"] == 9000
        assert scene_report["meshlet_count"] == 143
        assert geometry["vertex_capacity_bytes"] > 4096
        assert geometry["index_capacity_bytes"] > 4 * 1024 * 1024
        assert geometry["vertex_used_bytes"] <= geometry["vertex_capacity_bytes"]
        assert geometry["index_used_bytes"] <= geometry["index_capacity_bytes"]
        if run == 1 and "[OMDL] validated cache hit:" not in result.stdout:
            raise AssertionError("second sanitized load did not exercise OMDL cache hit")
    print("ASAN geometry growth: cold upload and OMDL cache-hit growth PASS")
