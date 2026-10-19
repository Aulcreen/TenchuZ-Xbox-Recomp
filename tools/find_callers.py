#!/usr/bin/env python3
"""List the functions that call (bl/b) given target addresses.

Usage:
  find_callers.py <default.pe> <codegen trace log> <target> [<target> ...]

Targets are hex addresses (0x8280969C) or import names found in
docs/xenia/imports.txt (VdSwap). Function ranges come from the trace log, as in
find_data_funcs.py.
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_data_funcs import BASE, branch_target, known_ranges, sections, word  # noqa: E402


def import_thunks():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "xenia", "imports.txt")
    names = {}
    if os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if len(parts) >= 5 and parts[0] == "F":
                names[parts[-1]] = int(parts[2], 16)
    return names


def main(pe_path, trace_path, targets):
    pe = open(pe_path, "rb").read()
    text = next(s for s in sections(pe) if s[0] == ".text")
    lo, hi = text[1], text[1] + text[2]
    starts = sorted(known_ranges(trace_path).items())
    entry_list = [s for s, _ in starts]
    thunks = import_thunks()
    by_name = {v: k for k, v in thunks.items()}

    resolved = []
    for t in targets:
        resolved.append(int(t, 16) if t.lower().startswith("0x") else thunks[t])
    wanted = set(resolved)

    callers = {t: {} for t in resolved}
    for a in range(lo, hi, 4):
        insn = word(pe, a)
        if insn >> 26 != 18:
            continue
        tgt, link = branch_target(insn, a)
        if tgt in wanted:
            i = bisect.bisect_right(entry_list, a) - 1
            func = starts[i][0] if i >= 0 and a < starts[i][1] else None
            callers[tgt].setdefault(func, []).append(a)

    for t in resolved:
        label = by_name.get(t, f"0x{t:08X}")
        print(f"{label} (0x{t:08X}):")
        for func, sites in sorted(callers[t].items(), key=lambda kv: kv[0] or 0):
            name = f"sub_{func:08X}" if func else "(outside known functions)"
            print(f"  {name}  call sites: {', '.join(f'0x{s:08X}' for s in sites)}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
