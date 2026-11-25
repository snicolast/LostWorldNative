"""32-bit PowerPC (big-endian) decoder for the Model 3's 603e.

It is the first piece of the static recompiler: decode() returns a
structured instruction (mnemonic, operands and branch targets) that
both the disassembler and the C++ generator will use.
"""
from dataclasses import dataclass, field


@dataclass
class Insn:
    addr: int
    word: int
    mnem: str
    ops: list = field(default_factory=list)
    # Control flow
    branch: int | None = None   # static target (b/bc)
    call: bool = False          # LK=1
    cond: bool = False          # conditional branch
    ends_block: bool = False    # b, bclr, bcctr, rfi...
    indirect: bool = False      # bclr/bcctr

    def text(self):
        if self.branch is not None:
            ops = [o for o in self.ops] + [f"0x{self.branch:08X}"]
        else:
            ops = self.ops
        return f"{self.mnem:<10}{', '.join(str(o) for o in ops)}"


SPR = {1: "xer", 8: "lr", 9: "ctr", 18: "dsisr", 19: "dar", 22: "dec", 25: "sdr1",
       26: "srr0", 27: "srr1", 272: "sprg0", 273: "sprg1", 274: "sprg2", 275: "sprg3",
       282: "ear", 287: "pvr", 528: "ibat0u", 529: "ibat0l", 530: "ibat1u", 531: "ibat1l",
       532: "ibat2u", 533: "ibat2l", 534: "ibat3u", 535: "ibat3l", 536: "dbat0u",
       537: "dbat0l", 538: "dbat1u", 539: "dbat1l", 540: "dbat2u", 541: "dbat2l",
       542: "dbat3u", 543: "dbat3l", 1008: "hid0", 1009: "hid1", 1010: "iabr",
       1013: "dabr", 268: "tbl", 269: "tbu", 284: "tbl_w", 285: "tbu_w",
       976: "dmiss", 977: "dcmp", 978: "hash1", 979: "hash2", 980: "imiss",
       981: "icmp", 982: "rpa"}


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def r(n):
    return f"r{n}"


def f(n):
    return f"f{n}"


def h(v):
    return f"0x{v & 0xFFFF:X}" if v >= 0 else f"-0x{-v:X}"


# D-form load/store primary opcodes
LDST = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb",
        39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu",
        46: "lmw", 47: "stmw"}
FLDST = {48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu",
         54: "stfd", 55: "stfdu"}

