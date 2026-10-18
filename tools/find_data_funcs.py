#!/usr/bin/env python3
"""Find functions reached only through pointers (vtables, callback tables,
lis/addi-built function pointers) that codegen missed, and emit [functions]
overrides for them.

Tenchu Z is built without RTTI, so ReXGlue's RTTI-based vtable scanner finds
nothing, and its lis/addi pointer scan is disabled upstream (false positives).
Frameless leaf functions without .pdata reached only that way are never
discovered and crash at runtime with "Call to invalid or unregistered function".

Code-built pointers (lis rX,hi ; ... ; addi/ori rY,rX,lo) are only accepted when
the target starts right after a function end (previous word is blr/bctr/b or 0),
which keeps the false-positive rate down.

Usage:
  find_data_funcs.py <default.pe> <codegen trace log> <out.toml>

The trace log comes from: rexglue codegen <manifest> --ignore-stamp -v --log-file <log>
"""
import re
import struct
import sys

BASE = 0x82000000
MAX_FUNC = 0x4000  # sanity cap for a recovered leaf function


def sections(pe):
    off = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, off + 6)[0]
    opt = struct.unpack_from("<H", pe, off + 20)[0]
    out = []
    for i in range(count):
        name, vsize, va = struct.unpack_from("<8sII", pe, off + 24 + opt + i * 40)
        out.append((name.rstrip(b"\0").decode(), BASE + va, vsize))
    return out


def word(pe, addr):
    return struct.unpack_from(">I", pe, addr - BASE)[0]


