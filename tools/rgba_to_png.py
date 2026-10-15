#!/usr/bin/env python3
"""Convert Obsidian headless RGBA8 readback to a portable, lossless PNG.

No Pillow or other dependency: the renderer emits raw row-major RGBA bytes.
The converter bounds dimensions, rejects truncated/overlong buffers, and
atomically replaces the destination only after the complete PNG is produced.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import struct
import tempfile
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"

def chunk(name: bytes, contents: bytes) -> bytes:
    return (struct.pack(">I", len(contents)) + name + contents +
            struct.pack(">I", zlib.crc32(name + contents)))

def encode(raw: bytes, width: int, height: int) -> bytes:
    if not 1 <= width <= 8192 or not 1 <= height <= 8192:
        raise ValueError("image dimensions must be in 1..8192")
    stride = width * 4
    if len(raw) != stride * height:
        raise ValueError("RGBA8 input size does not match width * height * 4")
    scanlines = b"".join(b"\0" + raw[y*stride:(y+1)*stride]
                         for y in range(height))
    return (SIGNATURE +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(scanlines, 6)) +
            chunk(b"IEND", b""))

def convert(source: Path, destination: Path, width: int, height: int) -> None:
    if not 1 <= width <= 8192 or not 1 <= height <= 8192:
        raise ValueError("image dimensions must be in 1..8192")
    if source.resolve() == destination.resolve():
        raise ValueError("output cannot overwrite the input RGBA file")
    expected = width * height * 4
    if source.stat().st_size != expected:
        raise ValueError("RGBA file does not match expected dimensions")
    content = encode(source.read_bytes(), width, height)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent,
                                         prefix=".obsidian-png-", delete=False) as f:
            temporary = Path(f.name)
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    args = parser.parse_args()
    convert(args.source, args.destination, args.width, args.height)

if __name__ == "__main__":
    main()
