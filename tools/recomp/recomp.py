#!/usr/bin/env python3
"""Static recompiler: main.dol (Gekko PPC) -> C for the 3DS runtime.

    PYTHONUTF8=1 python tools/recomp/recomp.py [main.dol] [out_dir]

Game, MSL, MWCC runtime, crt0 and MTX code is translated. SDK modules listed
in STUB_MODULES become calls to sdk_<name>() (weak empty stubs in
gen/sdk_stubs.c, overridden by runtime/). MetroTRK is skipped.
Every translated function is `void f_XXXXXXXX(void)` over the global CPU
state in runtime/ppc.h. Anything the translator cannot decide is collected
and written to docs/recomp_unknowns.md (generated section)."""
import re, struct, sys
from collections import defaultdict
from pathlib import Path
from ppc import *

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / 'data/extracted/sys/main.dol'
OUT = ROOT / 'gen'
SDK_DOC = ROOT / 'docs/sdk_functions.md'
UNKNOWNS = ROOT / 'docs/recomp_unknowns.md'
FUNCS_PER_FILE = 250

STUB_MODULES = {'base', 'OS', 'DB', 'DVD', 'VI', 'PAD', 'AI', 'AR', 'ARQ', 'AX', 'DSP', 'CARD', 'GX',
                'EXI', 'SI', 'THP'}
SKIP_MODULES = {'MetroTRK'}
# C library functions outside STUB_MODULES replaced natively in runtime/libc.c
NATIVE = {0x8000540c: 'memset', 0x800054f4: 'memcpy', 0x80169168: 'sqrt'}
# MTX functions replaced in runtime/mtx.c: calls go to sdk_<name>, the body is still
# translated (f_) and the weak stub calls it (sdmc:/cr_mtx_off.txt and the bench use it)
MTX_NATIVE = {0x801394c4, 0x801394f0, 0x80139524, 0x801395f0, 0x80139640, 0x80139738, 0x80139a40,
              0x80139a74, 0x80139eb4, 0x80139f08, 0x8013a160, 0x8013a184, 0x8013a1a8, 0x8013a1c4,
              0x8013a208, 0x8013a220, 0x8013a264, 0x8013a284}
# SDK functions inside STUB_MODULES that are pure memory code: translated from the guest
# (PADClamp + ClampStick: deadzone 15, circle radius 56/44, triggers 30..180)
# Whole ranges: every instruction must be 'tr', not only the entry (else recomp_bad).
TRANSLATE_SDK_RANGES = [(0x801409b0, 0x80140c64)]   # ClampStick 0x1a8 + PADClamp 0x10c
TRANSLATE_SDK = {0x80140b58, 0x801409b0}
# Functions only reached through pointers the scans miss (leaf, no prologue; address built
# far from its addi). Seen as recomp_bad call_indirect in a played log.
EXTRA_STARTS = {0x800fbc1c}   # campaign scene 6, called from 0x80091af4


# ---------------------------------------------------------------- input
class Dol:
    def __init__(self, path):
        d = path.read_bytes()
        offs = struct.unpack_from('>18I', d, 0)
        addr = struct.unpack_from('>18I', d, 0x48)
        size = struct.unpack_from('>18I', d, 0x90)
        self.entry = struct.unpack_from('>I', d, 0xE0)[0]
        self.secs = [(i, a, d[o:o + s]) for i, (o, a, s) in enumerate(zip(offs, addr, size)) if s]
        self.text = [(a, b) for i, a, b in self.secs if i < 7]
        self.data = [(a, b) for i, a, b in self.secs if i >= 7]

    def word(self, a):
        for base, blob in self.text + self.data:
            if base <= a < base + len(blob) - 3:
                return struct.unpack_from('>I', blob, a - base)[0]
        return None

    def in_text(self, a):
        return any(base <= a < base + len(b) for base, b in self.text)


def load_sdk_doc():
    """-> (ranges [(lo, hi, module)], {addr: name})"""
    text = SDK_DOC.read_text(encoding='utf-8')
    ranges = []
    for m in re.finditer(r'^- ([^:\n]+): (0x[0-9a-f]+)-(0x[0-9a-f]+)\s*$', text, re.M):
        label = m.group(1).strip()
        mod = label.split()[0].lstrip('.')
        mod = {'init_cpp': 'init_cpp', 'MWCC': 'MWCC', 'MSL': 'MSL', 'init': 'crt0'}.get(mod, mod)
        ranges.append((int(m.group(2), 16), int(m.group(3), 16), mod))
    names = {}
    for m in re.finditer(r'^\| (0x[0-9a-f]{8}) \| 0x[0-9a-f]+ \| ([^|]+?) \|', text, re.M):
        names[int(m.group(1), 16)] = m.group(2).strip()
    return ranges, names