# Opcode 31, X-form: xo -> (mnem, type)
X31 = {
    0: ("cmp", "cmp"), 32: ("cmpl", "cmp"),
    4: ("tw", "tw"),
    8: ("subfc", "xo"), 10: ("addc", "xo"), 11: ("mulhwu", "xo3"), 19: ("mfcr", "d"),
    20: ("lwarx", "ldx"), 23: ("lwzx", "ldx"), 24: ("slw", "sx"), 26: ("cntlzw", "s1"),
    28: ("and", "sx"), 40: ("subf", "xo"), 54: ("dcbst", "ab"), 55: ("lwzux", "ldx"),
    60: ("andc", "sx"), 75: ("mulhw", "xo3"), 83: ("mfmsr", "d"), 86: ("dcbf", "ab"),
    87: ("lbzx", "ldx"), 104: ("neg", "xo1"), 119: ("lbzux", "ldx"), 124: ("nor", "sx"),
    136: ("subfe", "xo"), 138: ("adde", "xo"), 144: ("mtcrf", "mtcrf"), 146: ("mtmsr", "s"),
    150: ("stwcx.", "stx"), 151: ("stwx", "stx"), 183: ("stwux", "stx"),
    200: ("subfze", "xo1"), 202: ("addze", "xo1"), 210: ("mtsr", "mtsr"),
    215: ("stbx", "stx"), 232: ("subfme", "xo1"), 234: ("addme", "xo1"),
    235: ("mullw", "xo"), 242: ("mtsrin", "sb"), 246: ("dcbtst", "ab"), 247: ("stbux", "stx"),
    266: ("add", "xo"), 278: ("dcbt", "ab"), 279: ("lhzx", "ldx"), 284: ("eqv", "sx"),
    306: ("tlbie", "b"), 310: ("eciwx", "ldx"), 311: ("lhzux", "ldx"), 316: ("xor", "sx"),
    339: ("mfspr", "mfspr"), 343: ("lhax", "ldx"), 370: ("tlbia", "none"),
    371: ("mftb", "mftb"), 375: ("lhaux", "ldx"), 407: ("sthx", "stx"), 412: ("orc", "sx"),
    438: ("ecowx", "stx"), 439: ("sthux", "stx"), 444: ("or", "sx"), 459: ("divwu", "xo"),
    467: ("mtspr", "mtspr"), 470: ("dcbi", "ab"), 476: ("nand", "sx"), 491: ("divw", "xo"),
    512: ("mcrxr", "crfd"), 533: ("lswx", "ldx"), 534: ("lwbrx", "ldx"), 535: ("lfsx", "lfx"),
    536: ("srw", "sx"), 566: ("tlbsync", "none"), 567: ("lfsux", "lfx"), 595: ("mfsr", "mfsr"),
    597: ("lswi", "lswi"), 598: ("sync", "none"), 599: ("lfdx", "lfx"), 631: ("lfdux", "lfx"),
    659: ("mfsrin", "db"), 661: ("stswx", "stx"), 662: ("stwbrx", "stx"), 663: ("stfsx", "lfx"),
    695: ("stfsux", "lfx"), 725: ("stswi", "lswi"), 727: ("stfdx", "lfx"), 759: ("stfdux", "lfx"),
    790: ("lhbrx", "ldx"), 792: ("sraw", "sx"), 824: ("srawi", "srawi"), 854: ("eieio", "none"),
    918: ("sthbrx", "stx"), 922: ("extsh", "s1"), 954: ("extsb", "s1"), 978: ("tlbld", "b"),
    982: ("icbi", "ab"), 983: ("stfiwx", "lfx"), 1010: ("tlbli", "b"), 1014: ("dcbz", "ab"),
}

# Opcode 63 (double) and 59 (single)
F63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 18: "fdiv", 20: "fsub",
         21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte", 28: "fmsub",
         29: "fmadd", 30: "fnmsub", 31: "fnmadd", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
         64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs",
         583: "mffs", 711: "mtfsf"}
F59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
       28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

CR19 = {33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand", 257: "crand",
        289: "creqv", 417: "crorc", 449: "cror"}

BO_COND = {(12, 0): "blt", (12, 1): "bgt", (12, 2): "beq", (12, 3): "bso",
           (4, 0): "bge", (4, 1): "ble", (4, 2): "bne", (4, 3): "bns"}


def _bc_name(bo, bi, suffix):
    """Simplified mnemonic for bc/bclr/bcctr."""
    if bo & 0x14 == 0x14:
        return "b" + suffix, []
    cr, bit = bi >> 2, bi & 3
    if bo & 0x04 and (bo & 0x1C) in (12, 4):
        name = BO_COND[(bo & 0x1C, bit)]
        return name + suffix, ([f"cr{cr}"] if cr else [])
    if bo & 0x1E == 16:
        return "bdnz" + suffix, []
    if bo & 0x1E == 18:
        return "bdz" + suffix, []
    return "bc" + suffix, [bo, bi]


