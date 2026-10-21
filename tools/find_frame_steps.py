#!/usr/bin/env python3
"""Find guest values that step evenly across consecutive frame dumps.

Usage:
  find_frame_steps.py mem_f28.bin mem_f29.bin mem_f30.bin [--max 200]

The dumps come from --tenchuz_dump_memory_at_frame (src/debug_tools.cpp):
records of {u32 guest address, u32 size (little-endian), data}. Guest values
are big-endian. Prints 4-byte values that change by the same nonzero step from
each dump to the next: floats with a small step (fades, timers) and small
integer counters. A frame-counted fade shows up as one of these.
"""
import argparse
import struct

import numpy as np


def load(path):
    regions = {}
    with open(path, "rb") as f:
        while True:
            header = f.read(8)
            if len(header) < 8:
                break
            addr, size = struct.unpack("<II", header)
            regions[addr] = np.frombuffer(f.read(size), dtype=np.uint8)
    return regions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--max", type=int, default=200)
    args = ap.parse_args()
    dumps = [load(p) for p in args.dumps]
    rows = []
    for addr, data0 in dumps[0].items():
        if any(addr not in d or len(d[addr]) != len(data0) for d in dumps[1:]):
            continue
        n = len(data0) // 4 * 4
        words = [d[addr][:n] for d in dumps]
        u = [w.view(">u4").astype(np.int64) for w in words]
        same = np.ones(n // 4, dtype=bool)
        steps = [u[i + 1] - u[i] for i in range(len(u) - 1)]
        for s in steps[1:]:
            same &= s == steps[0]
        changed = steps[0] != 0
        # Small integer counters.
        small = same & changed & (np.abs(steps[0]) <= 16) & (u[0] < 1_000_000)
        for i in np.nonzero(small)[0]:
            rows.append((addr + 4 * int(i), "int", [int(x[i]) for x in u], int(steps[0][i])))
        # Floats stepping by a constant small amount.
        f = [w.view(">f4").astype(np.float64) for w in words]
        with np.errstate(all="ignore"):
            fs = [f[i + 1] - f[i] for i in range(len(f) - 1)]
            ok = np.isfinite(f[0]) & (np.abs(f[0]) < 100) & (np.abs(fs[0]) > 1e-3) & (np.abs(fs[0]) < 0.5)
            for s in fs[1:]:
                ok &= np.abs(s - fs[0]) < 1e-5 * np.maximum(1.0, np.abs(fs[0])) + 1e-7
        for i in np.nonzero(ok)[0]:
            rows.append((addr + 4 * int(i), "float", [float(x[i]) for x in f], float(fs[0][i])))
    rows.sort(key=lambda r: (r[1], r[0]))
    for addr, kind, values, step in rows[: args.max]:
        vals = " ".join(f"{v:.6g}" for v in values)
        print(f"{addr:08X}  {kind:5}  step {step:+.6g}  values {vals}")
    print(f"{len(rows)} candidates")


if __name__ == "__main__":
    main()