# ---------------------------------------------------------------- analysis
class Program:
    def __init__(self, dol):
        self.dol = dol
        self.ranges, self.docnames = load_sdk_doc()
        self.unk = defaultdict(list)  # category -> [(addr, note)]
        self.jt = {}                  # bctr addr -> [targets]

    def module(self, a):
        for lo, hi, mod in self.ranges:
            if lo <= a < hi:
                return mod
        return 'game'

    def kind(self, a):
        """'tr' translate, 'stub' sdk stub, 'skip', or None (not code)."""
        if not self.dol.in_text(a):
            return None
        m = self.module(a)
        if a in NATIVE:
            return 'stub'
        if any(lo <= a < hi for lo, hi in TRANSLATE_SDK_RANGES):
            return 'tr'
        return 'stub' if m in STUB_MODULES else 'skip' if m in SKIP_MODULES else 'tr'

    def insn(self, a):
        w = self.dol.word(a)
        return w, (decode(w) if w is not None else None)

    def all_text_words(self):
        for base, blob in self.dol.text:
            for k in range(0, len(blob), 4):
                yield base + k, struct.unpack_from('>I', blob, k)[0]

    # -- jump tables: lis rT,hi / addi rT,rT,lo ... lwzx rX,rT,rI ... mtctr rX / bctr
    def find_jump_tables(self):
        for a, w in self.all_text_words():
            if w != 0x4E800420 or self.kind(a) != 'tr':
                continue
            got = self._jt_at(a)
            if got:
                self.jt[a] = got

    def _jt_at(self, a):
        back = [(a - 4 * k, self.dol.word(a - 4 * k)) for k in range(1, 40)]
        i = next((k for k, (_, w) in enumerate(back) if decode(w) == 'mtspr' and self._spr(w) == 9), None)
        if i is None:
            self.unk['bctr without mtctr (dynamic tail jump)'].append((a, ''))
            return None
        x = rD(back[i][1])
        j = next((k for k in range(i + 1, len(back)) if decode(back[k][1]) == 'lwzx' and rD(back[k][1]) == x), None)
        if j is None:
            return None  # computed tail call, not a table
        lw = back[j][1]
        base = None
        for tr, idx in ((rA(lw), rB(lw)), (rB(lw), rA(lw))):
            lo = hi = None
            for k in range(j + 1, len(back)):
                ww = back[k][1]; d = decode(ww)
                if lo is None and d == 'addi' and rD(ww) == tr:
                    lo, src = simm(ww), rA(ww)
                elif lo is not None and d == 'addis' and rD(ww) == src and rA(ww) == 0:
                    hi = simm(ww); break
            if lo is not None and hi is not None:
                base, ireg = ((hi << 16) + lo) & 0xFFFFFFFF, idx
                break
        if base is None:
            self.unk['jump table base not lis/addi'].append((a, ''))
            return None
        # bound: cmplwi rI0, N before the index scaling (rlwinm ireg, rI0, 2, ...)
        n = None
        src = ireg
        for k in range(j + 1, len(back)):
            ww = back[k][1]; d = decode(ww)
            if d == 'rlwinm' and rA(ww) == src:
                src = rD(ww)
            elif d == 'cmpli' and rA(ww) == src:
                n = uimm(ww) + 1
                break
        if n is None:
            self.unk['jump table without cmplwi bound (not translated as switch)'].append((a, f'table 0x{base:08x}'))
            return None
        tg = [self.dol.word(base + 4 * k) for k in range(n)]
        if any(t is None or self.kind(t) != 'tr' for t in tg):
            self.unk['jump table entry outside translated text'].append((a, f'table 0x{base:08x}'))
            return None
        self.jt_data = getattr(self, 'jt_data', set()) | {base + 4 * k for k in range(n)}
        return tg

    def lr_from_stack(self, a, reg):
        """Was reg (moved to LR at a) last set by mflr or a load from the stack frame?"""
        for k in range(1, 16):
            w = self.dol.word(a - 4 * k) or 0
            d = decode(w)
            if d == 'mfspr' and self._spr(w) == 8 and rD(w) == reg:
                return True
            if d in ('lwz', 'lwzu') and rD(w) == reg:
                return rA(w) in (1, 11)
            if d in ('b', 'bclr', 'bcctr') or (d and d.startswith('lmw')) or rD(w) == reg and d not in (
                    'stw', 'stwu', 'stb', 'sth', 'stmw', 'cmpi', 'cmpli', 'cmp', 'cmpl', 'bc', 'stfs', 'stfd',
                    'mtspr', 'psq_st'):
                return False
        return False

    @staticmethod
    def _spr(w):
        n = (w >> 11) & 0x3FF
        return ((n & 0x1F) << 5) | (n >> 5)

    # -- function starts
    def find_starts(self):
        st = {self.dol.entry} | {a for a in self.docnames if self.dol.in_text(a)} | EXTRA_STARTS
        for a, w in self.all_text_words():
            d = decode(w)
            if d in ('b', 'bc') and w & 1:
                st.add(branch_target(a, w))
        # pointers to code in data sections (vtables, callbacks, ctors)
        jtd = getattr(self, 'jt_data', set())
        for base, blob in self.dol.data:
            for k in range(0, len(blob) - 3, 4):
                v = struct.unpack_from('>I', blob, k)[0]
                if base + k in jtd or v & 3 or self.kind(v) is None:
                    continue
                w = self.dol.word(v)
                if w and decode(w):
                    st.add(v)
        # lis/addi (or ori) pairs that build a code address in translated code
        lis = {}
        for a, w in self.all_text_words():
            d = decode(w)
            if d == 'addis' and rA(w) == 0:
                lis[rD(w)] = (a, uimm(w) << 16)
            elif d in ('addi', 'ori') and rA(w) in lis and a - lis[rA(w)][0] < 64:
                v = (lis[rA(w)][1] + (simm(w) if d == 'addi' else uimm(w))) & 0xFFFFFFFF
                if not v & 3 and self.kind(v) and decode(self.dol.word(v) or 0):
                    st.add(v)
        # prologue after an unconditional terminator (functions reached only through
        # pointers this scan cannot see, e.g. tables built at run time)
        prev = None
        for a, w in self.all_text_words():
            if prev in (0x4E800020, 0x4E800420) or (prev is not None and prev >> 26 == 18 and not prev & 1):
                if (w >> 16) == 0x9421 or w == 0x7C0802A6:  # stwu r1,-x(r1) / mflr r0
                    st.add(a)
            prev = w
        self.starts = {a for a in st if self.kind(a) is not None}

    def sdk_name(self, a):
        if a in self.stubs:
            return self.stubs[a]
        n = self.docnames.get(a)
        n = 'sdk_' + re.sub(r'\W', '_', n) if n else f'sdk_{self.module(a)}_unlisted_{a:08x}'
        if n in self.stubs.values():
            n += f'_{a:08x}'
        self.stubs[a] = n
        return n

    def callee(self, t):
        k = self.kind(t)
        if t in MTX_NATIVE:
            return f'{self.sdk_name(t)}();'
        if k == 'tr':
            return f'f_{t:08x}();'
        if k == 'stub':
            return f'{self.sdk_name(t)}();'
        if k == 'skip':
            return f'recomp_skipped(0x{t:08x});'
        return None


# ---------------------------------------------------------------- translation
def h(v):
    return f'0x{v & 0xFFFFFFFF:x}'


# Guest registers live in C locals inside a function (r3, f1, cr0..cr7, ctr, ca, so);
# translate_function loads them at entry and after calls and stores the written ones
# to cpu before calls, returns and polls: SPILL / RELOAD mark those points.
SPILL, RELOAD = '@S@', '@L@'


def crbit(n):
    return f'((cr{n // 4} >> {3 - n % 4}) & 1u)'


def setcrbit(n, v):
    k = 3 - n % 4
    return f'cr{n // 4} = (cr{n // 4} & {h(~(1 << k) & 15)}u) | (({v}) & 1u) << {k};'


def cr0(x):
    return f'cr0 = cmp_s((s32)({x}), 0) | so;'


def ea_d(w):
    a, d = rA(w), simm(w)
    if a == 0:
        return h(d) + 'u'
    return f'R[{a}]' + (f' + {d}' if d > 0 else f' - {-d}' if d < 0 else '')


def ea_x(w):
    return f'R[{rB(w)}]' if rA(w) == 0 else f'R[{rA(w)}] + R[{rB(w)}]'


def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


LOADS = {'lwz': ('rd32({})', ''), 'lbz': ('rd8({})', ''), 'lhz': ('rd16({})', ''),
         'lha': ('(u32)(s32)(s16)rd16({})', '')}
STORES = {'stw': 'wr32({}, {})', 'stb': 'wr8({}, (u8){})', 'sth': 'wr16({}, (u16){})'}
X_LOAD = {'lwzx': 'lwz', 'lbzx': 'lbz', 'lhzx': 'lhz', 'lhax': 'lha', 'lwzux': 'lwz', 'lbzux': 'lbz',
          'lhzux': 'lhz', 'lhaux': 'lha'}
X_STORE = {'stwx': 'stw', 'stbx': 'stb', 'sthx': 'sth', 'stwux': 'stw', 'stbux': 'stb', 'sthux': 'sth'}
CROPS = {'crand': '{a} & {b}', 'cror': '{a} | {b}', 'crxor': '{a} ^ {b}', 'crnand': '~({a} & {b})',
         'crnor': '~({a} | {b})', 'creqv': '~({a} ^ {b})', 'crandc': '{a} & ~{b}', 'crorc': '{a} | ~{b}'}