def decode(word, addr):
    op = word >> 26
    rd = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    rc = word & 1
    simm = sx(word & 0xFFFF, 16)
    uimm = word & 0xFFFF
    I = lambda m, *o, **k: Insn(addr, word, m, list(o), **k)

    if op == 18:  # b
        li = sx(word & 0x03FFFFFC, 26)
        aa, lk = (word >> 1) & 1, word & 1
        tgt = (li if aa else addr + li) & 0xFFFFFFFF
        return I("bl" if lk else "b", branch=tgt, call=bool(lk), ends_block=not lk)
    if op == 16:  # bc
        bd = sx(word & 0xFFFC, 16)
        aa, lk = (word >> 1) & 1, word & 1
        tgt = (bd if aa else addr + bd) & 0xFFFFFFFF
        name, ops = _bc_name(rd, ra, "l" if lk else "")
        always = rd & 0x14 == 0x14
        return I(name, *ops, branch=tgt, call=bool(lk), cond=not always,
                 ends_block=always and not lk)
    if op == 19:
        xo = (word >> 1) & 0x3FF
        lk = word & 1
        if xo == 16:
            name, ops = _bc_name(rd, ra, "lr" + ("l" if lk else ""))
            always = rd & 0x14 == 0x14
            return I(name, *ops, cond=not always, call=bool(lk), indirect=True,
                     ends_block=always and not lk)
        if xo == 528:
            name, ops = _bc_name(rd, ra, "ctr" + ("l" if lk else ""))
            always = rd & 0x14 == 0x14
            return I(name, *ops, cond=not always, call=bool(lk), indirect=True,
                     ends_block=always and not lk)
        if xo == 0:
            return I("mcrf", f"cr{rd >> 2}", f"cr{ra >> 2}")
        if xo == 50:
            return I("rfi", ends_block=True, indirect=True)
        if xo == 150:
            return I("isync")
        if xo in CR19:
            return I(CR19[xo], rd, ra, rb)
    if op == 14:
        return I("li", r(rd), h(simm)) if ra == 0 else I("addi", r(rd), r(ra), h(simm))
    if op == 15:
        return I("lis", r(rd), h(uimm)) if ra == 0 else I("addis", r(rd), r(ra), h(uimm))
    if op == 12:
        return I("addic", r(rd), r(ra), h(simm))
    if op == 13:
        return I("addic.", r(rd), r(ra), h(simm))
    if op == 8:
        return I("subfic", r(rd), r(ra), h(simm))
    if op == 7:
        return I("mulli", r(rd), r(ra), h(simm))
    if op == 10:
        return I("cmplwi", f"cr{rd >> 2}", r(ra), h(uimm))
    if op == 11:
        return I("cmpwi", f"cr{rd >> 2}", r(ra), h(simm))
    if op == 24:
        if word == 0x60000000:
            return I("nop")
        return I("ori", r(ra), r(rd), h(uimm))
    if op == 25:
        return I("oris", r(ra), r(rd), h(uimm))
    if op == 26:
        return I("xori", r(ra), r(rd), h(uimm))
    if op == 27:
        return I("xoris", r(ra), r(rd), h(uimm))
    if op == 28:
        return I("andi.", r(ra), r(rd), h(uimm))
    if op == 29:
        return I("andis.", r(ra), r(rd), h(uimm))
    if op == 3:
        return I("twi", rd, r(ra), h(simm))
    if op == 17:
        return I("sc")
    if op in (20, 21, 23):
        sh, mb, me = rb, (word >> 6) & 31, (word >> 1) & 31
        name = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op] + ("." if rc else "")
        return I(name, r(ra), r(rd), r(rb) if op == 23 else sh, mb, me)
    if op in LDST:
        return I(LDST[op], r(rd), f"{h(simm)}({r(ra)})")
    if op in FLDST:
        return I(FLDST[op], f(rd), f"{h(simm)}({r(ra)})")
    if op == 31:
        xo10 = (word >> 1) & 0x3FF
        xo9 = (word >> 1) & 0x1FF
        oe = (word >> 10) & 1
        dot = "." if rc else ""
        entry = X31.get(xo10)
        if entry is None and xo9 in X31 and X31[xo9][1] in ("xo", "xo1", "xo3"):
            entry = X31[xo9]
        if entry is None:
            return I(".word", f"0x{word:08X}")
        m, kind = entry
        if kind in ("xo", "xo1", "xo3"):
            m += ("o" if oe and kind != "xo3" else "") + dot
            if kind == "xo1":
                return I(m, r(rd), r(ra))
            return I(m, r(rd), r(ra), r(rb))
        if kind == "sx":
            if m == "or" and rd == rb:
                return I("mr" + dot, r(ra), r(rd))
            return I(m + dot, r(ra), r(rd), r(rb))
        if kind == "s1":
            return I(m + dot, r(ra), r(rd))
        if kind == "srawi":
            return I(m + dot, r(ra), r(rd), rb)
        if kind == "cmp":
            return I(m + "w", f"cr{rd >> 2}", r(ra), r(rb))
        if kind == "ldx" or kind == "stx":
            return I(m, r(rd), r(ra), r(rb))
        if kind == "lfx":
            return I(m, f(rd), r(ra), r(rb))
        if kind == "ab":
            return I(m, r(ra), r(rb))
        if kind == "b":
            return I(m, r(rb))
        if kind == "d":
            return I(m, r(rd))
        if kind == "s":
            return I(m, r(rd))
        if kind == "db":
            return I(m, r(rd), r(rb))
        if kind == "sb":
            return I(m, r(rd), r(rb))
        if kind == "mfspr" or kind == "mftb":
            spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
            return I(m, r(rd), SPR.get(spr, spr))
        if kind == "mtspr":
            spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
            return I(m, SPR.get(spr, spr), r(rd))
        if kind == "mtcrf":
            return I(m, f"0x{(word >> 12) & 0xFF:02X}", r(rd))
        if kind == "mtsr":
            return I(m, ra & 15, r(rd))
        if kind == "mfsr":
            return I(m, r(rd), ra & 15)
        if kind == "lswi":
            return I(m, r(rd), r(ra), rb or 32)
        if kind == "crfd":
            return I(m, f"cr{rd >> 2}")
        if kind == "tw":
            return I("trap" if rd == 31 else "tw", *([] if rd == 31 else [rd, r(ra), r(rb)]))
        return I(m)
    if op == 59:
        xo = (word >> 1) & 31
        if xo in F59:
            rcn = (word >> 6) & 31
            dot = "." if rc else ""
            if xo in (25,):
                return I(F59[xo] + dot, f(rd), f(ra), f(rcn))
            if xo in (28, 29, 30, 31):
                return I(F59[xo] + dot, f(rd), f(ra), f(rcn), f(rb))
            if xo in (22, 24):
                return I(F59[xo] + dot, f(rd), f(rb))
            return I(F59[xo] + dot, f(rd), f(ra), f(rb))
    if op == 63:
        xo10 = (word >> 1) & 0x3FF
        xo5 = (word >> 1) & 31
        dot = "." if rc else ""
        rcn = (word >> 6) & 31
        if xo5 in (18, 20, 21):
            return I(F63_X[xo5] + dot, f(rd), f(ra), f(rb))
        if xo5 == 25:
            return I("fmul" + dot, f(rd), f(ra), f(rcn))
        if xo5 in (23, 28, 29, 30, 31):
            return I(F63_X[xo5] + dot, f(rd), f(ra), f(rcn), f(rb))
        if xo5 in (22, 26):
            return I(F63_X[xo5] + dot, f(rd), f(rb))
        if xo10 in (0, 32):
            return I(F63_X[xo10], f"cr{rd >> 2}", f(ra), f(rb))
        if xo10 in (12, 14, 15, 40, 72, 136, 264):
            return I(F63_X[xo10] + dot, f(rd), f(rb))
        if xo10 == 583:
            return I("mffs" + dot, f(rd))
        if xo10 == 711:
            return I("mtfsf" + dot, f"0x{(word >> 17) & 0xFF:02X}", f(rb))
        if xo10 in (38, 70):
            return I(F63_X[xo10] + dot, rd)
        if xo10 == 134:
            return I("mtfsfi" + dot, f"cr{rd >> 2}", (word >> 12) & 15)
        if xo10 == 64:
            return I("mcrfs", f"cr{rd >> 2}", f"cr{ra >> 2}")
    return I(".word", f"0x{word:08X}")
