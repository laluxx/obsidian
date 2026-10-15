#!/usr/bin/env python3
"""Assert the headless Sponza run uploaded the full scene without page exhaustion."""

import json
from pathlib import Path
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_headless_sponza_report.py BENCHMARK_DIR", file=sys.stderr)
        return 2

    benchmark_dir = Path(sys.argv[1])
    try:
        report = json.loads((benchmark_dir / "run-01.json").read_text())
        log = (benchmark_dir / "run-01.log").read_text(errors="replace")
    except (OSError, json.JSONDecodeError) as error:
        print(f"could not read headless Sponza result: {error}", file=sys.stderr)
        return 1

    scene = report.get("scene", {})
    if scene.get("mesh_count", 0) < 103 or scene.get("texture_count", 0) < 69:
        print(f"Sponza report is incomplete: {scene}", file=sys.stderr)
        return 1

    queued = re.findall(r"Successfully queued texture #(\d+)", log)
    if len(set(queued)) < scene["texture_count"]:
        print(
            f"only {len(set(queued))} unique textures were queued; "
            f"expected {scene['texture_count']}",
            file=sys.stderr,
        )
        return 1

    allocation_errors = (
        "Max texture pages reached",
        "Failed to allocate cached image",
    )
    if any(error in log for error in allocation_errors):
        print("texture allocation failed during the Sponza run", file=sys.stderr)
        return 1

    print(
        f"PASS · Sponza headless scene · {scene['mesh_count']} meshes · "
        f"{scene['texture_count']} textures queued without allocator failures"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
