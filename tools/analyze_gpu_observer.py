#!/usr/bin/env python3
"""Learn which D3D device offsets shadow which GPU registers.

Usage:
  analyze_gpu_observer.py <gpu_observe_f*.bin> [more .bin ...] [--names register_table.inc]
                          [--draws] [--unmapped]

Input: files written by src/renderer/gpu_observer.cpp. Each recorded draw holds
the GPU register file decoded from the command stream (ground truth) and a raw
copy of the D3D device object. Several files (title, menu, gameplay) can be
combined; more varied draws give less ambiguous matches.

A device dword matches a register when they are equal at every draw where the
register is known. Matches on values that change between draws are reliable
("varying"); matches on constant values are reported only as runs that extend
a varying match.
"""
import argparse
import collections
import os
import re
import struct

import numpy as np

HEADER_DWORDS = 16
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

parser = argparse.ArgumentParser()
parser.add_argument("files", nargs="+")
parser.add_argument("--names", default=os.path.join(
    ROOT, "third_party", "rexglue-sdk", "include", "rex", "graphics", "register_table.inc"))
parser.add_argument("--draws", action="store_true", help="list recorded draws")
parser.add_argument("--unmapped", action="store_true",
                    help="list varying registers with no device shadow")
parser.add_argument("--exclude-api", nargs="*", default=["82420E40"],
                    help="skip draws emitted by these D3D functions (default: internal GPR-setup draws)")
args = parser.parse_args()

names = {}
if os.path.exists(args.names):
    for line in open(args.names):
        m = re.match(r"XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)", line)
        if m:
            names[int(m.group(1), 16)] = m.group(2)


def reg_name(r):
    if r in names:
        return names[r]
    if 0x4000 <= r < 0x4800:
        return f"ALU_CONST[{(r - 0x4000) // 4}].{'xyzw'[(r - 0x4000) % 4]}"
    if 0x4800 <= r < 0x4900:
        return f"FETCH[{(r - 0x4800) // 6}].dw{(r - 0x4800) % 6}"
    if 0x4900 <= r < 0x4908:
        return f"BOOL[{r - 0x4900}]"
    if 0x4908 <= r < 0x4928:
        return f"LOOP[{r - 0x4908}]"
    return f"reg_{r:04X}"


# Register columns compared: shader GPR setup, render state, shader/fetch/bool/loop constants.
REG_COLUMNS = np.array(list(range(0x0D00, 0x0D10)) + list(range(0x2000, 0x2400)) +
                       list(range(0x4000, 0x4928)), dtype=np.int64)


def load(path):
    header = np.fromfile(path, dtype="<u4", count=8)
    magic, version, draws, reg_count, dev_dwords, device, frame, _ = (int(x) for x in header)
    assert magic == 0x424F5A54, f"{path}: not a gpu_observe file"
    first = np.fromfile(path, dtype="<i4", count=reg_count, offset=32)
    rec = HEADER_DWORDS + reg_count + dev_dwords
    body = np.memmap(path, dtype="<u4", mode="r", offset=32 + reg_count * 4, shape=(draws, rec))
    excluded = {int(x, 16) for x in args.exclude_api}
    keep = np.array([int(h) not in excluded for h in body[:, 0]], dtype=bool)
    rows = np.nonzero(keep)[0]
    return dict(frame=frame, device=device, first=first[REG_COLUMNS],
                headers=np.array(body[rows, :HEADER_DWORDS]),
                regs=np.array(body[rows][:, HEADER_DWORDS + REG_COLUMNS]),
                dev=np.array(body[rows, HEADER_DWORDS + reg_count:]))


sets = [load(f) for f in args.files]
for s, f in zip(sets, args.files):
    print(f"{os.path.basename(f)}: frame {s['frame']}, {len(s['regs'])} draws, device {s['device']:08X}")

# Only registers known at every draw of every file are compared.
known = np.ones(sets[0]["regs"].shape[1], dtype=bool)
for s in sets:
    known &= s["first"] < 0
regs = np.concatenate([s["regs"] for s in sets])
dev = np.concatenate([s["dev"] for s in sets])
n_draws = len(regs)
print(f"{n_draws} draws, {known.sum()} registers known throughout\n")

if args.draws:
    for s in sets:
        for i, h in enumerate(s["headers"]):
            print(f"  f{s['frame']} #{i:3} api=sub_{h[0]:08X} op={h[1]:02X} w={h[2]:08X} {h[3]:08X} {h[4]:08X} "
                  f"pred={h[5]} vs={h[6]:08X}/{h[7]} ps={h[8]:08X}/{h[9]}")
    print()

