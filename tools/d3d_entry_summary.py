#!/usr/bin/env python3
"""Static summary of D3D entry points: size, D3D-internal callees, and the
offsets each one stores to / loads from the device object (r3 on entry).

Usage:
  d3d_entry_summary.py <default.pe> <codegen trace log> <0xADDR> [<0xADDR> ...]

Tracks r3 through `mr rX,r3` copies so `stw r4,0x2A30(r31)` is reported as a
device store at +0x2A30. Good enough to tell setters apart; not a decompiler.
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_data_funcs import branch_target, known_ranges, word  # noqa: E402

STORES = {36: "stw", 37: "stwu", 38: "stb", 44: "sth", 54: "stfd", 52: "stfs"}
LOADS = {32: "lwz", 34: "lbz", 40: "lhz", 50: "lfd", 48: "lfs"}


def summarize(pe, start, end, starts):
    dev = {3}
    stores, loads, calls = [], [], []
    for a in range(start, end, 4):
        w = word(pe, a)
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):  # mr ra,rs (or rA,rS,rS)
            src, dst = rd, ra
            if src in dev:
                dev.add(dst)
            else:
                dev.discard(dst)
            continue
        if op in STORES or op in LOADS:
            d = w & 0xFFFF
            d = d - 0x10000 if d & 0x8000 else d
            if ra in dev and ra != 1:
                (stores if op in STORES else loads).append((STORES.get(op) or LOADS.get(op), d, rd))
            if op in LOADS and rd in dev:
                dev.discard(rd)
            continue
        if op == 18:
            t, link = branch_target(w, a)
            calls.append(("bl" if link else "b", t))
    return stores, loads, calls


def main(pe_path, trace_path, addrs):
    pe = open(pe_path, "rb").read()
    ranges = known_ranges(trace_path)
    for s in addrs:
        a = int(s, 16)
        end = ranges.get(a, a + 0x40)
        stores, loads, calls = summarize(pe, a, end, ranges)
        st = " ".join(f"{op}r{r}@+0x{off:X}" for op, off, r in stores[:10])
        ld = " ".join(f"+0x{off:X}" for _, off, _ in loads[:8])
        cl = " ".join(f"{k}:{t:08X}" for k, t in calls[:6] if not (0x82653F00 <= t < 0x82654000))
        print(f"sub_{a:08X} [{end - a:4}B] stores: {st or '-'}")
        if ld:
            print(f"{'':20} loads:  {ld}")
        if cl:
            print(f"{'':20} calls:  {cl}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
