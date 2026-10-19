#!/usr/bin/env python3
"""Summarize native-renderer frame logs (tenchuz: NATIVE ... lines).

Usage: summarize_native_log.py <logs/run.log> [--detail N]

One row per logged frame: tiling passes, colour/depth clears, resolves done /
skipped (depth or empty), distinct render targets and resolve destinations,
and whether the front buffer came from a native texture.
"""
import argparse
import re

parser = argparse.ArgumentParser()
parser.add_argument("log")
parser.add_argument("--detail", type=int, default=0, help="print the operations of the first N frames")
args = parser.parse_args()

frames, current = [], []
for line in open(args.log, encoding="utf-8", errors="replace"):
    m = re.search(r"tenchuz: NATIVE (.*)$", line)
    if not m:
        continue
    op = m.group(1).strip()
    if op.startswith(("RT0 surface", "depth surface", "viewport")):
        continue
    current.append(op)
    if op.startswith(("present front buffer", "frame end front buffer")):
        frames.append(current)
        current = []

print(f"{len(frames)} logged frames")
print(f"{'#':>3} {'tiling':>6} {'clrC':>5} {'clrZ':>5} {'resolve':>7} {'skip':>5} {'RTs':>4} {'dests':>5}  front buffer")
for i, ops in enumerate(frames):
    tiling = sum(o.startswith("begin tiling") for o in ops)
    clr_c = sum(o.startswith("clear RT") for o in ops)
    clr_z = sum(o.startswith("clear depth") for o in ops)
    res = [o for o in ops if o.startswith("resolve")]
    skipped = sum("skipped" in o for o in res)
    rts = {re.search(r"edram (\w+) (\d+x\d+)", o).groups() for o in ops if o.startswith("clear RT")}
    dests = {re.search(r"-> (\w+)", o).group(1) for o in res if "->" in o and "skipped" not in o}
    front = ops[-1].split(":")[-1].strip()
    print(f"{i:>3} {tiling:>6} {clr_c:>5} {clr_z:>5} {len(res) - skipped:>7} {skipped:>5} {len(rts):>4} {len(dests):>5}  {front}")

for i, ops in enumerate(frames[: args.detail]):
    print(f"\n--- frame {i} ({len(ops)} operations)")
    for o in ops:
        print("  " + o)
