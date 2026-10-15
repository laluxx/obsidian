#!/usr/bin/env python3
"""Cross-language OBSLOOK v1 inspection and authoring for Obsidian SDF presets.

The production C codec is in src/editor/sdf_appearance_clipboard.c. This
independent Python codec is both an authoring tool and a wire-format oracle.
"""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

MAGIC = b"OBSLOOK1"
HEADER = struct.Struct("<8sII")
PAYLOAD = struct.Struct("<10d")
CRC = struct.Struct("<I")
SIZE = HEADER.size + PAYLOAD.size + CRC.size
NAMES = ["r", "g", "b", "a", "roughness", "metallic", "emit_r", "emit_g",
         "emit_b", "emission_strength"]


def validate(values):
    if len(values) != 10 or any(not math.isfinite(n) for n in values):
        raise ValueError("expected ten finite real values")
    if any(not 0 <= n <= 1 for n in values[:9]):
        raise ValueError("RGBA, PBR and emission RGB must lie in [0,1]")
    if not 0 <= values[9] <= 1_000_000:
        raise ValueError("emission strength must lie in [0,1000000]")
    return values


def encode(values):
    values = validate(values)
    body = HEADER.pack(MAGIC, 1, PAYLOAD.size) + PAYLOAD.pack(*values)
    return body + CRC.pack(zlib.crc32(body))


def decode(blob):
    if len(blob) != SIZE:
        raise ValueError(f"expected exactly {SIZE} bytes")
    magic, version, length = HEADER.unpack_from(blob)
    if magic != MAGIC or version != 1 or length != PAYLOAD.size:
        raise ValueError("unsupported OBSLOOK envelope")
    if CRC.unpack_from(blob, SIZE - 4)[0] != zlib.crc32(blob[:-4]):
        raise ValueError("OBSLOOK checksum mismatch")
    return validate(PAYLOAD.unpack_from(blob, HEADER.size))


def write_atomic(path, blob):
    # Publish only after the complete private same-directory file is synced.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=path.name + ".tmp.",
                                         dir=path.parent, delete=False) as file:
            temporary = file.name
            os.chmod(temporary, 0o600)
            file.write(blob)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect", help="Validate and print JSON appearance")
    inspect.add_argument("path", type=Path)
    make = sub.add_parser("write", help="Validate and atomically write appearance")
    make.add_argument("path", type=Path)
    make.add_argument("--rgba", type=float, nargs=4, required=True)
    make.add_argument("--roughness", type=float, required=True)
    make.add_argument("--metallic", type=float, required=True)
    make.add_argument("--emissive", type=float, nargs=3, required=True)
    make.add_argument("--strength", type=float, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "write":
            values = (*args.rgba, args.roughness, args.metallic,
                      *args.emissive, args.strength)
            write_atomic(args.path, encode(values))
        else:
            if args.path.is_symlink():
                raise ValueError("symlink presets are not supported")
            values = decode(args.path.read_bytes())
            print(json.dumps(dict(zip(NAMES, values)), indent=2))
    except (OSError, ValueError, OverflowError, struct.error) as exc:
        parser.exit(1, f"obslook: {exc}\n")


if __name__ == "__main__":
    main()
