#!/usr/bin/env python3
"""Minimal XDVDFS (Xbox 360 disc) reader.

Usage:
  xiso.py list    <image.iso>
  xiso.py extract <image.iso> <out_dir> [path-glob ...]

Handles full Redump images (XGD1/XGD2/XGD3 video partition offsets) and
plain "game partition only" images.
"""
import fnmatch
import os
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets for raw (Redump) images, plus 0 for trimmed images.
PARTITION_OFFSETS = [0x0, 0xFD90000, 0x2080000, 0x18300000]


def find_partition(f):
    for off in PARTITION_OFFSETS:
        f.seek(off + 0x10000)
        if f.read(len(MAGIC)) == MAGIC:
            return off
    raise SystemExit("XDVDFS volume descriptor not found")


def read_dir(f, base, sector, size):
    """Yield (name, attrs, sector, size) for a directory table (binary tree)."""
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF:  # padding
            continue
        name = data[off + 14 : off + 14 + nlen].decode("latin-1")
        yield name, attrs, start, length
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def walk(f, base, sector, size, prefix=""):
    for name, attrs, start, length in read_dir(f, base, sector, size):
        path = f"{prefix}{name}"
        if attrs & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "extract"):
        print(__doc__)
        return 1
    with open(argv[2], "rb") as f:
        base = find_partition(f)
        f.seek(base + 0x10000 + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        files = sorted(walk(f, base, root_sector, root_size))
        if argv[1] == "list":
            print(f"# partition @ 0x{base:X}, {len(files)} files")
            for path, start, length in files:
                print(f"{length:>12}  {path}")
            return 0
        out_dir = argv[3]
        globs = argv[4:] or ["*"]
        for path, start, length in files:
            if not any(fnmatch.fnmatch(path.lower(), g.lower()) for g in globs):
                continue
            dst = os.path.join(out_dir, *path.split("/"))
            # Resumable: skip files already extracted at the right size.
            if os.path.isfile(dst) and os.path.getsize(dst) == length:
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(base + start * SECTOR)
            remaining = length
            with open(dst, "wb") as o:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 24))
                    o.write(chunk)
                    remaining -= len(chunk)
            print(f"{length:>12}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