def known_ranges(trace_path):
    """entry -> end. For functions without .pdata, the trace's funcEnd is the end of
    the search region, not the function, so use the discovered instruction count
    when the function is a single contiguous block."""
    rx_end = re.compile(r"discoverBlocks: entry=0x([0-9A-F]+) pdataSize=(\d+) funcEnd=0x([0-9A-F]+)")
    rx_blk = re.compile(r"discoverBlocks: entry=0x([0-9A-F]+) blocks=(\d+) instructions=(\d+)")
    ends, blocks = {}, {}
    with open(trace_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if m := rx_end.search(line):
                ends[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3), 16))
            elif m := rx_blk.search(line):
                blocks[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
    ranges = {}
    for entry, (pdata, func_end) in ends.items():
        nblocks, ninsns = blocks.get(entry, (0, 0))
        if pdata == 0 and nblocks == 1 and ninsns:
            ranges[entry] = entry + 4 * ninsns
        else:
            ranges[entry] = func_end
    return ranges


def branch_target(insn, addr):
    op = insn >> 26
    if op == 18:  # b / bl
        li = insn & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (li if insn & 2 else addr + li), bool(insn & 1)
    if op == 16:  # bc / bcl
        bd = insn & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (bd if insn & 2 else addr + bd), bool(insn & 1)
    return None, False


def is_unconditional_bc(insn):
    bo = (insn >> 21) & 0x1F
    return (bo & 0x14) == 0x14


def recover_size(pe, start, text_end, starts):
    """Walk control flow from start; return byte size or None if it doesn't look like code."""
    seen = set()
    work = [start]
    end = start
    while work:
        a = work.pop()
        while True:
            if a in seen or a >= text_end or a - start > MAX_FUNC:
                break
            if a != start and a in starts:
                break  # ran into another function
            seen.add(a)
            insn = word(pe, a)
            if insn == 0:
                return None if a == start else None
            end = max(end, a + 4)
            op = insn >> 26
            if op == 19 and ((insn >> 1) & 0x3FF) in (16, 528):  # bclr / bcctr
                if is_unconditional_bc(insn) and not insn & 1:
                    break  # blr / bctr: path ends
                a += 4
                continue
            if op in (16, 18):
                tgt, link = branch_target(insn, a)
                if link:  # call, falls through
                    a += 4
                    continue
                uncond = op == 18 or is_unconditional_bc(insn)
                local = start <= tgt < start + MAX_FUNC and tgt not in starts
                if local and tgt > start:
                    work.append(tgt)
                if uncond:
                    break  # tail call or jump: path ends here
                a += 4
                continue
            a += 4
    return end - start if end > start else None


def main(pe_path, trace_path, out_path):
    pe = open(pe_path, "rb").read()
    secs = sections(pe)
    text = next(s for s in secs if s[0] == ".text")
    text_lo, text_hi = text[1], text[1] + text[2]
    ranges = known_ranges(trace_path)
    starts = set(ranges)
    spans = sorted(ranges.items())
    print(f"{len(spans)} known functions from trace")

    def covered(addr):
        lo, hi = 0, len(spans)
        while lo < hi:
            mid = (lo + hi) // 2
            if spans[mid][0] <= addr:
                lo = mid + 1
            else:
                hi = mid
        return lo > 0 and spans[lo - 1][0] <= addr < spans[lo - 1][1]

    refs = {}
    for name, va, vsize in secs:
        if name not in (".rdata", ".data"):
            continue
        for a in range(va, va + vsize - 3, 4):
            v = word(pe, a)
            if text_lo <= v < text_hi and v % 4 == 0 and v not in starts and not covered(v):
                refs.setdefault(v, []).append(a)
    print(f"{len(refs)} uncovered .text addresses referenced from data")

    def after_function_end(addr):
        prev = word(pe, addr - 4)
        if prev == 0:
            return True
        op = prev >> 26
        if op == 19 and ((prev >> 1) & 0x3FF) in (16, 528) and is_unconditional_bc(prev) and not prev & 1:
            return True  # blr / bctr
        return op == 18 and not prev & 1  # unconditional b (tail call)

    code_refs = {}
    hi = {}
    for a in range(text_lo, text_hi, 4):
        if a in starts:
            hi.clear()
        insn = word(pe, a)
        op = insn >> 26
        rd, ra = (insn >> 21) & 31, (insn >> 16) & 31
        v = None
        if op == 15 and ra == 0:  # lis rD, imm
            hi[rd] = (insn & 0xFFFF) << 16
            continue
        if op == 14 and ra in hi:  # addi rD, rA, simm
            imm = insn & 0xFFFF
            v = (hi[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            hi.pop(rd, None)
        elif op == 24 and rd in hi:  # ori rA, rS, uimm (rS in bits 21-25)
            v = hi[rd] | (insn & 0xFFFF)
            hi.pop(ra, None)
        elif op == 19 and ((insn >> 1) & 0x3FF) == 16 and is_unconditional_bc(insn):
            hi.clear()  # blr
        if (v is not None and text_lo <= v < text_hi and v % 4 == 0 and v not in starts
                and v not in refs and not covered(v) and after_function_end(v)):
            code_refs.setdefault(v, []).append(a)
    print(f"{len(code_refs)} uncovered .text addresses built by lis/addi in code")
    for v, rs in code_refs.items():
        refs.setdefault(v, []).extend(rs)

    def is_import_thunk(addr):
        # Unpatched XEX import thunk: 0x01xxxxxx, 0x02xxxxxx records, mtctr r11, bctr.
        return word(pe, addr) >> 24 == 0x01 and word(pe, addr + 4) >> 24 == 0x02

    found = []
    for addr in sorted(refs):
        if is_import_thunk(addr):
            continue
        size = recover_size(pe, addr, text_hi, starts)
        if size:
            found.append((addr, size, refs[addr]))
    print(f"{len(found)} recovered as functions")

    with open(out_path, "w", newline="\n") as o:
        o.write("# GENERATED by tools/find_data_funcs.py - do not edit by hand.\n")
        o.write("# Leaf functions reached only via pointers (vtables, lis/addi in code), no .pdata.\n")
        o.write("[functions]\n")
        for addr, size, rs in found:
            o.write(f"0x{addr:08X} = {{ size = 0x{size:X} }}  # ref {', '.join(f'0x{r:08X}' for r in rs[:3])}\n")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    main(*sys.argv[1:])
