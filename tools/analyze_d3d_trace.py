#!/usr/bin/env python3
"""Summarize a D3D trace capture (user/traces/d3d_trace_f*.tsv).

Usage:
  analyze_d3d_trace.py <trace.tsv> [--top 40] [--entry-only] [--sequence N]

For each traced function: call count, threads, and whether it is an *entry
point* (called from outside the traced range, i.e. by game code) or internal.
--sequence N prints the first N entry-point calls in order, which is the
per-frame D3D call stream the native renderer must reproduce.
"""
import argparse
import collections
import csv

TRACE_START, TRACE_END = 0x8240C000, 0x82450000

parser = argparse.ArgumentParser()
parser.add_argument("trace")
parser.add_argument("--top", type=int, default=40)
parser.add_argument("--entry-only", action="store_true")
parser.add_argument("--sequence", type=int, default=0)
args = parser.parse_args()

rows = []
with open(args.trace, newline="") as f:
    for r in csv.DictReader(f, delimiter="\t"):
        rows.append(r)

count = collections.Counter()
external = collections.Counter()
callers = collections.defaultdict(collections.Counter)
threads = collections.defaultdict(set)
r3s = collections.defaultdict(set)
for r in rows:
    fn = int(r["function"], 16)
    lr = int(r["lr"], 16)
    count[fn] += 1
    threads[fn].add(r["thread"])
    r3s[fn].add(r["r3"])
    if not (TRACE_START <= lr < TRACE_END):
        external[fn] += 1
        callers[fn][lr] += 1

print(f"{len(rows)} events, {len(count)} functions, "
      f"{sum(1 for f in count if external[f])} called from game code\n")
print(f"{'function':12} {'calls':>8} {'from game':>9} {'threads':>8} {'r3 values':>9}  top callers")
shown = 0
for fn, n in count.most_common():
    if args.entry_only and not external[fn]:
        continue
    top = ", ".join(f"{lr:08X}x{c}" for lr, c in callers[fn].most_common(3))
    r3 = len(r3s[fn])
    print(f"sub_{fn:08X} {n:8} {external[fn]:9} {','.join(sorted(threads[fn])):>8} {r3:9}  {top}")
    shown += 1
    if shown >= args.top:
        break

if args.sequence:
    print(f"\nFirst {args.sequence} calls from game code:")
    k = 0
    for r in rows:
        lr = int(r["lr"], 16)
        if TRACE_START <= lr < TRACE_END:
            continue
        print(f"  {r['seq']:>7} t{r['thread']:>3} sub_{r['function']} <- {r['lr']}  "
              f"r3={r['r3']} r4={r['r4']} r5={r['r5']} r6={r['r6']}")
        k += 1
        if k >= args.sequence:
            break
