"""Static analysis of The Lost World's PowerPC program (CROM).

Walks the code from the reset vector and the exception vectors,
following branches and calls, discovers functions and generates:
  out/listing.asm    disassembly of all reachable code
  out/functions.txt  functions (address, size, calls)
  out/report.txt     statistics for the recompiler

Usage: python tools/analyze.py [data/crom.bin]
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from ppc import decode, sx  # noqa: E402

CROM_BASE = 0xFFE00000          # 2 MB CROM at the end of the 32-bit address space
RESET = 0xFFF00100
VECTORS = [0xFFF00000 + v for v in range(0x100, 0x1500, 0x100)]
RAM_SIZE = 8 * 1024 * 1024
COPY_TABLE = 0xFFF00058         # table (src-4, dst-4, word count, bank) used by the boot code
# After copying, the boot code does "lis r0,0; mtlr r0; blr": the game starts at RAM 0x0
RAM_ENTRY = 0x00000000
RAM_VECTORS = list(range(0x100, 0x1500, 0x100))


class Memory:
    """Fixed CROM at 0xFFE00000 + RAM image rebuilt the same way the boot code does."""

    def __init__(self, rom):
        self.rom = rom
        self.ram = bytearray(RAM_SIZE)
        self.copies = []
        t = COPY_TABLE
        while True:
            src = self.r32(t)
            if src == 0:
                break
            dst, n = self.r32(t + 4), self.r32(t + 8)
            src, dst = (src + 4) & 0xFFFFFFFF, (dst + 4) & 0xFFFFFFFF
            for i in range(n):
                struct.pack_into(">I", self.ram, dst + 4 * i, self.r32(src + 4 * i))
            self.copies.append((src, dst, n * 4))
            t += 16

    def valid(self, a):
        return a < RAM_SIZE or CROM_BASE <= a < CROM_BASE + len(self.rom)

    def r32(self, a):
        if a < RAM_SIZE:
            return struct.unpack_from(">I", self.ram, a)[0]
        return struct.unpack_from(">I", self.rom, a - CROM_BASE)[0]


def jump_table(mem, insns, a):
    """Recognizes the compiler's typical switch pattern:
        cmplwi crN, rI, MAX ; bgt crN, default ; ... lis/addi rT, TABLE ;
        lwzx ; add rX, r0, rT ; mtctr rX ; bctr
    The table holds offsets relative to its own address."""
    base = limit = None
    for k in range(1, 20):
        ins = insns.get(a - 4 * k)
        if ins is None:
            break
        w = ins.word
        op = w >> 26
        if base is None and op == 14 and k > 1:
            prev = insns.get(a - 4 * (k + 1))
            if prev is not None and prev.word >> 26 == 15:
                base = ((prev.word & 0xFFFF) << 16) + sx(w & 0xFFFF, 16)
                base &= 0xFFFFFFFF
        if op == 10 and limit is None:
            limit = (w & 0xFFFF) + 1
            break
    if base is None or limit is None or limit > 1024:
        return []
    targets = []
    for i in range(limit):
        t = (base + sx(mem.r32(base + 4 * i), 32)) & 0xFFFFFFFF
        if mem.valid(t) and t % 4 == 0:
            targets.append(t)
    return targets


def discover(mem):
    """Discovers functions and reachable code. Returns a dict with:
    insns {addr: Insn}, entries (set), callers, heuristic, outside,
    indirect [bctr addr], jumptables {bctr addr: [targets]}."""
    word = mem.r32
    insns = {}
    func_entries = set([RESET, RAM_ENTRY]) | set(VECTORS) | set(RAM_VECTORS)
    # Functions called through pointers: Sega's compiler generates the prologue
    # "stwu r1,-N(r1)" (sometimes preceded by "mflr r0"). We take them as
    # candidates within the code copied to RAM.
    heuristic = set()
    for src, dst, n in mem.copies:
        for a in range(dst & ~3, dst + n, 4):
            w = word(a)
            if (w >> 16) == 0x9421 and w & 0x8000:
                prev = word(a - 4) if a >= 4 else 0
                start = a - 4 if prev == 0x7C0802A6 else a
                heuristic.add(start)
    func_entries |= heuristic
    # Extra entries: those the runtime detected while running (out/missing.txt)
    # and those added by hand (tools/extra_entries.txt).
    for extra in ("out/missing.txt", os.path.join(os.path.dirname(__file__), "extra_entries.txt")):
        if os.path.exists(extra):
            for line in open(extra):
                line = line.split("#")[0].strip()
                if line:
                    func_entries.add(int(line, 16))
    callers = collections.defaultdict(set)
    work = list(func_entries)
    outside = collections.Counter()
    indirect = []
    jumptables = {}
    scanned = set()
    while True:
        _walk(mem, work, insns, func_entries, callers, outside, indirect, jumptables)
        # Function addresses built with "lis rX,hi ; addi/ori rY,rX,lo"
        # (function pointers stored in tables or variables)
        work = []
        for a in sorted(set(insns) - scanned):
            scanned.add(a)
            w = insns[a].word
            if w >> 26 != 15 or (w >> 16) & 31:
                continue
            rx, hi = (w >> 21) & 31, (w & 0xFFFF) << 16
            for k in range(1, 9):
                n = insns.get(a + 4 * k)
                if n is None:
                    break
                op = n.word >> 26
                if op in (14, 24) and (n.word >> 16) & 31 == rx:
                    lo = n.word & 0xFFFF
                    t = (hi + (sx(lo, 16) if op == 14 else lo)) & 0xFFFFFFFF
                    if t not in func_entries and looks_like_function(mem, t):
                        func_entries.add(t)
                        heuristic.add(t)
                        work.append(t)
                    break
                if (n.word >> 21) & 31 == rx and op not in (36, 37, 38, 44, 47):
                    break
        if not work:
            break

    return dict(insns=insns, entries=func_entries, callers=callers, heuristic=heuristic,
                outside=outside, indirect=indirect, jumptables=jumptables)


def looks_like_function(mem, t):
    """Plausible function start: aligned, within the code copied to RAM,
    and right after a function end (blr, b, rfi or padding)."""
    if t % 4 or not any(dst <= t < dst + n for _, dst, n in mem.copies):
        return False
    if decode(mem.r32(t), t).mnem == ".word":
        return False
    prev = mem.r32(t - 4)
    return prev in (0x4E800020, 0x4C000064, 0) or prev >> 26 == 18 and not prev & 1


def _walk(mem, work, insns, func_entries, callers, outside, indirect, jumptables):
    word = mem.r32
    while work:
        a = work.pop()
        while mem.valid(a) and a not in insns:
            ins = decode(word(a), a)
            insns[a] = ins
            if ins.mnem == ".word":
                break
            if ins.branch is not None:
                t = ins.branch
                if mem.valid(t):
                    if ins.call:
                        func_entries.add(t)
                        callers[t].add(a)
                    work.append(t)
                else:
                    outside[t] += 1
            if ins.indirect and not ins.call and not ins.cond and ins.mnem.startswith("bctr"):
                indirect.append(a)
                tg = jump_table(mem, insns, a)
                jumptables[a] = tg
                work.extend(tg)
            if ins.ends_block:
                break
            a += 4


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/crom.bin"
    rom = open(path, "rb").read()
    mem = Memory(rom)
    for src, dst, n in mem.copies:
        print(f"copy {src:08X} -> RAM {dst:08X} ({n} bytes)")
    with open("data/ram_boot.bin", "wb") as fo:
        fo.write(mem.ram)

    d = discover(mem)
    insns, func_entries, callers = d["insns"], d["entries"], d["callers"]
    heuristic, outside, indirect = d["heuristic"], d["outside"], d["indirect"]

    # Functions: range from each entry up to the next entry
    entries = sorted(func_entries)
    os.makedirs("out", exist_ok=True)
    mnem = collections.Counter(i.mnem.rstrip(".") for i in insns.values())
    bad = [i for i in insns.values() if i.mnem == ".word"]
    mmio = collections.Counter()
    for i in insns.values():
        if i.mnem == "lis":
            hi = int(i.ops[1], 16)
            if hi >= 0x8000:
                mmio[hi << 16] += 1

    with open("out/listing.asm", "w") as fo:
        prev = None
        for a in sorted(insns):
            if prev is not None and a != prev + 4:
                fo.write("\n")
            if a in func_entries:
                fo.write(f"\nfn_{a:08X}:   ; called from {len(callers[a])} sites\n")
            fo.write(f"  {a:08X}: {insns[a].word:08X}  {insns[a].text()}\n")
            prev = a
    with open("out/functions.txt", "w") as fo:
        for e in entries:
            n = 0
            a = e
            while a in insns and (a == e or a not in func_entries):
                n += 1
                a += 4
            fo.write(f"fn_{e:08X}  {n:6} insns  callers={len(callers[e])}\n")
    with open("out/report.txt", "w", encoding="utf-8") as fo:
        fo.write(f"Reachable instructions: {len(insns)} ({len(insns) * 4 // 1024} KB of code)\n")
        fo.write(f"Functions: {len(entries)} ({len(heuristic)} of them found by their prologue)\n")
        fo.write(f"Indirect bctr branches (jump tables): {len(indirect)}\n")
        fo.write(f"Unrecognized opcodes in reachable code: {len(bad)}\n")
        for i in bad[:20]:
            fo.write(f"   {i.addr:08X}: {i.word:08X}\n")
        fo.write("\nBranches outside the CROM (code copied to RAM or other):\n")
        for t, c in outside.most_common(20):
            fo.write(f"   {t:08X} x{c}\n")
        fo.write("\nHigh memory bases loaded with lis (hardware registers):\n")
        for t, c in sorted(mmio.items()):
            fo.write(f"   {t:08X} x{c}\n")
        fo.write("\nMost used mnemonics:\n")
        for m, c in mnem.most_common(60):
            fo.write(f"   {m:10} {c}\n")
    print(open("out/report.txt", encoding="utf-8").read()[:2500])


if __name__ == "__main__":
    main()
