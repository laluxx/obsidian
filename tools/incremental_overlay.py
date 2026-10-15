#!/usr/bin/env python3
"""Create and verify additive Obsidian incremental overlays.

This deliberately rejects links, traversal, implicit deletions and members not
covered by the manifest. Run from any directory; only the caller's explicit
changed-file list is archived. It never modifies the baseline tree.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
from pathlib import Path, PurePosixPath
import re
import tarfile

HASH_LINE = re.compile(r"([a-f0-9]{64})  (.+)")
MANIFEST = re.compile(r"docs/CYCLE([0-9]+)-MANIFEST\.sha256")


def relative_name(name: str) -> str:
    path = PurePosixPath(name)
    if (not name or name.startswith("/") or "\\" in name or
            path.parts[0] != "obsidian" or len(path.parts) < 2 or
            any(part in (".", "..", "") for part in name.split("/"))):
        raise ValueError(f"unsafe overlay member: {name!r}")
    return path.relative_to("obsidian").as_posix()


def _records_from_bytes(blob: bytes) -> list[str]:
    try:
        lines = blob.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ValueError("manifest is not valid UTF-8") from exc
    if not lines or any(not line for line in lines):
        raise ValueError("manifest/list is empty or has empty entries")
    return lines


def verify_archive(path: Path) -> int:
    with tarfile.open(path, "r:gz") as archive:
        members: dict[str, tarfile.TarInfo] = {}
        for member in archive:
            name = relative_name(member.name)
            if not member.isfile() or name in members:
                raise ValueError(f"non-file or duplicate overlay member: {name}")
            members[name] = member
        manifests = [p for p in members if MANIFEST.fullmatch(p)]
        if len(manifests) != 1:
            raise ValueError("overlay needs exactly one cycle manifest")
        manifest = manifests[0]
        cycle = MANIFEST.fullmatch(manifest).group(1)
        file_list = f"docs/CYCLE{cycle}-PATCH-FILES.txt"
        if file_list not in members:
            raise ValueError("missing patch file list")

        def contents(name: str) -> bytes:
            stream = archive.extractfile(members[name])
            if stream is None:
                raise ValueError(f"cannot read overlay member: {name}")
            return stream.read()

        expected = {}
        for line in _records_from_bytes(contents(manifest)):
            match = HASH_LINE.fullmatch(line)
            if not match:
                raise ValueError(f"invalid SHA256 manifest line: {line!r}")
            digest, name = match.groups()
            relative_name(f"obsidian/{name}")
            if name == manifest or name in expected:
                raise ValueError("self-referencing or duplicate manifest path")
            expected[name] = digest
        names = _records_from_bytes(contents(file_list))
        for name in names:
            relative_name(f"obsidian/{name}")
        actual = set(members) - {manifest}
        if set(names) != actual or len(names) != len(actual) or set(expected) != actual:
            raise ValueError("manifest, patch list, and archive members disagree")
        for name, digest in expected.items():
            if hashlib.sha256(contents(name)).hexdigest() != digest:
                raise ValueError(f"overlay hash mismatch: {name}")
        return len(actual)


def pack(root: Path, cycle: int, changed: Path, output: Path) -> int:
    manifest = f"docs/CYCLE{cycle}-MANIFEST.sha256"
    file_list = f"docs/CYCLE{cycle}-PATCH-FILES.txt"
    paths = [line.strip() for line in changed.read_text().splitlines() if line.strip()]
    if not paths or len(paths) != len(set(paths)) or manifest in paths or file_list in paths:
        raise ValueError("invalid explicit changed-file list")
    for name in paths:
        relative_name(f"obsidian/{name}")
        file = root / name
        if not file.is_file() or file.is_symlink():
            raise ValueError(f"missing or unsafe changed file: {name}")
    names = sorted(paths + [file_list])
    (root / file_list).write_text("".join(name + "\n" for name in names))
    (root / manifest).write_text("".join(
        f"{hashlib.sha256((root / name).read_bytes()).hexdigest()}  {name}\n"
        for name in names))
    # Reproducible archive: constant metadata and gzip timestamp. Tar contains
    # only the exactly listed changed files and two integrity artifacts.
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as archive:
                for name in sorted(names + [manifest]):
                    blob = (root / name).read_bytes()
                    info = tarfile.TarInfo(f"obsidian/{name}")
                    info.size = len(blob)
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mode = 0o644
                    archive.addfile(info, io.BytesIO(blob))
    count = verify_archive(output)
    if count != len(names):
        raise ValueError("post-pack verification count mismatch")
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    check = sub.add_parser("verify")
    check.add_argument("archive", type=Path)
    build = sub.add_parser("pack")
    build.add_argument("--root", required=True, type=Path)
    build.add_argument("--cycle", required=True, type=int)
    build.add_argument("--changed", required=True, type=Path)
    build.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.action == "verify":
        count = verify_archive(args.archive)
    else:
        count = pack(args.root, args.cycle, args.changed, args.output)
    print(f"PASS: {count} hash-checked incremental overlay members")


if __name__ == "__main__":
    main()
