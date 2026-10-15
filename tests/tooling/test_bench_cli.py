#!/usr/bin/env python3
"""Consumer-level checks for the public benchmark command catalog."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = {
    "font": "bench-font",
    "dispatch": "bench-dispatch",
    "keymap": "bench-keymap",
    "node-topology": "bench-node-topology",
    "object-identity": "bench-object-identity",
    "foreign-slots": "bench-foreign-slots",
    "omdl-digest": "bench-omdl-digest",
    "texture-refs": "bench-texture-refs",
    "sdf-undo": "bench-sdf-undo",
    "skin-overrides": "bench-skin-overrides",
    "animation-channels": "bench-animation-channels",
    "transparent-sort": "bench-transparent-sort",
    "keychords-performance": "test-keychords-performance",
    "engine": "bench-monad",
    "engine-validation": "bench-monad-validation",
}


def main() -> None:
    result = subprocess.run(
        ["./make", "bench", "--list", "--color", "never"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    rows = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in EXPECTED:
            rows[fields[0]] = fields[1]
    assert rows == EXPECTED, (rows, result.stdout)

    test_catalog = subprocess.run(
        ["./make", "test", "--list", "--color", "never"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert test_catalog.returncode == 0, test_catalog.stderr
    assert "headless" in test_catalog.stdout
    assert "test-headless" in test_catalog.stdout
    assert "transparent-sort" in test_catalog.stdout
    assert "test-transparent-sort" in test_catalog.stdout

    invalid_gate = subprocess.run(
        ["./make", "bench", "font", "--max-regression-pct", "1"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert invalid_gate.returncode == 2
    assert "--compare-to" in invalid_gate.stdout

    alias = subprocess.run(
        ["make", "-n", "bench", "BENCH_SELECTION=keymap"],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert alias.returncode == 0, alias.stderr
    assert './make bench "keymap"' in alias.stdout, alias.stdout
    print(f"benchmark CLI catalog and Make alias PASS ({len(rows)} workloads)")


if __name__ == "__main__":
    main()
