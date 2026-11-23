#!/usr/bin/env python3
"""Compress the checked static UI font for the firmware image."""

import argparse
from pathlib import Path
import struct
import zlib


def pack(source: Path) -> bytes:
    raw = source.read_bytes()
    return struct.pack("<4sI", b"FNT1", len(raw)) + zlib.compress(raw, level=9)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    packed = pack(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != packed:
        args.output.write_bytes(packed)
    print(f"{args.output}: {args.source.stat().st_size} -> {len(packed)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
