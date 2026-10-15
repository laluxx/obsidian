#!/usr/bin/env python3
"""Deterministic round-trip and failure safety for the real CLI image writer."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import zlib

spec = importlib.util.spec_from_file_location(
    "rgba_to_png", Path(__file__).resolve().parents[2] / "tools" / "rgba_to_png.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
raw = bytes((255,0,0,255, 0,255,0,128))
png = m.encode(raw, 2, 1)
assert png.startswith(m.SIGNATURE)
assert png[12:16] == b"IHDR"
assert struct.unpack(">II", png[16:24]) == (2, 1)
# Parse the chunk stream to validate each CRC and row filter, not just magic.
offset, decoded = len(m.SIGNATURE), None
while offset < len(png):
    length = struct.unpack(">I", png[offset:offset+4])[0]
    kind = png[offset+4:offset+8]
    payload = png[offset+8:offset+8+length]
    checksum = struct.unpack(">I", png[offset+8+length:offset+12+length])[0]
    assert zlib.crc32(kind + payload) == checksum
    if kind == b"IDAT": decoded = zlib.decompress(payload)
    offset += 12 + length
assert decoded == b"\0" + raw
with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    source, dest = folder / "src.rgba", folder / "output.png"
    source.write_bytes(raw)
    m.convert(source, dest, 2, 1)
    assert dest.read_bytes() == png
    previous = dest.read_bytes()
    for wrong in ((0, 1), (2, 0), (8193, 1), (3, 1)):
        try:
            m.convert(source, dest, *wrong)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted malformed dimensions {wrong}")
        assert dest.read_bytes() == previous
    try:
        m.convert(source, source, 2, 1)
    except ValueError:
        pass
    else:
        raise AssertionError("same source and destination accepted")
print("Cycle 50 native RGBA -> PNG conversion, CRC and atomic failure PASS")
