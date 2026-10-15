#!/usr/bin/env python3
"""Run a native Vulkan contract and fail on validation errors, not just exit status."""
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/bench"))
from engine_bench import validation_error_lines

if len(sys.argv) < 2:
    raise SystemExit("native contract executable required")
env = os.environ.copy()
env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
env["VK_LAYER_VALIDATE_SYNC"] = "1"
result = subprocess.run(sys.argv[1:], env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
sys.stdout.write(result.stdout)
errors = validation_error_lines(result.stdout)
if errors:
    print(f"FAIL: {len(errors)} Vulkan validation error(s)", file=sys.stderr)
raise SystemExit(result.returncode if result.returncode else bool(errors))
