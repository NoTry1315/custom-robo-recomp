"""Gekko instruction decoding by raw bit fields (no capstone: paired singles
and exact operand forms matter). decode(w) -> mnemonic or None."""

def opcd(w): return w >> 26
def rD(w): return (w >> 21) & 31
def rA(w): return (w >> 16) & 31
def rB(w): return (w >> 11) & 31
def rC(w): return (w >> 6) & 31
def rc(w): return w & 1
def simm(w): v = w & 0xFFFF; return v - 0x10000 if v & 0x8000 else v
def uimm(w): return w & 0xFFFF
def crfD(w): return (w >> 23) & 7
def crfS(w): return (w >> 18) & 7

OP = {3: 'twi', 7: 'mulli', 8: 'subfic', 10: 'cmpli', 11: 'cmpi', 12: 'addic', 13: 'addic.',
      14: 'addi', 15: 'addis', 16: 'bc', 17: 'sc', 18: 'b', 20: 'rlwimi', 21: 'rlwinm', 23: 'rlwnm',
      24: 'ori', 25: 'oris', 26: 'xori', 27: 'xoris', 28: 'andi.', 29: 'andis.',
      32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu',
      40: 'lhz', 41: 'lhzu', 42: 'lha', 43: 'lhau', 44: 'sth', 45: 'sthu', 46: 'lmw', 47: 'stmw',
      48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu',
      56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu'}

OP19 = {0: 'mcrf', 16: 'bclr', 33: 'crnor', 50: 'rfi', 129: 'crandc', 150: 'isync', 193: 'crxor',
        225: 'crnand', 257: 'crand', 289: 'creqv', 417: 'crorc', 449: 'cror', 528: 'bcctr'}

OP31 = {0: 'cmp', 4: 'tw', 8: 'subfc', 10: 'addc', 11: 'mulhwu', 19: 'mfcr', 20: 'lwarx', 23: 'lwzx',
        24: 'slw', 26: 'cntlzw', 28: 'and', 32: 'cmpl', 40: 'subf', 54: 'dcbst', 55: 'lwzux', 60: 'andc',
        75: 'mulhw', 83: 'mfmsr', 86: 'dcbf', 87: 'lbzx', 104: 'neg', 119: 'lbzux', 124: 'nor',
        136: 'subfe', 138: 'adde', 144: 'mtcrf', 146: 'mtmsr', 150: 'stwcx.', 151: 'stwx', 183: 'stwux',
        200: 'subfze', 202: 'addze', 210: 'mtsr', 215: 'stbx', 232: 'subfme', 234: 'addme', 235: 'mullw',
        242: 'mtsrin', 246: 'dcbtst', 247: 'stbux', 266: 'add', 278: 'dcbt', 279: 'lhzx', 284: 'eqv',
        306: 'tlbie', 310: 'eciwx', 311: 'lhzux', 316: 'xor', 339: 'mfspr', 343: 'lhax', 371: 'mftb',
        375: 'lhaux', 407: 'sthx', 412: 'orc', 438: 'ecowx', 439: 'sthux', 444: 'or', 459: 'divwu',
        467: 'mtspr', 470: 'dcbi', 476: 'nand', 491: 'divw', 512: 'mcrxr', 534: 'lwbrx', 535: 'lfsx',
        536: 'srw', 566: 'tlbsync', 567: 'lfsux', 595: 'mfsr', 598: 'sync', 599: 'lfdx', 631: 'lfdux',
        659: 'mfsrin', 662: 'stwbrx', 663: 'stfsx', 695: 'stfsux', 727: 'stfdx', 759: 'stfdux',
        790: 'lhbrx', 792: 'sraw', 824: 'srawi', 854: 'eieio', 918: 'sthbrx', 922: 'extsh',
        954: 'extsb', 982: 'icbi', 983: 'stfiwx', 1014: 'dcbz'}
OE_OPS = {8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}

OP59 = {18: 'fdivs', 20: 'fsubs', 21: 'fadds', 24: 'fres', 25: 'fmuls', 28: 'fmsubs', 29: 'fmadds',
        30: 'fnmsubs', 31: 'fnmadds'}
OP63A = {18: 'fdiv', 20: 'fsub', 21: 'fadd', 23: 'fsel', 25: 'fmul', 26: 'frsqrte', 28: 'fmsub',
         29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd'}
OP63X = {0: 'fcmpu', 12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 32: 'fcmpo', 38: 'mtfsb1', 40: 'fneg',
         64: 'mcrfs', 70: 'mtfsb0', 72: 'fmr', 134: 'mtfsfi', 136: 'fnabs', 264: 'fabs', 583: 'mffs',
         711: 'mtfsf'}
OP4X10 = {0: 'ps_cmpu0', 32: 'ps_cmpo0', 40: 'ps_neg', 64: 'ps_cmpu1', 72: 'ps_mr', 96: 'ps_cmpo1',
          136: 'ps_nabs', 264: 'ps_abs', 528: 'ps_merge00', 560: 'ps_merge01', 592: 'ps_merge10',
          624: 'ps_merge11', 1014: 'dcbz_l'}
OP4X6 = {6: 'psq_lx', 7: 'psq_stx', 38: 'psq_lux', 39: 'psq_stux'}
OP4A = {10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1', 14: 'ps_madds0',
        15: 'ps_madds1', 18: 'ps_div', 20: 'ps_sub', 21: 'ps_add', 23: 'ps_sel', 24: 'ps_res',
        25: 'ps_mul', 26: 'ps_rsqrte', 28: 'ps_msub', 29: 'ps_madd', 30: 'ps_nmsub', 31: 'ps_nmadd'}


def decode(w):
    """Mnemonic without '.'/'o' suffixes (use rc(w)/oe(w)); None if not a Gekko instruction."""
    o = w >> 26
    if o in OP:
        return OP[o]
    x10 = (w >> 1) & 0x3FF
    if o == 19:
        return OP19.get(x10)
    if o == 31:
        if x10 in OP31:
            return OP31[x10]
        if (x10 & 0x1FF) in OE_OPS:
            return OP31[x10 & 0x1FF]
        return None
    if o == 59:
        return OP59.get((w >> 1) & 0x1F)
    if o == 63:
        if x10 in OP63X:
            return OP63X[x10]
        return OP63A.get((w >> 1) & 0x1F)
    if o == 4:
        if x10 in OP4X10:
            return OP4X10[x10]
        if ((w >> 1) & 0x3F) in OP4X6:
            return OP4X6[(w >> 1) & 0x3F]
        return OP4A.get((w >> 1) & 0x1F)
    return None


def oe(w):
    return (w >> 10) & 1 and (w >> 26) == 31 and ((w >> 1) & 0x1FF) in OE_OPS


def branch_target(addr, w):
    o = w >> 26
    if o == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
    else:
        li = w & 0xFFFC
        if li & 0x8000:
            li -= 0x10000
    return (li if w & 2 else addr + li) & 0xFFFFFFFF
