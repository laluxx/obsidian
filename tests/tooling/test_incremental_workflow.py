#!/usr/bin/env python3
"""Incremental tar protocol contract: manifest parity, hashes and safe paths."""
import importlib.util
from pathlib import Path
import tarfile
import tempfile

spec = importlib.util.spec_from_file_location(
    "overlay", Path(__file__).resolve().parents[2] / "tools/incremental_overlay.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def rejects(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError("unsafe or corrupt incremental tar accepted")


with tempfile.TemporaryDirectory() as scratch:
    tmp = Path(scratch)
    root = tmp / "obsidian"
    (root / "src").mkdir(parents=True)
    (root / "docs").mkdir(parents=True)
    (root / "src/code.c").write_text("int cycle42(void) { return 42; }\n")
    (root / "docs/CYCLE42-REPORT.org").write_text("cycle 42 test\n")
    files = tmp / "paths.txt"
    files.write_text("src/code.c\ndocs/CYCLE42-REPORT.org\n")
    tar = tmp / "cycle42.tar.gz"
    assert module.pack(root, 42, files, tar) == 3
    assert module.verify_archive(tar) == 3
    assert sorted(root.rglob("*.sha256")) == [root / "docs/CYCLE42-MANIFEST.sha256"]

    # Corrupt one byte without updating its manifest.
    bad = tmp / "bad.tar.gz"
    with tarfile.open(tar, "r:gz") as source, tarfile.open(bad, "w:gz") as sink:
        import io
        for member in source:
            blob = source.extractfile(member).read()
            if member.name == "obsidian/src/code.c":
                blob = b"changed source\n"
                member.size = len(blob)
            sink.addfile(member, io.BytesIO(blob))
    rejects(lambda: module.verify_archive(bad))
    rejects(lambda: module.relative_name("obsidian/../../etc/passwd"))
    rejects(lambda: module.relative_name("/etc/passwd"))
    rejects(lambda: module.relative_name("obsidian/dir/../code.c"))
    rejects(lambda: module.relative_name("other/src/code.c"))
    files.write_text("src/code.c\nsrc/code.c\n")
    rejects(lambda: module.pack(root, 42, files, tmp / "duplicates.tar.gz"))

print("incremental workflow: deterministic pack, digest and traversal guards PASS")