def emit_op(p, a, w, d):
    """Straight-line instruction -> list of C statements. Control flow is handled by the caller."""
    D, A, B, C = rD(w), rA(w), rB(w), rC(w)
    S = D
    rec = rc(w)
    out = []
    o = out.append

    def fpu_note():
        p.unk['float op with Rc=1 (cr1 from FPSCR, not modelled)'].append((a, d))
        o('cr1 = cpu.fpscr >> 28;')

    if oe(w):
        p.unk['OE (overflow-enable) form'].append((a, d))
    # ---- integer arithmetic
    if d == 'addi':
        o(f'R[{D}] = {ea_d(w)};')
    elif d == 'addis':
        o(f'R[{D}] = ' + (h(simm(w) << 16) + 'u;' if A == 0 else f'R[{A}] + {h(simm(w) << 16)}u;'))
    elif d in ('addic', 'addic.'):
        o(f'R[{D}] = addc(R[{A}], {h(simm(w))}u, 0);')
        if d == 'addic.': o(cr0(f'R[{D}]'))
    elif d == 'subfic':
        o(f'R[{D}] = addc(~R[{A}], {h(simm(w))}u, 1);')
    elif d == 'mulli':
        o(f'R[{D}] = R[{A}] * {h(simm(w))}u;')
    elif d in ('add', 'subf', 'addc', 'subfc', 'adde', 'subfe', 'addze', 'subfze', 'addme', 'subfme',
               'neg', 'mullw', 'mulhw', 'mulhwu', 'divw', 'divwu', 'and', 'or', 'xor', 'nand', 'nor',
               'eqv', 'andc', 'orc', 'slw', 'srw', 'sraw'):
        if d in ('and', 'or', 'xor', 'nand', 'nor', 'eqv', 'andc', 'orc', 'slw', 'srw', 'sraw'):
            dst, x, y = A, f'R[{S}]', f'R[{B}]'
        else:
            dst, x, y = D, f'R[{A}]', f'R[{B}]'
        e = {'add': f'{x} + {y}', 'subf': f'{y} - {x}', 'addc': f'addc({x}, {y}, 0)',
             'subfc': f'addc(~{x}, {y}, 1)', 'adde': f'addc({x}, {y}, cpu.xer_ca)',
             'subfe': f'addc(~{x}, {y}, cpu.xer_ca)', 'addze': f'addc({x}, 0, cpu.xer_ca)',
             'subfze': f'addc(~{x}, 0, cpu.xer_ca)', 'addme': f'addc({x}, 0xFFFFFFFFu, cpu.xer_ca)',
             'subfme': f'addc(~{x}, 0xFFFFFFFFu, cpu.xer_ca)', 'neg': f'0u - {x}',
             'mullw': f'{x} * {y}', 'mulhw': f'mulhw({x}, {y})',
             'mulhwu': f'mulhwu({x}, {y})', 'divw': f'divw({x}, {y})', 'divwu': f'divwu({x}, {y})',
             'and': f'{x} & {y}', 'or': f'{x} | {y}', 'xor': f'{x} ^ {y}', 'nand': f'~({x} & {y})',
             'nor': f'~({x} | {y})', 'eqv': f'~({x} ^ {y})', 'andc': f'{x} & ~{y}', 'orc': f'{x} | ~{y}',
             'slw': f'slw({x}, {y})', 'srw': f'srw({x}, {y})', 'sraw': f'sraw({x}, {y})'}[d]
        if d == 'or' and S == B:
            e = x
        o(f'R[{dst}] = {e};')
        if rec: o(cr0(f'R[{dst}]'))
    elif d in ('ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.'):
        imm = uimm(w) << (16 if d.endswith('is') or d == 'andis.' else 0)
        op = {'o': '|', 'x': '^', 'a': '&'}[d[0]]
        o(f'R[{A}] = R[{S}] {op} {h(imm)}u;')
        if d.startswith('and'): o(cr0(f'R[{A}]'))
    elif d in ('extsb', 'extsh', 'cntlzw'):
        e = {'extsb': f'(u32)(s32)(s8)R[{S}]', 'extsh': f'(u32)(s32)(s16)R[{S}]', 'cntlzw': f'cntlzw(R[{S}])'}[d]
        o(f'R[{A}] = {e};')
        if rec: o(cr0(f'R[{A}]'))
    elif d == 'srawi':
        o(f'R[{A}] = srawi(R[{S}], {B});')
        if rec: o(cr0(f'R[{A}]'))
    elif d in ('rlwinm', 'rlwnm', 'rlwimi'):
        m = mask((w >> 6) & 31, (w >> 1) & 31)
        rot = f'rotl(R[{S}], R[{B}])' if d == 'rlwnm' else (f'rotl(R[{S}], {B})' if B else f'R[{S}]')
        if d == 'rlwimi':
            o(f'R[{A}] = ({rot} & {h(m)}u) | (R[{A}] & {h(~m)}u);')
        else:
            o(f'R[{A}] = {rot} & {h(m)}u;')
        if rec: o(cr0(f'R[{A}]'))
    # ---- compare
    elif d in ('cmpi', 'cmpli', 'cmp', 'cmpl'):
        f = crfD(w)
        if d == 'cmpi': o(f'cr{f} = cmp_s((s32)R[{A}], {simm(w)}) | so;')
        elif d == 'cmpli': o(f'cr{f} = cmp_u(R[{A}], {h(uimm(w))}u) | so;')
        elif d == 'cmp': o(f'cr{f} = cmp_s((s32)R[{A}], (s32)R[{B}]) | so;')
        else: o(f'cr{f} = cmp_u(R[{A}], R[{B}]) | so;')
    # ---- loads / stores
    elif d in LOADS or d[:-1] in LOADS and d.endswith('u'):
        base = d if d in LOADS else d[:-1]
        if base == d:
            o(f'R[{D}] = {LOADS[base][0].format(ea_d(w))};')
        else:
            o(f'{{ u32 ea = {ea_d(w)}; R[{D}] = {LOADS[base][0].format("ea")}; R[{A}] = ea; }}')
    elif d in X_LOAD:
        if d.endswith('ux'):
            o(f'{{ u32 ea = {ea_x(w)}; R[{D}] = {LOADS[X_LOAD[d]][0].format("ea")}; R[{A}] = ea; }}')
        else:
            o(f'R[{D}] = {LOADS[X_LOAD[d]][0].format(ea_x(w))};')
    elif d in STORES or d[:-1] in STORES and d.endswith('u'):
        base = d if d in STORES else d[:-1]
        if base == d:
            o(STORES[base].format(ea_d(w), f'R[{S}]') + ';')
        else:
            o(f'{{ u32 ea = {ea_d(w)}; {STORES[base].format("ea", f"R[{S}]")}; R[{A}] = ea; }}')
    elif d in X_STORE:
        if d.endswith('ux'):
            o(f'{{ u32 ea = {ea_x(w)}; {STORES[X_STORE[d]].format("ea", f"R[{S}]")}; R[{A}] = ea; }}')
        else:
            o(STORES[X_STORE[d]].format(ea_x(w), f'R[{S}]') + ';')
    elif d == 'lwbrx':
        o(f'R[{D}] = __builtin_bswap32(rd32({ea_x(w)}));')
    elif d == 'lhbrx':
        o(f'R[{D}] = __builtin_bswap16(rd16({ea_x(w)}));')
    elif d == 'stwbrx':
        o(f'wr32({ea_x(w)}, __builtin_bswap32(R[{S}]));')
    elif d == 'sthbrx':
        o(f'wr16({ea_x(w)}, __builtin_bswap16((u16)R[{S}]));')
    elif d == 'lmw':
        o(f'{{ u32 ea = {ea_d(w)}; ' + ' '.join(f'R[{k}] = rd32(ea + {4 * (k - D)});' for k in range(D, 32)) + ' }')
    elif d == 'stmw':
        o(f'{{ u32 ea = {ea_d(w)}; ' + ' '.join(f'wr32(ea + {4 * (k - S)}, R[{k}]);' for k in range(S, 32)) + ' }')
    elif d == 'lwarx':
        o(f'R[{D}] = rd32({ea_x(w)});')
    elif d == 'stwcx.':
        o(f'wr32({ea_x(w)}, R[{S}]); cr0 = 2 | so;')
    # ---- float loads / stores
    elif d in ('lfs', 'lfsu', 'lfsx', 'lfsux', 'lfd', 'lfdu', 'lfdx', 'lfdux',
               'stfs', 'stfsu', 'stfsx', 'stfsux', 'stfd', 'stfdu', 'stfdx', 'stfdux', 'stfiwx'):
        ea = ea_x(w) if d.endswith('x') else ea_d(w)
        upd = d.endswith('u') or d.endswith('ux')
        if d.startswith('lfs'):
            body = f'{{ double v = u2f(rd32(ea)); F[{D}].ps0 = v; F[{D}].ps1 = v; }}'
        elif d.startswith('lfd'):
            body = f'F[{D}].ps0 = u2d(rd64(ea));'
        elif d.startswith('stfs'):
            body = f'wr32(ea, f2u((float)F[{S}].ps0));'
        elif d == 'stfiwx':
            body = f'wr32(ea, (u32)d2u(F[{S}].ps0));'
        else:
            body = f'wr64(ea, d2u(F[{S}].ps0));'
        o(f'{{ u32 ea = {ea}; {body}' + (f' R[{A}] = ea;' if upd else '') + ' }')
    elif d in ('psq_l', 'psq_lu', 'psq_st', 'psq_stu'):
        dd = w & 0xFFF
        dd = dd - 0x1000 if dd & 0x800 else dd
        W, I = (w >> 15) & 1, (w >> 12) & 7
        ea = (f'R[{A}] + {dd}' if A or d.endswith('u') else h(dd) + 'u')
        op = f'F[{D}] = psq_ld(ea, {W}, {I});' if 'l' in d[4:] else f'psq_st(F[{D}], ea, {W}, {I});'
        o(f'{{ u32 ea = {ea}; {op}' + (f' R[{A}] = ea;' if d.endswith('u') else '') + ' }')
    elif d in ('psq_lx', 'psq_stx', 'psq_lux', 'psq_stux'):
        W, I = (w >> 10) & 1, (w >> 7) & 7
        op = f'F[{D}] = psq_ld(ea, {W}, {I});' if d.startswith('psq_l') else f'psq_st(F[{D}], ea, {W}, {I});'
        upd = 'ux' in d
        o(f'{{ u32 ea = {ea_x(w)}; {op}' + (f' R[{A}] = ea;' if upd else '') + ' }')
    # ---- floating point (Dolphin semantics: single ops round via fsgl, c via f25)
    elif d in OP59.values() or d in OP63A.values():
        fa, fb, fc = f'F[{A}].ps0', f'F[{B}].ps0', f'F[{C}].ps0'
        single = d in OP59.values()
        if single:  # SADD/SMUL/SMADD... (ppc.h): rounding to single included, RECOMP_FLOATOPS = float math
            e = {'fdiv': f'SDIV({fa}, {fb})', 'fsub': f'SSUB({fa}, {fb})', 'fadd': f'SADD({fa}, {fb})',
                 'fmul': f'SMUL({fa}, {fc})', 'fmsub': f'SMSUB({fa}, {fc}, {fb})', 'fmadd': f'SMADD({fa}, {fc}, {fb})',
                 'fnmsub': f'SMSUB({fa}, {fc}, {fb})', 'fnmadd': f'SMADD({fa}, {fc}, {fb})',
                 'fres': f'ppc_fres({fb})'}
        else:
            e = {'fdiv': f'{fa} / {fb}', 'fsub': f'{fa} - {fb}', 'fadd': f'{fa} + {fb}',
                 'fmul': f'{fa} * {fc}', 'fmsub': f'fma({fa}, {fc}, -{fb})', 'fmadd': f'fma({fa}, {fc}, {fb})',
                 'fnmsub': f'fma({fa}, {fc}, -{fb})', 'fnmadd': f'fma({fa}, {fc}, {fb})',
                 'fsel': f'fsel({fa}, {fc}, {fb})', 'frsqrte': f'ppc_frsqrte({fb})'}
        base = d[:-1] if d.endswith('s') and d[:-1] in e else d
        v = e[base]
        if d in ('fres',):
            p.unk['fres/frsqrte estimate (needs Dolphin table)'].append((a, d))
            o(f'{{ double v = fsgl({v}); F[{D}].ps0 = v; F[{D}].ps1 = v; }}')
        elif d == 'frsqrte':
            p.unk['fres/frsqrte estimate (needs Dolphin table)'].append((a, d))
            o(f'F[{D}].ps0 = {v};')
        elif single:
            if base in ('fnmsub', 'fnmadd'): v = f'fnoneg({v})'
            o(f'{{ double v = {v}; F[{D}].ps0 = v; F[{D}].ps1 = v; }}')
        else:
            if base in ('fnmsub', 'fnmadd'): v = f'fnoneg({v})'
            o(f'F[{D}].ps0 = {v};')
        if rec: fpu_note()
    elif d in ('fcmpu', 'fcmpo'):
        o(f'cr{crfD(w)} = fcmp_v(F[{A}].ps0, F[{B}].ps0);')
    elif d in ('fmr', 'fneg', 'fabs', 'fnabs'):
        e = {'fmr': f'F[{B}].ps0', 'fneg': f'u2d(d2u(F[{B}].ps0) ^ 0x8000000000000000ull)',
             'fabs': f'u2d(d2u(F[{B}].ps0) & ~0x8000000000000000ull)',
             'fnabs': f'u2d(d2u(F[{B}].ps0) | 0x8000000000000000ull)'}[d]
        o(f'F[{D}].ps0 = {e};')
        if rec: fpu_note()
    elif d == 'frsp':
        o(f'{{ double v = fsgl(F[{B}].ps0); F[{D}].ps0 = v; F[{D}].ps1 = v; }}')
        if rec: fpu_note()
    elif d in ('fctiw', 'fctiwz'):
        o(f'F[{D}].ps0 = u2d(fctiw_bits(F[{B}].ps0, {int(d == "fctiwz")}));')
        if rec: fpu_note()
    elif d == 'mffs':
        o(f'F[{D}].ps0 = u2d(0xFFF8000000000000ull | cpu.fpscr);')
    elif d == 'mtfsf':
        p.unk['FPSCR write (rounding mode not modelled on ARM)'].append((a, d))
        o(f'ppc_mtfsf({(w >> 17) & 0xFF}, d2u(F[{B}].ps0));')
    elif d in ('mtfsb0', 'mtfsb1'):
        p.unk['FPSCR write (rounding mode not modelled on ARM)'].append((a, d))
        o(f'cpu.fpscr {"|=" if d[-1] == "1" else "&= ~"}(0x80000000u >> {D});')
    elif d in ('mtfsfi', 'mcrfs'):
        p.unk['unhandled instruction'].append((a, d))
        o(f'recomp_bad(0x{a:08x}, "{d}");')
    # ---- paired singles
    elif d.startswith('ps_'):
        a0, a1, b0, b1, c0, c1 = (f'F[{A}].ps0', f'F[{A}].ps1', f'F[{B}].ps0', f'F[{B}].ps1',
                                  f'F[{C}].ps0', f'F[{C}].ps1')
        two = None
        if d in ('ps_add', 'ps_sub', 'ps_div'):
            op = {'ps_add': 'SADD', 'ps_sub': 'SSUB', 'ps_div': 'SDIV'}[d]
            two = (f'{op}({a0}, {b0})', f'{op}({a1}, {b1})')
        elif d == 'ps_mul':
            two = (f'SMUL({a0}, {c0})', f'SMUL({a1}, {c1})')
        elif d in ('ps_madd', 'ps_msub', 'ps_nmadd', 'ps_nmsub'):
            m = 'SMSUB' if 'sub' in d else 'SMADD'
            two = tuple(f'{m}({x}, {c}, {y})' for x, c, y in ((a0, c0, b0), (a1, c1, b1)))
            if d.startswith('ps_n'):
                two = tuple(f'fnoneg({t})' for t in two)
        elif d in ('ps_muls0', 'ps_muls1'):
            c = c0 if d.endswith('0') else c1
            two = (f'SMUL({a0}, {c})', f'SMUL({a1}, {c})')
        elif d in ('ps_madds0', 'ps_madds1'):
            c = c0 if d.endswith('0') else c1
            two = (f'SMADD({a0}, {c}, {b0})', f'SMADD({a1}, {c}, {b1})')
        elif d == 'ps_sum0':
            two = (f'SADD({a0}, {b1})', f'fsgl({c1})')
        elif d == 'ps_sum1':
            two = (f'fsgl({c0})', f'SADD({a0}, {b1})')
        elif d.startswith('ps_merge'):
            x, y = d[-2], d[-1]
            two = (a1 if x == '1' else a0, b1 if y == '1' else b0)
        elif d == 'ps_mr':
            two = (b0, b1)
        elif d in ('ps_neg', 'ps_abs', 'ps_nabs'):
            fx = {'ps_neg': '^ 0x8000000000000000ull', 'ps_abs': '& ~0x8000000000000000ull',
                  'ps_nabs': '| 0x8000000000000000ull'}[d]
            two = (f'u2d(d2u({b0}) {fx})', f'u2d(d2u({b1}) {fx})')
        elif d == 'ps_sel':
            two = (f'fsel({a0}, {c0}, {b0})', f'fsel({a1}, {c1}, {b1})')
        elif d in ('ps_res', 'ps_rsqrte'):
            p.unk['fres/frsqrte estimate (needs Dolphin table)'].append((a, d))
            fn = 'ppc_fres' if d == 'ps_res' else 'ppc_frsqrte'
            two = (f'fsgl({fn}({b0}))', f'fsgl({fn}({b1}))')
        elif d in ('ps_cmpu0', 'ps_cmpo0', 'ps_cmpu1', 'ps_cmpo1'):
            k = 'ps1' if d.endswith('1') else 'ps0'
            o(f'cr{crfD(w)} = fcmp_v(F[{A}].{k}, F[{B}].{k});')
        elif d == 'dcbz_l':
            p.unk['unhandled instruction'].append((a, d))
            o(f'recomp_bad(0x{a:08x}, "dcbz_l");')
        if two:
            o(f'{{ double v0 = {two[0]}, v1 = {two[1]}; F[{D}].ps0 = v0; F[{D}].ps1 = v1; }}')
            if rec: fpu_note()
    # ---- CR / SPR / system
    elif d in CROPS:
        o(setcrbit(D, CROPS[d].format(a=crbit(A), b=crbit(B))))
    elif d == 'mcrf':
        o(f'cr{crfD(w)} = cr{crfS(w)};')
    elif d == 'mcrxr':
        o(f'@CA_OUT@ cr{crfD(w)} = get_xer() >> 28; cpu.xer_so = cpu.xer_ov = cpu.xer_ca = 0; so = 0;')
    elif d == 'mfcr':
        o(f'R[{D}] = ' + ' | '.join(f'cr{i} << {28 - 4 * i}' for i in range(8)) + ';')
    elif d == 'mtcrf':
        crm = (w >> 12) & 0xFF
        o(' '.join(f'cr{i} = (R[{S}] >> {28 - 4 * i}) & 15;' for i in range(8) if crm & (0x80 >> i)))
    elif d in ('mfspr', 'mtspr'):
        n = p._spr(w)
        fld = {8: 'cpu.lr', 9: 'cpu.ctr'}.get(n) or (f'cpu.gqr[{n - 912}]' if 912 <= n < 920 else None)
        if d == 'mfspr':
            o(f'R[{D}] = {fld};' if fld else (f'@CA_OUT@ R[{D}] = get_xer();' if n == 1 else f'R[{D}] = ppc_mfspr({n});'))
        else:
            o(f'{fld} = R[{S}];' if fld else (f'set_xer(R[{S}]); @CA_IN@ so = cpu.xer_so;' if n == 1 else f'ppc_mtspr({n}, R[{S}]);'))
        if not fld and n != 1:
            p.unk['system SPR access in translated code'].append((a, f'{d} {n}'))
    elif d == 'mftb':
        o(f'R[{D}] = ppc_mftb({p._spr(w)});')
    elif d == 'mfmsr':
        o(f'R[{D}] = cpu.msr;')
    elif d == 'mtmsr':
        p.unk['system instruction in translated code'].append((a, d))
        o(f'ppc_mtmsr(R[{S}]);')
    elif d in ('mfsr', 'mtsr', 'mfsrin', 'mtsrin', 'tlbie', 'tlbsync', 'eciwx', 'ecowx'):
        p.unk['system instruction in translated code'].append((a, d))
        o(f'recomp_bad(0x{a:08x}, "{d}");')
    elif d == 'dcbz':
        o(f'dcbz({ea_x(w)});')
    elif d in ('dcbst', 'dcbf', 'dcbi', 'dcbt', 'dcbtst', 'icbi', 'sync', 'isync', 'eieio'):
        o(f'/* {d} */')
    elif d in ('twi', 'tw'):
        p.unk['trap instruction in translated code'].append((a, d))
        o(f'ppc_trap(0x{a:08x});')
    elif d == 'sc':
        p.unk['system instruction in translated code'].append((a, d))
        o(f'ppc_sc(0x{a:08x});')
    else:
        raise ValueError(f'no emitter for {d} at {a:08x}')
    if A == 1 and (d in LOADS or d in STORES or d[:-1] in LOADS or d[:-1] in STORES or
                   d in ('lmw', 'stmw') or d[:3] in ('lfs', 'lfd', 'stf')):
        out = [re.sub(r'\b(rd|wr)(8|16|32|64)\(', r'\1\2_ram(', x) for x in out]
    return out


