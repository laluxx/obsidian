#!/usr/bin/env python3
"""Independent Python-vs-production-C OBSLOOK acceptance."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import obslook


def main():
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="c62-obslook-") as d:
        preset = Path(d) / "look.obslook"
        fields = [.2, .3, .4, 1., .7, .6, .5, .4, .3, 99.]
        args = [sys.executable, str(root / "tools/obslook.py"), "write",
                str(preset), "--rgba", ".2", ".3", ".4", "1",
                "--roughness", ".7", "--metallic", ".6",
                "--emissive", ".5", ".4", ".3", "--strength", "99"]
        subprocess.run(args, check=True)
        assert len(preset.read_bytes()) == 100
        assert list(obslook.decode(preset.read_bytes())) == fields
        output = subprocess.check_output(
            [sys.executable, str(root / "tools/obslook.py"), "inspect",
             str(preset)], text=True)
        assert json.loads(output)["metallic"] == .6
        # Actual production C decoder, scene journal and GPU-slot path accept
        # the independent Python encoder's preset.
        test_build = Path(os.environ.get("OBSIDIAN_TEST_BUILD", "build/tests"))
        if not test_build.is_absolute():
            test_build = root / test_build
        binary = test_build / "sdf-appearance-clipboard"
        captured = Path(d) / "c-produced.obslook"
        env = dict(os.environ, OBSIDIAN_C62_PRESET_INPUT=str(preset),
                   OBSIDIAN_C62_CAPTURE_FILE=str(captured))
        subprocess.run([str(binary)], cwd=root, env=env, check=True)
        assert list(obslook.decode(captured.read_bytes())) == fields
        assert captured.read_bytes() == preset.read_bytes()
        original = preset.read_bytes()
        for candidate in (original[:-1], original + b"\0",
                          original[:20] + b"z" + original[21:]):
            try:
                obslook.decode(candidate)
                raise AssertionError("malformed file accepted")
            except ValueError:
                pass
        failure = subprocess.run(args[:-1] + ["-1"], capture_output=True)
        assert failure.returncode != 0 and preset.read_bytes() == original
        print("Cycle 62: independent Python codec + real C loader + scene undo PASS")


if __name__ == "__main__":
    main()