# Index known registers by their value vector across all draws.
by_vector = collections.defaultdict(list)
varying_reg = {}
for c in np.nonzero(known)[0]:
    col = regs[:, c]
    r = int(REG_COLUMNS[c])
    by_vector[col.tobytes()].append(r)
    varying_reg[r] = bool((col != col[0]).any())

matches = {}  # device dword index -> (candidates, varying)
for d in range(dev.shape[1]):
    col = dev[:, d]
    cands = by_vector.get(col.tobytes())
    if cands:
        matches[d] = (cands, bool((col != col[0]).any()))

# Build runs: consecutive device dwords mapped to consecutive registers.
runs = []
d = 0
while d < dev.shape[1]:
    if d in matches and matches[d][1]:
        for r in matches[d][0]:
            # extend backwards/forwards while device d+k == reg r+k (any variance)
            lo = 0
            while (d - lo - 1) in matches and (r - lo - 1) in matches[d - lo - 1][0]:
                lo += 1
            hi = 0
            while (d + hi + 1) in matches and (r + hi + 1) in matches[d + hi + 1][0]:
                hi += 1
            runs.append((d - lo, r - lo, lo + hi + 1))
    d += 1
runs = sorted(set(runs))
# Drop runs contained in a longer run.
best = []
for run in sorted(runs, key=lambda x: -x[2]):
    ds, rs, n = run
    if any(ds >= b[0] and ds + n <= b[0] + b[2] and rs - ds == b[1] - b[0] for b in best):
        continue
    best.append(run)
best.sort()

print("Device offset -> GPU register runs (anchored on values that change between draws):")
mapped_regs = set()
for ds, rs, n in best:
    varying = sum(1 for k in range(n) if matches.get(ds + k, ([], False))[1])
    first, last = reg_name(rs), reg_name(rs + n - 1)
    span = first if n == 1 else f"{first} .. {last}"
    print(f"  +0x{ds * 4:04X}..+0x{(ds + n) * 4 - 1:04X}  ->  0x{rs:04X}..0x{rs + n - 1:04X}  "
          f"({n} dwords, {varying} varying)  {span}")
    mapped_regs.update(range(rs, rs + n))

varying_regs = [r for r, v in varying_reg.items() if v]
unmapped = [r for r in varying_regs if r not in mapped_regs]
print(f"\n{len(varying_regs)} registers change between draws; {len(varying_regs) - len(unmapped)} have a "
      f"device shadow, {len(unmapped)} do not")
if args.unmapped:
    groups = collections.Counter()
    for r in unmapped:
        groups[reg_name(r).split('[')[0].split('.')[0]] += 1
    for name, n in groups.most_common(40):
        print(f"  {name:40} {n}")
    print("  first 60:", ", ".join(f"{r:04X}:{reg_name(r)}" for r in unmapped[:60]))

# ---------------------------------------------------------------------------
# Layout check: compare the hypothesized device layout register by register.
# ---------------------------------------------------------------------------
LAYOUT = [  # (device byte offset, first register, count, label)
    (0x0400, 0x4800, 192, "fetch constants"),
    (0x0700, 0x4000, 1024, "VS float constants"),
    (0x1700, 0x4400, 1024, "PS float constants"),
    (0x2700, 0x4900, 40, "bool + loop constants"),
    (0x2800, 0x2000, 16, "0x2000 group"),
    (0x284C, 0x2100, 21, "0x2100 group"),
    (0x28A0, 0x2180, 5, "0x2180 group"),
    (0x28B4, 0x2200, 12, "0x2200 group"),
    (0x28E4, 0x2280, 21, "0x2280 group"),
    (0x2938, 0x2300, 38, "0x2300 group"),
    (0x29D0, 0x2380, 8, "0x2380 group"),
]
col_of = {int(r): c for c, r in enumerate(REG_COLUMNS)}
print("\nLayout check (device value == GPU value at each draw):")
for dev_off, reg0, count, label in LAYOUT:
    exact = partial = 0
    worst = []
    for k in range(count):
        r = reg0 + k
        c = col_of.get(r)
        if c is None or not known[c]:
            continue
        d = (dev_off // 4) + k
        g, v = regs[:, c], dev[:, d]
        same = (g == v)
        rate = same.mean()
        if rate == 1.0:
            exact += 1
        else:
            partial += 1
            diff = np.bitwise_or.reduce(g[~same] ^ v[~same])
            worst.append((rate, r, int(diff), int((~same).sum())))
    print(f"  {label:22} +0x{dev_off:04X} -> 0x{reg0:04X} x{count}: {exact} exact, {partial} differ")
    for rate, r, diff, n in sorted(worst)[:12]:
        print(f"      {reg_name(r):34} match {rate*100:5.1f}%  ({n} draws differ, bits {diff:08X})")