def cond_expr(w):
    """bc/bclr/bcctr BO/BI -> (pre statements, C condition or None for always)."""
    bo, bi = rD(w), rA(w)
    pre, parts = [], []
    if not bo & 4:
        pre.append('cpu.ctr--;')
        parts.append('cpu.ctr == 0' if bo & 2 else 'cpu.ctr != 0')
    if not bo & 0x10:
        parts.append(crbit(bi) if bo & 8 else '!' + crbit(bi))
    return pre, (' && '.join(parts) or None)


# MWCC's register save/restore entry chains (_savefpr_N, _restfpr_N, _savegpr_N,
# _restgpr_N: stfd/lfd/stw/lwz rN..r31 at r11 - k, then blr). A `bl` into one is
# emitted inline (same stores/loads, on the caller's locals), not as a call.
SAVE_RESTORE = [(0x8015f8fc, 0x8015f944), (0x8015f948, 0x8015f990), (0x8015f994, 0x8015f9dc),
                (0x8015f9e0, 0x8015fa28)]   # [first entry, blr]


def inline_helper(t):
    return any(lo <= t < hi for lo, hi in SAVE_RESTORE)


def inline_body(p, t):
    """The helper's instructions from entry t to its blr; r11 is the caller's frame
    pointer (addi r11, r1, N), so the accesses skip the hardware check like r1's."""
    out = []
    while True:
        w, d = p.insn(t)
        if d == 'bclr':
            return out
        assert d in ('stw', 'lwz', 'stfd', 'lfd') and rA(w) == 11, f'save/restore helper {t:08x}: {d}'
        out += [re.sub(r'\b(rd|wr)(32|64)\(', r'\1\2_ram(', x) for x in emit_op(p, t, w, d)]
        t += 4


