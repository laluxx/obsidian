#!/usr/bin/env python3
"""Build regressions documented in BUILD-PROBLEMS.org; run from repo root."""
import subprocess

plan = subprocess.run(['make', '-Bn'], text=True, capture_output=True)
assert plan.returncode == 0, plan.stderr
assert 'cmake -S tools/msdf' not in plan.stdout, 'plain make selects optional MSDF tool'
assert 'vendor/tinyexr_c_impl.o' in plan.stdout, 'EXR implementation missing from engine build'
assert '-lmonad-compiler' in plan.stdout, 'Monad source-loading library missing from link'
print('PASS: engine build target, EXR and Monad linkage')