def translate_function(p, entry):
    """Walk intra-procedural flow from entry -> Func (per-instruction C, see localize).
    Bodies may overlap (code is duplicated)."""
    body, todo = {}, [entry]
    labels = set()
    lines_for = {}

    def flow(src, t, is_branch):
        """C for transferring control to t: goto (intra) or a tail call."""
        k = p.kind(t)
        if k == 'tr':
            tail = t != entry and t in p.starts and is_branch and not (entry < t <= src)
            if not tail:
                labels.add(t)
                if t not in body:
                    todo.append(t)
                # loop back-edge: pending "interrupts" (ARQ DMA done) are taken here
                if is_branch and t <= src:
                    return f'if (__builtin_expect(recomp_event_pending, 0)) {{ recomp_poll_events(); }} goto L_{t:08x};'
                return f'goto L_{t:08x};'
            return f'f_{t:08x}(); return;'
        c = p.callee(t) if t in p.starts or k == 'stub' and t in p.docnames else None
        if c:
            return c + ' return;'
        p.unk['flow into non-translated code that is not a known entry'].append((src, f'-> 0x{t:08x}'))
        return f'recomp_bad(0x{src:08x}, "jump 0x{t:08x}"); return;'

    while todo:
        a = todo.pop()
        while a not in body:
            if p.kind(a) != 'tr':
                body[a] = [flow(a - 4, a, False)]
                break
            w, d = p.insn(a)
            nxt = a + 4
            L = []
            body[a] = L
            if d is None:
                p.unk['invalid instruction reached'].append((a, f'{w:08x}' if w is not None else 'no data'))
                L.append(f'recomp_bad(0x{a:08x}, "invalid insn"); return;')
                break
            if d == 'b':
                t = branch_target(a, w)
                if w & 1 and inline_helper(t):
                    L.append(f'cpu.lr = 0x{nxt:08x}u;')
                    L.extend(inline_body(p, t))
                    a = nxt
                    continue
                if w & 1:
                    c = p.callee(t)
                    L.append(f'cpu.lr = 0x{nxt:08x}u; ' + (c or f'recomp_bad(0x{a:08x}, "call 0x{t:08x}");'))
                    a = nxt
                    continue
                L.append(flow(a, t, True))
                break
            if d == 'bc':
                t = branch_target(a, w)
                pre, cnd = cond_expr(w)
                L.extend(pre)
                if w & 1:
                    c = p.callee(t) or f'recomp_bad(0x{a:08x}, "call");'
                    L.append(f'if ({cnd or 1}) {{ cpu.lr = 0x{nxt:08x}u; {c} }}')
                else:
                    L.append(f'if ({cnd or 1}) {{ {flow(a, t, True)} }}' if cnd else flow(a, t, True))
                    if not cnd:
                        break
                a = nxt
                continue
            if d in ('bclr', 'bcctr'):
                pre, cnd = cond_expr(w)
                L.extend(pre)
                reg = 'cpu.lr' if d == 'bclr' else 'cpu.ctr'
                if w & 1:
                    L.append(f'if ({cnd or 1}) {{ u32 t = {reg}; cpu.lr = 0x{nxt:08x}u; call_indirect(t); }}')
                    a = nxt
                    continue
                if d == 'bclr':
                    act = 'return;'
                elif a in p.jt:
                    cases = ' '.join(f'case 0x{t:08x}u: goto L_{t:08x};' for t in sorted(set(p.jt[a])))
                    for t in set(p.jt[a]):
                        labels.add(t)
                        if t not in body:
                            todo.append(t)
                    act = f'switch (cpu.ctr) {{ {cases} default: recomp_bad(0x{a:08x}, "jump table"); return; }}'
                else:
                    act = 'call_indirect(cpu.ctr); return;'
                L.append(f'if ({cnd}) {{ {act} }}' if cnd else act)
                if not cnd:
                    break
                a = nxt
                continue
            if d == 'rfi':
                p.unk['system instruction in translated code'].append((a, d))
                L.append(f'ppc_rfi(0x{a:08x}); return;')
                break
            if d == 'mtspr' and p._spr(w) == 8 and not p.lr_from_stack(a, rD(w)):
                p.unk['mtlr from a non-stack value (blr treated as return)'].append((a, ''))
            L.extend(emit_op(p, a, w, d))
            a = nxt
    p.covered.update(body)
    # emit in address order; non-terminal blocks not followed by their successor get a goto
    order = sorted(body)
    term = lambda x: x.endswith('return;') or x.startswith(('goto ', 'if (__builtin_expect(recomp_event_pending', 'switch'))
    gotos = set()
    for i, a in enumerate(order):
        if not term(body[a][-1]) and (i + 1 == len(order) or order[i + 1] != a + 4):
            assert a + 4 in body, f'fallthrough target missing {a + 4:08x}'
            labels.add(a + 4)
            gotos.add(a)
    return Func(entry, order, body, labels, gotos)


# ---------------------------------------------------------------- registers in locals
# Guest registers are C locals inside a function, loaded at entry; the `cpu` copies are
# what other functions and the runtime see. Each call, return, poll and system call is
# an event with its own store (spill) and load (reload) set:
#  - call to G: store the dirty (written since the last store) registers of CALLU that G
#    may read (in(G)) or may change while they are still live; reload the RELU registers G
#    may change (clob(G)). in/clob are a fixpoint over the call graph (summarize()).
#  - tail branch: store the dirty CALLU | RETU registers. return: the dirty RETU ones.
#  - poll: only r1 (callbacks run on a stack below it; recomp_call_guest saves and
#    restores all of cpu, so no reload). recomp_bad, sc, trap, rfi, mtmsr: everything.
# The universes are the old fixed sets, so a store is only ever skipped, never added:
# PPC EABI: a callee reads r1, r3-r10, f1-f8 (varargs: cr1 bit 6) and preserves r1, r2,
# r13-r31, f14-f31, cr2-cr4; the caller reads r3/r4/f1-f4 back. The `cpu` copies of
# r14-r31 etc. may be stale, which only _savegpr/_restgpr stack slots see.
GB = lambda *ks: sum(1 << k for k in ks)
FB = lambda *ks: sum(1 << (32 + k) for k in ks)
CB = lambda *ks: sum(1 << (64 + k) for k in ks)
CTR, CA, SO = 1 << 72, 1 << 73, 1 << 74
ALLREGS = (1 << 75) - 1
VOLG = (0, *range(3, 13))
CALLU = GB(1, *VOLG) | FB(*range(1, 9)) | CB(1) | SO          # stored before a call
RETU = GB(*VOLG) | FB(*range(1, 5)) | CTR | CA | SO           # stored before a return
RELU = GB(*VOLG) | FB(*range(14)) | CB(0, 1, 5, 6, 7) | CTR | CA | SO   # reloaded after a call
NAME_RE = re.compile(r'(?<![\w.])(r\d+|fp\d+|cr[0-7]|ctr|ca|so)\b')
DEF_RE = re.compile(r'(?<![\w.])(r\d+|fp\d+|cr[0-7]|ctr|ca|so)(\.ps[01])? = ')
EVENT_RE = re.compile(
    r'(?P<call>(?:f_[0-9a-f]{8}|sdk_\w+|call_indirect)\([^;\n{]*\);)(?P<cret> return;)?'
    r'|(?P<sys>(?:recomp_bad|recomp_skipped|recomp_poll_events|ppc_mtmsr|ppc_sc|ppc_trap|ppc_rfi)'
    r'\([^;\n{]*\);)(?P<sret> return;)?'
    r'|(?<![\w@])(?P<ret>return;)')
# SDK natives (runtime/, sdk_stubs.c) read any CALLU register (LOG() prints r3-r6) and
# write only r3, r4 and f1 (guest callbacks run through recomp_call_guest, which restores
# all of cpu). check_native_writes() enforces this on runtime/*.c. Exceptions:
NATIVE_IN, NATIVE_CLOB = CALLU, GB(3, 4) | FB(1)
NATIVE_REGS = {'sdk_memcpy': (GB(3, 4, 5), 0), 'sdk_memset': (GB(3, 4, 5), 0),
               'sdk_sqrt': (FB(1), FB(1))}
# runtime/ files allowed other register writes: boot (cpu_init), recomp_call_guest (saves
# and restores cpu), the MTX bench (saves and restores cpu) and PSMTXConcat's r6 (MTX_REGS)
NATIVE_WRITE_OK = {'cpu.c', 'mtx.c'}
# MTX natives (runtime/mtx.c): the translated body's summary plus what the native reads/writes
MTX_REGS = (GB(3, 4, 5) | FB(1, 2, 3), GB(3, 6) | FB(1))


def check_native_writes():
    """runtime/*.c may write guest registers r3, r4 and f1 only (NATIVE_CLOB)."""
    bad = []
    for f in sorted((ROOT / 'runtime').glob('*.c')):
        if f.name in NATIVE_WRITE_OK:
            continue
        for n, line in enumerate(f.read_text(encoding='utf-8').splitlines(), 1):
            for m in re.finditer(r'\b(R|F)\[(\w+)\](?:\.ps[01])?\s*[-+*/|&^]?=(?!=)'
                                 r'|\bcpu\.(gpr|fpr|cr|ctr|xer_\w+)\b\s*[-+|&^]?=(?!=)', line):
                if m.group(1) and (m.group(1), m.group(2)) in (('R', '3'), ('R', '4'), ('F', '1')):
                    continue
                bad.append(f'{f.name}:{n}: {line.strip()}')
    if bad:
        sys.exit('guest register written by a native outside NATIVE_CLOB (recomp.py):\n' + '\n'.join(bad))


def popc(m):
    return bin(m).count('1')


def bit(name):
    if name[0] == 'r':
        return 1 << int(name[1:])
    if name[:2] == 'fp':
        return 1 << (32 + int(name[2:]))
    if name[:2] == 'cr' and name != 'ctr':
        return 1 << (64 + int(name[2:]))
    return {'ctr': CTR, 'ca': CA, 'so': SO}[name]


def usedef(text):
    """C of one instruction (locals) -> (uses, kills, writes). Uses count as before the
    writes; a write to one half of a paired single also reads the register."""
    kill = write = 0
    halves = defaultdict(set)

    def d(m):
        nonlocal kill, write
        b = bit(m.group(1))
        write |= b
        if m.group(2):
            halves[b].add(m.group(2))
        else:
            kill |= b
        return '@'
    t = DEF_RE.sub(d, text)
    if '&ca' in t:
        t = t.replace('&ca', '@')
        kill |= CA; write |= CA
    if re.search(r'(?<![\w.])ctr--', t):
        kill |= CTR; write |= CTR
    use = 0
    for m in NAME_RE.finditer(t):
        use |= bit(m.group(1))
    for b, hs in halves.items():
        if len(hs) == 2:
            kill |= b
        else:
            use |= b
    return use, kill, write


class Func:
    """One translated function: per-instruction C (guest registers as locals, events as
    @S<n>@ / @L<n>@ markers) plus what the register analysis needs."""

    def __init__(self, entry, order, body, labels, gotos):
        self.entry, self.order, self.labels, self.gotos = entry, order, labels, gotos
        self.lines, self.events = {}, []
        self.use, self.kill, self.write, self.ev, self.succ = {}, {}, {}, {}, {}
        term = lambda x: x.endswith('return;') or x.startswith(('goto ', 'if (__builtin_expect(recomp_event_pending', 'switch'))
        for a in order:
            evs, out = [], []
            for x in body[a]:
                cond = x.startswith(('if (', 'switch'))
                out.append(localize_line(EVENT_RE.sub(lambda m: self._event(m, cond, evs), x)))
            self.lines[a], self.ev[a] = out, evs
            self.use[a], self.kill[a], self.write[a] = usedef(re.sub(r'@[SL]\d+@', '', ' '.join(out)))
            s = {int(t, 16) for x in body[a] for t in re.findall(r'goto L_([0-9a-f]{8})', x)}
            if not term(body[a][-1]):
                s.add(a + 4)
            self.succ[a] = s
        self.W = 0
        for a in order:
            self.W |= self.write[a]
        text = '\n'.join(x for a in order for x in self.lines[a])
        self.text = text
        self.gu = sorted({int(x) for x in re.findall(r'\br(\d+)\b', text)})
        self.fu = sorted({int(x) for x in re.findall(r'\bfp(\d+)\b', text)})
        self.cu = sorted({int(x) for x in re.findall(r'\bcr([0-7])\b', text)})
        self.xu = [n for n in ('ctr', 'ca', 'so') if re.search(rf'\b{n}\b', text)]
        self.used = GB(*self.gu) | FB(*self.fu) | CB(*self.cu) | sum(bit(n) for n in self.xu)
        # cpu.xer_* written directly (mfxer/mtxer/mcrxr)
        self.direct = CA | SO if re.search(r'set_xer\(|cpu\.xer_ca = ', text) else 0
        self.pred = defaultdict(set)
        for a in order:
            for t in self.succ[a]:
                self.pred[t].add(a)

    def _event(self, m, cond, evs):
        i = len(self.events)
        if m.group('call'):
            c = m.group('call')
            callee = c[:c.index('(')]
            kind = 'tail' if m.group('cret') else 'call'
            self.events.append((kind, callee, cond))
            evs.append(i)
            return f'@S{i}@ {c} return;' if kind == 'tail' else f'@S{i}@ {c} @L{i}@'
        if m.group('sys'):
            c = m.group('sys')
            kind = 'poll' if c.startswith('recomp_poll_events') else 'sysret' if m.group('sret') else 'sys'
            self.events.append((kind, None, cond))
            evs.append(i)
            return f'@S{i}@ {c} return;' if kind == 'sysret' else f'@S{i}@ {c} @L{i}@'
        self.events.append(('ret', None, cond))
        evs.append(i)
        return f'@S{i}@ return;'

    def callees(self):
        return {c for k, c, _ in self.events if k in ('call', 'tail')}


def localize_line(x):
    x = re.sub(r'\bR\[(\d+)\]', r'r\1', x)
    x = re.sub(r'\bF\[(\d+)\]', r'fp\1', x)
    x = x.replace('cpu.ctr', 'ctr').replace('cpu.xer_ca', 'ca').replace('cpu.xer_so', 'so')
    x = x.replace('@CA_OUT@', 'cpu.xer_ca = ca;').replace('@CA_IN@', 'ca = cpu.xer_ca;')
    return re.sub(r'\b(addc|srawi|sraw)\(', r'\1_l(&ca, ', x)


class Summaries:
    """in(F): CALLU registers whose `cpu` value at entry F may read; clob(F): registers
    whose `cpu` value F may change. Least fixpoint over the call graph."""

    def __init__(self, p, funcs):
        self.p, self.funcs = p, {f.entry: f for f in funcs}
        self.s = {e: (0, 0) for e in self.funcs}
        self.name_addr = {n: a for a, n in p.stubs.items()}

    def get(self, callee):
        if callee.startswith('f_'):
            return self.s.get(int(callee[2:], 16), (CALLU, RELU))
        a = self.name_addr.get(callee)
        if a in MTX_NATIVE and a in self.s:
            i, c = self.s[a]
            return i | MTX_REGS[0], c | MTX_REGS[1]
        if callee == 'call_indirect':
            return CALLU, RELU
        return NATIVE_REGS.get(callee, (NATIVE_IN, NATIVE_CLOB))

    def dep(self, callee):
        """translated function whose summary get(callee) reads"""
        if callee.startswith('f_'):
            return int(callee[2:], 16)
        a = self.name_addr.get(callee)
        return a if a in MTX_NATIVE else None

    def solve(self):
        callers = defaultdict(set)
        for f in self.funcs.values():
            for c in f.callees():
                d = self.dep(c)
                if d is not None:
                    callers[d].add(f.entry)
        work = sorted(self.funcs)
        inq = set(work)
        while work:
            e = work.pop()
            inq.discard(e)
            new = analyze(self.funcs[e], self.get)[:2]
            if new != self.s[e]:
                self.s[e] = new
                for c in callers[e]:
                    if c not in inq:
                        work.append(c)
                        inq.add(c)


def liveness(fn, get, extra):
    """live-in per instruction (backward): instruction reads, then at its events a call
    reads in(callee), a poll r1, and extra[a] (stores of the events)."""
    order, ev, events = fn.order, fn.ev, fn.events
    live = {a: 0 for a in order}
    changed = True
    while changed:
        changed = False
        for a in reversed(order):
            mid = extra.get(a, 0)
            for t in fn.succ[a]:
                mid |= live[t]
            for i in ev[a]:
                k, c, _ = events[i]
                if k in ('call', 'tail'):
                    mid |= get(c)[0] & CALLU
                elif k == 'poll':
                    mid |= GB(1)
            v = fn.use[a] | (mid & ~fn.kill[a])
            if v != live[a]:
                live[a], changed = v, True
    return live


def dirty_sites(fn, get, live):
    """Dirty locals (forward, may: written since the last store or reload) -> store and
    reload mask per event."""
    order, ev, events = fn.order, fn.ev, fn.events
    live_out = {}
    for a in order:
        o = 0
        for t in fn.succ[a]:
            o |= live[t]
        live_out[a] = o
    W, used = fn.W, fn.used
    din = {a: 0 for a in order}
    dout = dict.fromkeys(order)
    sites = {}
    work = list(reversed(order))
    while work:
        a = work.pop()
        d = din[a] | fn.write[a]
        for i in ev[a]:
            k, c, cond = events[i]
            if k == 'call':
                ci, cc = get(c)
                sp = d & W & CALLU & (ci | (cc & live_out[a]))
                rl = RELU & cc & used & live_out[a]
                after = (d & ~sp & ~cc) | (cc & W & ~RELU)
                d = after | d if cond else after
            elif k == 'poll':
                sp, rl = d & W & GB(1), 0
            else:
                sp = d & W & {'tail': CALLU | RETU, 'ret': RETU}.get(k, ALLREGS)
                rl = used if k == 'sys' else 0
                if not cond:
                    d = 0
            sites[i] = (sp, rl)
        if d != dout[a]:
            dout[a] = d
            for t in fn.succ[a]:
                if din[t] | d != din[t] or dout[t] is None:
                    din[t] |= d
                    work.append(t)
    return sites


def analyze(fn, get):
    """-> (in, clob, {event: (store mask, reload mask)}) for fn under callee summaries get().
    A RELU register the callee may change is reloaded only if a later read or store needs
    it; otherwise its value stays in cpu and the stale local counts as clean (never stored).
    Stores therefore count as reads for the reload decision (iterated until stable); the
    in-set uses plain reads only (a store of a clean local writes back the entry value)."""
    order, ev, events = fn.order, fn.ev, fn.events
    live_in = liveness(fn, get, {})
    extra = {}
    while True:
        live = liveness(fn, get, extra) if extra else live_in
        sites = dirty_sites(fn, get, live)
        new = defaultdict(int)
        for a in order:
            for i in ev[a]:
                new[a] |= sites[i][0]
        new = {a: m for a, m in new.items() if m}
        if new == extra:
            break
        extra = new
    clob = fn.direct
    for i, (sp, rl) in sites.items():
        clob |= sp
        k, c, _ = events[i]
        if k in ('call', 'tail'):
            clob |= get(c)[1]
    return live_in[order[0]] & CALLU, clob, sites


def emit(fn, sites):
    """Func + store/reload masks per event -> C."""
    entry = fn.entry

    def spill(m):
        out = [f'R[{k}] = r{k};' for k in range(32) if m >> k & 1]
        out += [f'F[{k}] = fp{k};' for k in range(32) if m >> (32 + k) & 1]
        c = [i for i in range(8) if m >> (64 + i) & 1]
        if c:
            mm = sum(0xF << (28 - 4 * i) for i in c)
            out.append(f'cpu.cr = (cpu.cr & {h(~mm)}u) | ' + ' | '.join(f'cr{i} << {28 - 4 * i}' for i in c) + ';')
        if m & CTR: out.append('cpu.ctr = ctr;')
        if m & CA: out.append('cpu.xer_ca = ca;')
        if m & SO: out.append('cpu.xer_so = so;')
        return ' '.join(out + ['FNPROF(0);'])

    def reload(m):
        out = ([f'r{k} = R[{k}];' for k in fn.gu if m >> k & 1] +
               [f'fp{k} = F[{k}];' for k in fn.fu if m >> (32 + k) & 1] +
               [f'cr{i} = (cpu.cr >> {28 - 4 * i}) & 15;' for i in fn.cu if m >> (64 + i) & 1])
        out += [f'{n} = cpu.{v};' for n, v in (('ctr', 'ctr'), ('ca', 'xer_ca'), ('so', 'xer_so'))
                if n in fn.xu and m & bit(n)]
        return ' '.join(out + [f'FNPROF(0x{entry:08x}u);'])

    out = [f'void f_{entry:08x}(void) {{']
    decl = []
    if fn.gu: decl.append('u32 ' + ', '.join(f'r{k}' for k in fn.gu) + ';')
    if fn.fu: decl.append('PS ' + ', '.join(f'fp{k}' for k in fn.fu) + ';')
    if fn.cu: decl.append('u32 ' + ', '.join(f'cr{i}' for i in fn.cu) + ';')
    decl += [f'u32 {n};' for n in fn.xu]
    out.append('    ' + ' '.join(decl))
    out.append('    ' + reload(ALLREGS))
    for a in fn.order:
        if a in fn.labels:
            out.append(f'L_{a:08x}:;')
        out.extend('    ' + x for x in fn.lines[a])
        if a in fn.gotos:
            out.append(f'    goto L_{a + 4:08x};')
    out.append('}')
    src = '\n'.join(out)
    return re.sub(r'@([SL])(\d+)@', lambda m: (spill(sites[int(m.group(2))][0]) if m.group(1) == 'S'
                                                else reload(sites[int(m.group(2))][1])), src)


def main():
    dol_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DOL
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    p = Program(Dol(dol_path))
    p.stubs, p.covered = {}, set()
    p.find_jump_tables()
    p.find_starts()
    tr = sorted(a for a in p.starts if p.kind(a) == 'tr')
    for a in sorted(p.starts):  # sorted: the lower address of a duplicate name keeps the plain name
        if p.kind(a) == 'stub':
            p.sdk_name(a)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob('recomp_*.c'):
        old.unlink()
    fns = [translate_function(p, a) for a in tr]
    for fn in fns:
        if fn.order[0] != fn.entry: print(f"note: f_{fn.entry:08x} starts at {fn.order[0]:08x}")
    check_native_writes()
    sm = Summaries(p, fns)
    sm.solve()
    funcs, n = [], [0] * 5
    for fn in fns:
        sites = analyze(fn, sm.get)[2]
        funcs.append(emit(fn, sites))
        for i, (sp, rl) in sites.items():
            if fn.events[i][0] == 'call':
                n = [x + y for x, y in zip(n, (1, popc(sp), popc(rl), popc(fn.W & CALLU), popc(RELU & fn.used)))]
    print(f'{n[0]} call sites: {n[1]} register stores (fixed sets: {n[3]}), {n[2]} reloads ({n[4]})')
    hdr =['/* generated by tools/recomp/recomp.py -- do not edit */', '#pragma once', '#include "ppc.h"']
    hdr += [f'void f_{a:08x}(void);' for a in tr]
    hdr += [f'void {n}(void);' for n in sorted(set(p.stubs.values()))]
    (out / 'funcs.h').write_text('\n'.join(hdr) + '\n', encoding='utf-8')
    for k in range(0, len(funcs), FUNCS_PER_FILE):
        src = '/* generated by tools/recomp/recomp.py -- do not edit */\n#include "funcs.h"\n\n'
        (out / f'recomp_{k // FUNCS_PER_FILE:03d}.c').write_text(src + '\n\n'.join(funcs[k:k + FUNCS_PER_FILE]) + '\n', encoding='utf-8')
    # SDK stubs: weak, they only report the call; runtime/ provides the real ones
    st = ['/* generated: weak logging SDK stubs, overridden by runtime/ */', '#include "funcs.h"']
    for a, n in sorted(p.stubs.items()):
        body = f'f_{a:08x}();' if a in MTX_NATIVE else f'recomp_sdk_call("{n[4:]}");'
        st.append(f'__attribute__((weak)) void {n}(void) {{ {body} }}  /* 0x{a:08x} {p.module(a)} */')
    (out / 'sdk_stubs.c').write_text('\n'.join(dict.fromkeys(st)) + '\n', encoding='utf-8')
    # function table for bctrl/blrl/computed tail calls
    skip = sorted(a for a in p.starts if p.kind(a) == 'skip')
    ft = ['/* generated: guest address -> host function */', '#include "funcs.h"']
    ft += [f'static void skip_{a:08x}(void) {{ recomp_skipped(0x{a:08x}); }}' for a in skip]
    ent = [(a, f'f_{a:08x}') for a in tr] + list(p.stubs.items()) + [(a, f'skip_{a:08x}') for a in skip]
    ent.sort()
    ft.append('static const struct { u32 a; GuestFn f; } table[] = {')
    ft += [f'    {{0x{a:08x}u, {n}}},' for a, n in ent]
    ft.append('};')
    ft.append('''void call_indirect(u32 t) {
    int lo = 0, hi = (int)(sizeof table / sizeof table[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (table[m].a == t) { table[m].f(); return; }
        if (table[m].a < t) lo = m + 1; else hi = m - 1;
    }
    recomp_bad(t, "call_indirect: no function at this address");
}''')
    (out / 'functable.c').write_text('\n'.join(ft) + '\n', encoding='utf-8')
    for a, w in p.all_text_words():
        if w and p.kind(a) == 'tr' and a not in p.covered and decode(w):
            p.unk['valid code not reached from any function start'].append((a, ''))
    write_unknowns(p, len(tr), len(set(p.stubs.values())), len(skip))
    print(f'translated {len(tr)} functions, {len(p.stubs)} sdk stubs, {len(skip)} skipped, '
          f'{len(p.jt)} jump tables, {sum(len(v) for v in p.unk.values())} unknown sites')


def write_unknowns(p, ntr, nstub, nskip):
    head = UNKNOWNS.read_text(encoding='utf-8').split('<!-- generated -->')[0] if UNKNOWNS.exists() else '# Recompiler unknowns\n\n'
    g = ['<!-- generated -->', '## Generated by tools/recomp/recomp.py', '',
         f'Functions translated: {ntr}. SDK stubs: {nstub}. MetroTRK entries skipped: {nskip}. '
         f'Jump tables as switch: {len(p.jt)}.', '']
    for cat, items in sorted(p.unk.items()):
        uniq = sorted(set(items))
        g.append(f'### {cat} ({len(uniq)})')
        g.append('')
        g.append(', '.join(f'0x{a:08x}' + (f' {n}' if n else '') for a, n in uniq[:200]) +
                 (' ...' if len(uniq) > 200 else ''))
        g.append('')
    UNKNOWNS.write_text(head + '\n'.join(g) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
