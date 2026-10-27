#!/usr/bin/env python3
"""Static recompiler: main.dol (Gekko PPC) -> C for the 3DS runtime.

    PYTHONUTF8=1 python tools/recomp/recomp.py [main.dol] [out_dir]

Game, MSL, MWCC runtime, crt0 and MTX code is translated. SDK modules listed
in STUB_MODULES become calls to sdk_<name>() (weak empty stubs in
gen/sdk_stubs.c, overridden by runtime/). MetroTRK is skipped.
Every translated function is `void f_XXXXXXXX(void)` over the global CPU
state in runtime/ppc.h. Anything the translator cannot decide is collected
and written to docs/recomp_unknowns.md (generated section)."""
import copy, heapq, os, re, struct, sys
from collections import defaultdict
from pathlib import Path
from ppc import *

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / 'data/extracted/sys/main.dol'
OUT = ROOT / 'gen'
SDK_DOC = ROOT / 'docs/sdk_functions.md'
UNKNOWNS = ROOT / 'docs/recomp_unknowns.md'
def write_if_changed(path, text):
    # unchanged files keep their mtime, so make rebuilds only what changed
    if path.exists() and path.read_text(encoding='utf-8') == text: return
    path.write_text(text, encoding='utf-8')


FUNCS_PER_FILE = 60   # small files: make -j spreads them over the cores

# Call sites emitted as a call to an always_inline copy of the callee's C (fi_<callee>, same text as
# f_<callee>) in the caller's file, so GCC inlines it there: (bl address, callee). An explicit list from
# per-site call counts, not a size rule ("Selective inlining" in recomp_unknowns.md). Citra 2v2 game -0.2% for
# +0.6% .text; a run-time toggle in the test build (RECOMP_INLINE_SITES, main()).
INLINE_SITES = [
    (0x80181bc4, 0x80185310),   # ps_disp -> ps_disp_tev_setup, 186 calls per 2v2 frame
    (0x80181d40, 0x80184f60),   # ps_disp -> ps_disp_chan_color, 186
    (0x80181d48, 0x80184b44),   # ps_disp -> ps_disp_tev_color, 186
    (0x8018292c, 0x801829d0),   # ps_disp_particle_a -> ps_disp_quad, 84
    (0x80183f38, 0x801829d0),   # ps_disp_particle_b -> ps_disp_quad, 102
]

# RECOMP_PTR_EXTENT=dual (default): the functions that get both bodies (old rule f_, pointer extents fp_,
# chosen at entry by recomp_ptr_extent). Every one of the 142 functions whose C differs would add 1.03 MB
# (+15.7% .text), so only these: each has >= 0.05% of the FNPROF samples of the Citra 2v2 (scene 4.2, r4500+)
# or battle (r2460+) replay with the extent body (2026-10-26); together ~210 KB. The other 126 keep the old rule.
# RECOMP_PTR_EXTENT_ALL=1 gives every differing function both bodies.
PTR_EXTENT_DUAL = {
    0x801829d0, 0x801835b0, 0x8018144c,   # ps_disp_quad, ps_disp_particle_b, ps_disp (2v2 2.1 / 1.2 / 0.9%)
    0x8006eef8, 0x80090710, 0x8005e270, 0x8006207c, 0x8006b848, 0x80095e6c, 0x8006b46c, 0x800e7fc8,
    0x8006c970, 0x80061620, 0x80054238, 0x80053da8, 0x80056c6c,
}

STUB_MODULES = {'base', 'OS', 'DB', 'DVD', 'VI', 'PAD', 'AI', 'AR', 'ARQ', 'AX', 'DSP', 'CARD', 'GX',
                'EXI', 'SI', 'THP'}
SKIP_MODULES = {'MetroTRK'}
# C library functions outside STUB_MODULES replaced natively in runtime/libc.c
# (0x80023394 is game code, not in sdk_functions.md: a byte-wise zero fill with 88 callers)
NATIVE = {0x8000540c: 'memset', 0x800054f4: 'memcpy', 0x80169168: 'sqrt', 0x80023394: 'mem_zero',
          0x80169424: 'sqrtf'}   # sqrtf = bl sqrt (no frsp)
# MTX functions replaced in runtime/mtx.c: calls go to sdk_<name>, the body is still
# translated (f_) and the weak stub calls it (sdmc:/cr_mtx_off.txt and the bench use it)
MTX_NATIVE = {0x801394c4, 0x801394f0, 0x80139524, 0x801395f0, 0x80139640, 0x80139738, 0x80139a40,
              0x80139a74, 0x80139eb4, 0x80139f08, 0x8013a160, 0x8013a184, 0x8013a1a8, 0x8013a1c4,
              0x8013a208, 0x8013a220, 0x8013a264, 0x8013a284}
# MSL sin/cos and their float wrappers, replaced in runtime/libm.c the same way (body translated,
# sdmc:/cr_libm_off.txt and the native's rare paths call it); summary = the translated body's
MSL_NATIVE = {0x80168f38, 0x801689d0, 0x801693dc, 0x80169400}   # sin, cos, sinf, cosf
BODY_NATIVE = MTX_NATIVE | MSL_NATIVE
# SDK functions inside STUB_MODULES that are pure memory code: translated from the guest
# (PADClamp + ClampStick: deadzone 15, circle radius 56/44, triggers 30..180)
# Whole ranges: every instruction must be 'tr', not only the entry (else recomp_bad).
TRANSLATE_SDK_RANGES = [(0x801409b0, 0x80140c64),   # ClampStick 0x1a8 + PADClamp 0x10c
                        # THP decoders (video 0x801aeb40.., audio 0x801b60a8..); THPInit
                        # 0x801b6008 is native (runtime/thp.c: locked cache at LC_BASE)
                        (0x801aeb40, 0x801b6008), (0x801b60a8, 0x801b65d8)]   # 0x801b65d8: padding
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
        self.sda2 = self._sda2()
        self.gqr0_written = []        # translated mtspr GQR0 sites (psq_op, funcs.h)
        self.inline = {}              # leaf entry -> C emitted at each bl to it (inline_leaves)

    def _sda2(self):
        """(r2, lo, hi): r2 is set once in translated code (lis/ori in __start; skipped
        MetroTRK reuses it) and the data section around it (.sdata2, constants) is never stored to through r2, so r2-relative loads from it are
        folded to literals. None (no folding) if either does not hold."""
        sets = []
        for base, blob in self.dol.text:
            for k in range(0, len(blob), 4):
                w = struct.unpack_from('>I', blob, k)[0]
                d = decode(w)
                if d is None or self.kind(base + k) != 'tr':
                    continue
                if d.startswith(('st', 'psq_st')) and rA(w) == 2:
                    return None
                if d in ('addis', 'addi') and rD(w) == 2 or d == 'ori' and rA(w) == 2:
                    sets.append((d, w))
        if [d for d, _ in sets] != ['addis', 'ori'] or rA(sets[0][1]) != 0 or rD(sets[1][1]) != 2:
            return None
        r2 = (sets[0][1] & 0xFFFF) << 16 | (sets[1][1] & 0xFFFF)
        for base, blob in self.dol.data:
            if base <= r2 - 0x8000 < base + len(blob) or base <= r2 < base + len(blob):
                return r2, base, base + len(blob)
        return None

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
        bl = set()
        for a, w in self.all_text_words():
            d = decode(w)
            if d in ('b', 'bc') and w & 1:
                st.add(branch_target(a, w))
                bl.add(branch_target(a, w))
        ptr = {self.dol.entry} | EXTRA_STARTS | (st - bl)   # entered other than by a bl/b: self.taken
        # pointers to code in data sections (vtables, callbacks, ctors)
        jtd = getattr(self, 'jt_data', set())
        for base, blob in self.dol.data:
            for k in range(0, len(blob) - 3, 4):
                v = struct.unpack_from('>I', blob, k)[0]
                if base + k in jtd or v & 3 or self.kind(v) is None:
                    continue
                w = self.dol.word(v)
                if w and decode(w):
                    st.add(v); ptr.add(v)
        # lis/addi (or ori) pairs that build a code address in translated code
        lis = {}
        for a, w in self.all_text_words():
            d = decode(w)
            if d == 'addis' and rA(w) == 0:
                lis[rD(w)] = (a, uimm(w) << 16)
            elif d in ('addi', 'ori') and rA(w) in lis and a - lis[rA(w)][0] < 64:
                v = (lis[rA(w)][1] + (simm(w) if d == 'addi' else uimm(w))) & 0xFFFFFFFF
                if not v & 3 and self.kind(v) and decode(self.dol.word(v) or 0):
                    st.add(v); ptr.add(v)
        # prologue after an unconditional terminator (functions reached only through
        # pointers this scan cannot see, e.g. tables built at run time)
        prev = None
        for a, w in self.all_text_words():
            if prev in (0x4E800020, 0x4E800420) or (prev is not None and prev >> 26 == 18 and not prev & 1):
                if (w >> 16) == 0x9421 or w == 0x7C0802A6:  # stwu r1,-x(r1) / mflr r0
                    st.add(a)
                    if a not in bl:   # nearly every function matches; only those without a bl count
                        ptr.add(a)
            prev = w
        self.starts = {a for a in st if self.kind(a) is not None}
        # taken: starts that may be entered other than by a translated bl or b, i.e. through
        # call_indirect (translated bctrl/blrl, recomp_call_guest): data pointers, lis/addi constants,
        # prologue-scan starts that no bl reaches, EXTRA_STARTS, the DOL entry, doc names no bl
        # reaches. rtcalled: called directly from runtime/*.c (mtx.c's MTX bodies and bench) or through
        # the MTX weak stubs, except __start (main.c reads nothing after it returns). call_indirect
        # logs a call to a start not in taken (functable.c): out() (return stores) relies on knowing
        # every caller.
        rt = {int(x, 16) for f in (ROOT / 'runtime').glob('*.c')
              for x in re.findall(r'\bf_([0-9a-f]{8})\s*\(', f.read_text(encoding='utf-8'))}
        self.taken = self.starts & ptr
        self.rtcalled = self.starts & (rt | BODY_NATIVE) - {self.dol.entry}

    def sdk_name(self, a):
        if a in self.stubs:
            return self.stubs[a]
        n = self.docnames.get(a) or NATIVE.get(a)
        n = 'sdk_' + re.sub(r'\W', '_', n) if n else f'sdk_{self.module(a)}_unlisted_{a:08x}'
        if n in self.stubs.values():
            n += f'_{a:08x}'
        self.stubs[a] = n
        return n

    def callee(self, t):
        k = self.kind(t)
        if t in BODY_NATIVE:
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


def psq_op(d, D, W, I):
    """psq_l*/psq_st* body; I = 0 -> psq_ld0/psq_st0 (GQR0 = 0, ppc.h; r1-relative: stack_access)."""
    if I == 0:
        return f'F[{D}] = psq_ld0(ea, {W});' if d.startswith('psq_l') else f'psq_st0(F[{D}], ea, {W});'
    return f'F[{D}] = psq_ld(ea, {W}, {I});' if d.startswith('psq_l') else f'psq_st(F[{D}], ea, {W}, {I});'


def emit_op(p, a, w, d, sp11=False):
    """Straight-line instruction -> list of C statements. Control flow is handled by the caller.
    sp11: r11 is a stack pointer (inline_body), so its accesses are left to stack_access."""
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
        op = psq_op(d, D, W, I)
        o(f'{{ u32 ea = {ea}; {op}' + (f' R[{A}] = ea;' if d.endswith('u') else '') + ' }')
    elif d in ('psq_lx', 'psq_stx', 'psq_lux', 'psq_stux'):
        W, I = (w >> 10) & 1, (w >> 7) & 7
        op = psq_op(d, D, W, I)
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
                 'fmul': f'{fa} * {fc}', 'fmsub': f'fmadd_d({fa}, {fc}, -{fb})', 'fmadd': f'fmadd_d({fa}, {fc}, {fb})',
                 'fnmsub': f'fmadd_d({fa}, {fc}, -{fb})', 'fnmadd': f'fmadd_d({fa}, {fc}, {fb})',
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
        e = {'fmr': f'F[{B}].ps0', 'fneg': f'fneg_d(F[{B}].ps0)', 'fabs': f'fabs_d(F[{B}].ps0)',
             'fnabs': f'fnabs_d(F[{B}].ps0)'}[d]
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
            fx = {'ps_neg': 'fneg_d', 'ps_abs': 'fabs_d', 'ps_nabs': 'fnabs_d'}[d]
            two = (f'{fx}({b0})', f'{fx}({b1})')
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
        if d == 'mtspr' and n == 912:
            p.gqr0_written.append(a)
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
    if A == 2 and p.sda2 and d in ('lwz', 'lhz', 'lha', 'lbz', 'lfs', 'lfd'):
        ea = p.sda2[0] + simm(w)
        n = {'lbz': 1, 'lhz': 2, 'lha': 2}.get(d, 8 if d == 'lfd' else 4)
        if p.sda2[1] <= ea and ea + n <= p.sda2[2]:
            blob = next(b for base, b in p.dol.data if base == p.sda2[1])
            v = int.from_bytes(blob[ea - p.sda2[1]:ea - p.sda2[1] + n], 'big')
            out = [{'lwz': f'R[{D}] = 0x{v:x}u;', 'lhz': f'R[{D}] = 0x{v:x}u;', 'lbz': f'R[{D}] = 0x{v:x}u;',
                    'lha': f'R[{D}] = 0x{(v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF:x}u;',
                    'lfs': f'{{ double v = u2f(0x{v:x}u); F[{D}].ps0 = v; F[{D}].ps1 = v; }}',
                    'lfd': f'F[{D}].ps0 = u2d(0x{v:x}ull);'}[d]]
            p.sda2_folds = getattr(p, 'sda2_folds', 0) + 1
    if A == 1 and (d in LOADS or d in STORES or d[:-1] in LOADS or d[:-1] in STORES or
                   d in ('lmw', 'stmw') or d[:3] in ('lfs', 'lfd', 'stf', 'psq')):
        out = [stack_access(x) for x in out]
    elif BASE_ACCESS and A != 0 and not (sp11 and A == 11) and d in BASE_OPS:
        out = [base_access(x, A) for x in out]
    return out


SP_EA = re.compile(r'\{ u32 ea = (R\[1?1\])(?: ([+-]) (-?\d+))?; (.*) \}$')   # psq: R[1] + -24
SP_USE = re.compile(r'\b(?:(rd|wr)(8|16|32|64)|psq_(ld|st)0)\((F\[\d+\], )?ea(?: \+ (\d+))?(?=[,)])')


def stack_access(x):
    """r1/r11-relative (stack) access -> *_sp(base, k) (ppc.h: base masked once, k as the load/store
    offset); update forms, which need ea, and indexed ones -> *_ram (no IS_HW check, no WR_MARK)."""
    x = re.sub(r'\b(rd|wr)(8|16|32|64)\((R\[1?1\])(?: ([+-]) (\d+))?(?=[,)])',
               lambda m: f'{m[1]}{m[2]}_sp({m[3]}, {(m[4] or "") + (m[5] or "0")}'.replace('+', ''), x)
    m = SP_EA.match(x)
    if m and ' = ea;' not in m[4]:
        k = (-1 if m[2] == '-' else 1) * int(m[3] or 0)
        def use(u):
            off = k + int(u[5] or 0)
            name = f'{u[1]}{u[2]}_sp(' if u[1] else f'psq_{u[3]}0_sp({u[4] or ""}'
            return f'{name}{m[1]}, {off}'
        body = SP_USE.sub(use, m[4])
        if not re.search(r'\bea\b', body):
            return '{ ' + body + ' }'
    x = re.sub(r'\b(rd|wr)(8|16|32|64)\(', r'\1\2_ram(', x)
    return re.sub(r'\bpsq_(ld|st)0\(', r'psq_\g<1>0_ram(', x)


# D-form (base + constant offset) loads/stores, no update: base_access
BASE_OPS = {'lwz', 'lbz', 'lhz', 'lha', 'stw', 'stb', 'sth', 'lfs', 'lfd', 'stfs', 'stfd', 'psq_l', 'psq_st'}
BASE_ACCESS = os.environ.get('RECOMP_BASE_ACCESS', '1') != '0'
BASE_EA = re.compile(r'\{ u32 ea = R\[(\d+)\](?: ([+-]) (-?\d+))?; (.*) \}$')
BASE_USE = re.compile(r'\b(?:(rd|wr)(8|16|32|64)|psq_(ld|st)0)\((F\[\d+\], )?ea(?=[,)])')


def base_access(x, A):
    """rA + k access (rA not r1) -> *_b(rA, k) (ppc.h): the hardware check and the RAM mask work on
    the base register, k is the load/store offset, so accesses off one pointer share one check and
    one masked base. RECOMP_BASE_ACCESS=0 keeps rd32(rA + k)."""
    x = re.sub(rf'\b(rd|wr)(8|16|32|64)\(R\[{A}\](?: ([+-]) (\d+))?(?=[,)])',
               lambda m: f'{m[1]}{m[2]}_b(R[{A}], {(m[3] or "") + (m[4] or "0")}'.replace('+', ''), x)
    m = BASE_EA.match(x)
    if m and int(m[1]) == A and ' = ea;' not in m[4]:
        k = (-1 if m[2] == '-' else 1) * int(m[3] or 0)
        body = BASE_USE.sub(lambda u: (f'{u[1]}{u[2]}_b(' if u[1] else f'psq_{u[3]}0_b({u[4] or ""}') + f'R[{A}], {k}', m[4])
        if not re.search(r'\bea\b', body):
            return '{ ' + body + ' }'
    return x


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
        out += [stack_access(x) for x in emit_op(p, t, w, d, sp11=True)]
        t += 4


def lr_read_after(body, a0):
    """Can cpu.lr be read after the instruction at a0 before it is written? Reads: mflr, blrl,
    returns and tail calls (the caller or target may mflr), recomp_bad/recomp_skipped/ppc_* (log lr)."""
    seen, todo = set(), [a0 + 4]
    while todo:
        a = todo.pop()
        if a in seen or a not in body:
            continue
        seen.add(a)
        x = ' '.join(body[a])
        if re.search(r'= cpu\.lr\b|\(cpu\.lr\)|return;|recomp_bad|recomp_skipped|ppc_', x):
            return True
        if re.search(r'(?<!@LR@)cpu\.lr = ', x):
            continue
        todo += [int(t, 16) for t in re.findall(r'goto L_([0-9a-f]{8})', x)] + [a + 4]
    return False


def inline_leaves(p, fns):
    """Straight-line leaves (one blr, no other events, no cpu. access, no ctr/ca write; at most 16
    instructions, or 64 with at most 4 call sites) whose entry reads only CALLU registers: a `bl`
    to one is emitted inline (translate_function) on the caller's locals, without a call. What the
    callee read from cpu at entry is the caller's local then (stored if dirty / reloaded if live, by
    the in-set rules); its RETU writes become the caller's dirty locals, stored by the caller's own
    events where something reads them (same EABI assumption as for calls: a volatile register the
    next callee may change is dead unless read); its other writes (f0, cr0, r1 ..) go to block-local
    copies, which the caller never saw before either. cpu.lr is set only if read before the next
    write (lr_read_after). -> {entry: C lines}"""
    calls = defaultdict(int)
    for fn in fns:
        for k, c, _ in fn.events:
            if k == 'call':
                calls[c] += 1
    out = {}
    for fn in fns:
        n, last = len(fn.order) - 1, fn.order[-1]
        if (fn.entry in BODY_NATIVE or fn.order[0] != fn.entry or last - fn.entry != 4 * n or fn.labels or fn.gotos or
                [k for k, _, _ in fn.events] != ['ret'] or fn.ev[last] != [0] or
                not (n <= 16 or n <= 64 and calls[f'f_{fn.entry:08x}'] <= 4) or
                'cpu.' in fn.text or fn.W & (CTR | CA | SO)):
            continue
        live = liveness(fn, lambda c: (0, 0), {})[fn.entry]
        if live & ~CALLU:
            continue
        priv = fn.W & ~RETU
        decl = []
        for k in range(75):
            if priv >> k & 1:
                reg, ty = (f'R[{k}]', 'u32') if k < 32 else (f'F[{k - 32}]', 'PS') if k < 64 else (f'cr{k - 64}', 'u32')
                pv = f'pv_r{k}' if k < 32 else f'pv_fp{k - 32}' if k < 64 else f'pv_cr{k - 64}'
                decl.append(f'{ty} {pv}' + (f' = {reg};' if live >> k & 1 else ';'))

        def rename(x):
            for k in range(75):
                if priv >> k & 1:
                    x = (re.sub(rf'\bR\[{k}\]', f'pv_r{k}', x) if k < 32 else
                         re.sub(rf'\bF\[{k - 32}\]', f'pv_fp{k - 32}', x) if k < 64 else
                         re.sub(rf'(?<![\w.])cr{k - 64}\b', f'pv_cr{k - 64}', x))
            return x
        out[fn.entry] = ([f'{{ /* inlined f_{fn.entry:08x} */ ' + ' '.join(decl)] +
                         [rename(x) for a in fn.order[:-1] for x in fn.body[a]] + ['}'])
    return out


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
                if w & 1 and t in p.inline and t != entry:
                    L.append(f'@LR@cpu.lr = 0x{nxt:08x}u;')   # kept only if lr is read before written
                    L.extend(p.inline[t])
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
    for a, L in body.items():
        if L and L[0].startswith('@LR@'):
            L[0] = L[0][4:] if lr_read_after(body, a) else '/* inlined leaf: lr not read */'
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
#    may change (clob(G)). in/clob are a fixpoint over the call graph (Summaries).
#  - tail branch: store every dirty register (the target may be shared code of this
#    function, e.g. a common epilogue that reads r31). return: the dirty RETU ones that a
#    caller may read afterwards (out(F), solve_out).
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
# SDK natives (runtime/, sdk_stubs.c) read the CALLU registers runtime/*.c reads (native_reads():
# r1, r3-r6 for LOG(), what the code names; NATIVE_IN = all of CALLU, used before the scan) and
# write only r3, r4 and f1. Guest callbacks they run through recomp_call_guest get r1/r3/r4 set and
# are assumed to read no other register at entry (as for callbacks delivered at polls, which store
# only r1); recomp_call_guest restores all of cpu afterwards. check_native_writes() enforces the
# writes on runtime/*.c. Exceptions:
NATIVE_IN, NATIVE_CLOB = CALLU, GB(3, 4) | FB(1)
NATIVE_REGS = {'sdk_memcpy': (GB(3, 4, 5), 0), 'sdk_memset': (GB(3, 4, 5), 0),
               'sdk_sqrt': (FB(1), FB(1)), 'sdk_sqrtf': (FB(1), FB(1)), 'sdk_mem_zero': (GB(3, 4), GB(0))}
# runtime/ files allowed other register writes: boot (cpu_init), recomp_call_guest (saves
# and restores cpu), the MTX bench (saves and restores cpu) and PSMTXConcat's r6 (MTX_REGS)
NATIVE_WRITE_OK = {'cpu.c', 'mtx.c'}
# single registers a file may write besides NATIVE_CLOB (each also in that native's NATIVE_REGS):
# mem_zero leaves r0 = 0 like the guest loop
# libm.c leaves r6, f2-f4 and XER[CA] as the translated sin/cos (rem_pio2, kernels) do (MSL_NATIVE)
NATIVE_WRITE_EXTRA = {('libc.c', 'R', '0'), ('libm.c', 'R', '6'), ('libm.c', 'F', '2'), ('libm.c', 'F', '3'),
                      ('libm.c', 'F', '4'), ('libm.c', 'cpu', 'xer_ca')}
# MTX natives (runtime/mtx.c): the translated body's summary plus what the native reads/writes
MTX_REGS = (GB(3, 4, 5) | FB(1, 2, 3), GB(3, 6) | FB(1))


def check_native_writes():
    """runtime/*.c may write guest registers r3, r4 and f1 only (NATIVE_CLOB; NATIVE_WRITE_EXTRA)."""
    bad = []
    for f in sorted((ROOT / 'runtime').glob('*.c')):
        if f.name in NATIVE_WRITE_OK:
            continue
        for n, line in enumerate(f.read_text(encoding='utf-8').splitlines(), 1):
            for m in re.finditer(r'\b(R|F)\[(\w+)\](?:\.ps[01])?\s*[-+*/|&^]?=(?!=)'
                                 r'|\bcpu\.(gpr|fpr|cr|ctr|xer_\w+)\b\s*[-+|&^]?=(?!=)', line):
                if m.group(1) and ((m.group(1), m.group(2)) in (('R', '3'), ('R', '4'), ('F', '1')) or
                                   (f.name, m.group(1), m.group(2)) in NATIVE_WRITE_EXTRA):
                    continue
                if m.group(3) and (f.name, 'cpu', m.group(3)) in NATIVE_WRITE_EXTRA:
                    continue
                bad.append(f'{f.name}:{n}: {line.strip()}')
    if bad:
        sys.exit('guest register written by a native outside NATIVE_CLOB (recomp.py):\n' + '\n'.join(bad))


# computed guest register indices read in runtime/*.c -> the registers they can reach
INDEX_READS = {('os.c', 'gpr'): GB(*range(3, 11)),   # OSReport-style varargs: R[gpr], gpr <= 10
               ('os.c', 'fpr'): FB(*range(1, 9)),    # F[fpr], fpr <= 8
               ('gx.c', '1 + i'): FB(*range(1, 9))}  # F[1 + i]: viewport, light position/dir/attn


def native_reads():
    """The CALLU registers runtime/*.c reads (R[k], F[k], cpu.gpr/fpr/cr), plus r1 (recomp_call_guest's
    callback stack) and r3-r6 (LOG() prints them): the in-set of SDK natives (was all of CALLU).
    A computed index not in INDEX_READS fails the run."""
    m, bad = GB(1, 3, 4, 5, 6), []
    for f in sorted((ROOT / 'runtime').glob('*.c')):
        for n, line in enumerate(f.read_text(encoding='utf-8').splitlines(), 1):
            for x in re.finditer(r'\b(R|F|cpu\.gpr|cpu\.fpr)\[([^\]]+)\](?:\.ps[01])?(\s*([-+*/|&^]?)=(?!=))?', line):
                if x.group(3) and not x.group(4):
                    continue   # plain write
                idx, g = x.group(2).strip(), x.group(1) in ('R', 'cpu.gpr')
                if idx.isdigit():
                    m |= GB(int(idx)) if g else FB(int(idx))
                elif (f.name, idx) in INDEX_READS:
                    m |= INDEX_READS[(f.name, idx)]
                else:
                    bad.append(f'{f.name}:{n}: {line.strip()}')
            if re.search(r'\bcpu\.cr\b(?!\s*[-+|&^]?=(?!=))', line):
                m |= CB(*range(8))
    if bad:
        sys.exit('guest register read with a computed index not in INDEX_READS (recomp.py):\n' + '\n'.join(bad))
    return m & CALLU


def fold_base_regs(fns, quiet=False):
    """r2 and r13 (SDA bases) are written only by one lis/ori pair (__start; Func write masks of all
    translated code) to the value cpu_init also sets: their other reads become that constant, so
    r2/r13-relative accesses get constant addresses (GCC drops the IS_HW check and the base register)."""
    init = dict(re.findall(r'R\[(\d+)\] = (0x[0-9a-fA-F]+)u;', (ROOT / 'runtime' / 'cpu.c').read_text(encoding='utf-8')))
    for n in (2, 13):
        wr = {a: tuple(fn.lines[a]) for fn in fns for a in fn.order if fn.write[a] & GB(n)}
        hi = [int(m[1], 16) for x in wr.values() for m in [re.fullmatch(rf'r{n} = 0x([0-9a-f]+)u;', ' '.join(x))] if m]
        lo = [int(m[1], 16) for x in wr.values() for m in [re.fullmatch(rf'r{n} = r{n} \| 0x([0-9a-f]+)u;', ' '.join(x))] if m]
        if len(wr) != 2 or len(hi) != 1 or len(lo) != 1 or int(init.get(str(n), '0'), 16) != hi[0] | lo[0]:
            if not quiet:
                print(f'note: r{n} not folded (written at ' + ', '.join(f'{a:08x}' for a in sorted(wr)) + ')')
            continue
        v, k = hi[0] | lo[0], 0
        for i, fn in enumerate(fns):
            if not any(fn.use[a] & GB(n) for a in fn.order if a not in wr):
                continue
            body = {a: x if a in wr else [re.sub(rf'\bR\[{n}\]', f'0x{v:08x}u', y) for y in x] for a, x in fn.body.items()}
            k += sum(len(re.findall(rf'\bR\[{n}\]', y)) for a, x in fn.body.items() if a not in wr for y in x)
            fns[i] = Func(fn.entry, fn.order, body, fn.labels, fn.gotos)
        if not quiet:
            print(f'r{n} = 0x{v:08x}: {k} reads folded to the constant')
    return fns


def fold_so(fns, quiet=False):
    """XER[SO] is always 0: cpu_init clears it, emit_op never sets it from arithmetic (OE forms are
    listed as unknowns, none in main.dol), runtime/ never writes cpu.xer_* (check_native_writes;
    cpu.c/mtx.c only copy cpu back) and the only translated writer would be mtxer (set_xer). Without
    one, compares drop `| so`, so `so` is no longer loaded at entry or reloaded after calls."""
    if any('set_xer(' in x for fn in fns for v in fn.body.values() for x in v):
        if not quiet:
            print('note: so not folded (translated mtxer)')
        return fns
    k = 0
    for i, fn in enumerate(fns):
        if not fn.used & SO:
            continue
        body = {}
        for a, v in fn.body.items():
            body[a] = [re.sub(r' \| so;', ';', x) for x in v]
            k += sum(x.count(' | so;') for x in v)
        fns[i] = Func(fn.entry, fn.order, body, fn.labels, fn.gotos)
    if not quiet:
        print(f'so = 0: {k} compares without | so')
    return fns


NV_SAVE = [   # prologue saves of a non-volatile register (MWCC stw/stmw/stfd/psq_st, _savegpr/_savefpr inline)
    ('g', re.compile(r'wr32_sp\((?P<b>R\[1?1\]), (?P<k>-?\d+), R\[(?P<n>\d+)\]\);')),
    ('d', re.compile(r'wr64_sp\((?P<b>R\[1?1\]), (?P<k>-?\d+), d2u\(F\[(?P<n>\d+)\]\.ps0\)\);')),
    ('q', re.compile(r'psq_st0_sp\(F\[(?P<n>\d+)\], (?P<b>R\[1?1\]), (?P<k>-?\d+), 0\);'))]
NV_RESTORE = {'g': r'R\[{n}\] = rd32_sp\({b}, {k}\);', 'd': r'F\[{n}\]\.ps0 = u2d\(rd64_sp\({b}, {k}\)\);',
              'q': r'F\[{n}\] = psq_ld0_sp\({b}, {k}, 0\);'}
SP_ACCESS = re.compile(r'\b(?:rd|wr)(?P<w>8|16|32|64)_sp\((?P<b>R\[1?1\]), (?P<k>-?\d+)'
                       r'|\bpsq_(?:ld0_sp\(|st0_sp\(F\[\d+\], )(?P<qb>R\[1?1\]), (?P<qk>-?\d+)')


def drop_nv_saves(fns):
    """MWCC prologues save r14-r31/f14-f31 to the frame and epilogues load them back. Translated
    callers keep their non-volatile registers in C locals and never reload them from `cpu` after a
    call (EABI, see "Translator performance"), so a prologue saves the possibly stale `cpu` copy it
    loaded at entry, and the restore loads it into a local that is dead at the return. Both are
    dropped where the slot is provably used for nothing else: the frame is one stwu at the entry, r1
    is otherwise only written by `addi r1, r1, size` followed by no stack access before the return,
    no address is derived from r1 (indexed/update forms, mr, a pointer at or above the save area),
    the register still holds its entry value at the save, and every other access to the slot (via r1,
    or r11 = r1 + N) in the function is a restore of the same register. The guest stack keeps older
    contents in those slots (a reader of uninitialized stack would see that; the saved values were
    already the stale `cpu` copies). RECOMP_KEEP_NV_SAVES=1 keeps every save."""
    if os.environ.get('RECOMP_KEEP_NV_SAVES') == '1':
        print('note: non-volatile saves kept (RECOMP_KEEP_NV_SAVES=1)')
        return fns
    stat = defaultdict(int)
    for i, fn in enumerate(fns):
        if fn.order[0] != fn.entry:
            continue
        m = re.fullmatch(r'\{ u32 ea = R\[1\] - (\d+); wr32_ram\(ea, R\[1\]\); R\[1\] = ea; \}', ' '.join(fn.body[fn.entry]))
        if not m:
            continue
        size = int(m[1])
        # prologue: straight-line from the entry to the first label, branch, event or r1 write
        cands, written, r11, prev = {}, 0, None, fn.entry   # slot offset -> (kind, reg, bytes, statement, address)
        for a in fn.order[1:]:
            x = ' '.join(fn.body[a])
            if a != prev + 4 or a in fn.labels or re.search(
                    r'goto |return;|\bif \(|switch|recomp_|\bf_[0-9a-f]{8}\(|\bsdk_\w+\(|call_indirect|R\[1\] = ', x):
                break
            prev = a
            for kind, rx in NV_SAVE:
                for s in rx.finditer(x):
                    n, k, b = int(s['n']), int(s['k']), s['b']
                    if n < 14 or written & (GB(n) if kind == 'g' else FB(n)) or (b == 'R[11]' and r11 is None):
                        continue
                    cands[k + (r11 if b == 'R[11]' else 0)] = (kind, n, 4 if kind == 'g' else 8, s[0], a)
            mm = re.search(r'R\[11\] = R\[1\] \+ (\d+);', x)
            r11 = int(mm[1]) if mm else None if re.search(r'R\[11\] = ', x) else r11
            written |= fn.write[a]
        if not cands:
            continue
        lo = min(cands)
        bad, keep, rest, r11 = None, set(), defaultdict(list), None
        for a in fn.order[1:]:
            x = ' '.join(fn.body[a])
            if re.search(r'R\[1\] = ', x):
                if x.strip() != f'R[1] = R[1] + {size};':
                    bad = 'r1 write'
                    break
                seen, todo = set(), list(fn.succ[a])
                while todo:
                    t = todo.pop()
                    if t in seen or t not in fn.body:
                        continue
                    seen.add(t)
                    y = ' '.join(fn.body[t])
                    if SP_ACCESS.search(y) or re.search(r'R\[1\]', y):
                        bad = 'stack access after the epilogue'
                    if 'return;' not in y:
                        todo += list(fn.succ[t])
                if bad:
                    break
                continue
            if re.search(r'ea = R\[1?1\]\b|R\[\d+\] = R\[1\];|R\[1\] [-*&|^]|[-+*&|^] R\[1\]', x):
                bad = 'address derived from r1'
                break
            if any(int(mm[1]) != 11 and int(mm[2]) >= lo for mm in re.finditer(r'R\[(\d+)\] = R\[1\] \+ (\d+);', x)):
                bad = 'pointer into the save area'
                break
            mm = re.search(r'R\[11\] = R\[1\] \+ (\d+);', x)
            r11 = int(mm[1]) if mm else None if re.search(r'R\[11\] = ', x) else r11
            for s in SP_ACCESS.finditer(x):
                b, k = (s['b'], s['k']) if s['b'] else (s['qb'], s['qk'])
                if b == 'R[11]' and r11 is None:
                    bad = 'r11 access, base unknown'
                    break
                off, width = int(k) + (r11 if b == 'R[11]' else 0), SP_SIZE[s['w']] if s['w'] else 8
                for c, (kind, n, sz, st, ca) in cands.items():
                    if not (off < c + sz and c < off + width) or (ca == a and s[0] in st):
                        continue
                    r = re.search(NV_RESTORE[kind].format(n=n, b=re.escape(b), k=re.escape(k)), x)
                    if r and off == c:
                        rest[c].append((a, r[0]))
                    else:
                        keep.add(c)
            if bad:
                break
        if bad:
            stat['skipped'] += 1
            continue
        for c, (kind, n, sz, st, ca) in cands.items():   # a restored register must be dead until the exit
            nb = GB(n) if kind == 'g' else FB(n)
            same = {ra for c2, v in cands.items() if v[1] == n and (v[0] == 'g') == (kind == 'g') for ra, _ in rest[c2]}
            for ra, rs in rest[c]:
                seen, todo = set(), list(fn.succ[ra])
                while todo and c not in keep:
                    t = todo.pop()
                    if t in seen or t not in fn.body:
                        continue
                    seen.add(t)
                    if t in same:   # the other half's restore (lfd after psq_l): dropped with it
                        todo += list(fn.succ[t])
                    elif fn.use[t] & nb:
                        keep.add(c)
                    elif not fn.kill[t] & nb and 'return;' not in ' '.join(fn.body[t]):
                        todo += list(fn.succ[t])
            if c not in keep and any(len(re.findall(rf'\b(?:R|F)\[{n}\]', ' '.join(fn.body[ra]))) > 1 for ra, _ in rest[c]):
                keep.add(c)   # the restoring instruction itself reads the register again
        stat['kept'] += len(keep)
        drop = {c: v for c, v in cands.items() if c not in keep}
        if not drop:
            continue
        body = {a: list(v) for a, v in fn.body.items()}
        for c, (kind, n, sz, st, a) in drop.items():
            body[a] = [y.replace(st, '', 1) for y in body[a]]
            for ra, rs in rest[c]:
                body[ra] = [y.replace(rs, '', 1) for y in body[ra]]
            stat[kind] += 1
            stat['restores'] += len(rest[c])
        stat['functions'] += 1
        fns[i] = Func(fn.entry, fn.order, body, fn.labels, fn.gotos)
    print(f"non-volatile saves dropped in {stat['functions']} functions: {stat['g']} stw, {stat['d']} stfd, {stat['q']} "
          f"psq_st, {stat['restores']} restores; {stat['skipped']} functions with saves left as they are (frame use), "
          f"{stat['kept']} slots kept (other access or read after the restore)")
    return fns


SP_SIZE = {'8': 1, '16': 2, '32': 4, '64': 8}
# natives that read their caller's stack: GXInitTexObjCI's 9th argument at r1 + 8 (runtime/gx.c)
NATIVE_STACK_READS = {'sdk_GXInitTexObjCI': {8}}
STWU_RE = re.compile(r'\{ u32 ea = R\[1\] - (\d+); wr32_ram\(ea, R\[1\]\); R\[1\] = ea; \}')
R1_ACC = re.compile(r'\b(?:(?P<op>rd|wr)(?P<w>8|16|32|64)_sp\(|psq_(?P<q>ld|st)0_sp\((?:F\[\d+\], )?)R\[1\], (?P<k>-?\d+)')
DERIVE_RE = re.compile(r'R\[(\d+)\] = R\[(\d+)\](?: ([+-]) (\d+|R\[\d+\]))?;')


def call_args(x, i):
    """x[i] is just past '(': -> (list of top-level argument strings, index just past ')')"""
    depth, start, args = 0, i, []
    while True:
        ch = x[i]
        if ch in '([':
            depth += 1
        elif ch in ')]':
            if depth == 0:
                args.append(x[start:i].strip())
                return args, i + 1
            depth -= 1
        elif ch == ',' and depth == 0:
            args.append(x[start:i].strip())
            start = i + 1
        i += 1


NO_BOUND = -(1 << 30)
LOAD_RHS = re.compile(r'(?:\(u32\)\(s32\)\(s1[56]\))?(?:rd(?:8|16|32|64)(?:_b|_sp|_ram)?|u2f|u2d)\(')


def frame_pointers(fn, skip):
    """Forward may-analysis of the registers that hold r1 + k (r1 = the value after the frame's stwu;
    instructions in skip, the prologue up to it, are left out). -> frame offsets reached where such a
    register is used: its k, lowered by a negative constant offset used with it. A register is r1 + k
    after `R[d] = R[s] (+|-) k`, after `R[d] = R[s] + R[x]` (index taken as >= 0, as C indexes an
    array from its address up) and through an update form; anything else computed from one (masks,
    shifts) gets k - 4096; minus a register, or a loop that keeps lowering it, has no bound; loads
    are data. After a call r3/r4 may return a pointer argument (memcpy-style)."""
    first = max(skip) + 4 if skip else fn.entry
    if first not in fn.body:
        return []
    st_in, visits, uses, work = {first: {1: 0}}, defaultdict(int), [], [first]

    def value(rhs, state):
        if LOAD_RHS.match(rhs):
            return None
        d = re.fullmatch(r'R\[(\d+)\](?: ([+-]) (-?\d+|R\[\d+\]))?u?', rhs)
        if d and int(d[1]) in state:
            off = state[int(d[1])]
            if d[2] and not d[3].startswith('R['):
                return off + int(d[3]) * (1 if d[2] == '+' else -1)
            return NO_BOUND if d[2] == '-' else off
        srcs = [state[int(r)] for r in re.findall(r'R\[(\d+)\]', rhs) if int(r) in state]
        return min(srcs) - 4096 if srcs else None

    while work:
        a = work.pop()
        if a not in fn.body:
            continue
        pre = st_in[a]
        x = ' '.join(fn.body[a])
        state, ea = dict(pre), None
        for s in re.finditer(r'(?:u32 ea|R\[(\d+)\]) = ([^;]*);|\b(?:f_[0-9a-f]{8}|sdk_\w+|call_indirect)\(', x):
            if s[0].endswith('('):   # a call: volatile registers change, r3/r4 may be a pointer argument
                args = [state[r] for r in range(3, 11) if r in state]
                for r in (0, *range(3, 13)):
                    state.pop(r, None)
                if args:
                    state[3] = state[4] = min(args)
                continue
            v = ea if s[2] == 'ea' else value(s[2].strip(), state)
            if s[1] is None:
                ea = v
            elif int(s[1]) != 1:
                if v is None:
                    state.pop(int(s[1]), None)
                else:
                    state[int(s[1])] = v
        if re.search(r'\bif \(', x):   # conditional parts: either state may follow
            for r, v in pre.items():
                state[r] = min(v, state.get(r, v))
        for r in set(pre) | set(state):   # uses (stores into them included): k and negative offsets
            if r == 1:
                continue
            for u in re.finditer(rf'\bR\[{r}\](?:, (-?\d+)| ([+-]) (\d+))?', x):
                k = int(u[1]) if u[1] else int(u[3]) * (1 if u[2] == '+' else -1) if u[2] else 0
                for st in (pre, state):
                    if r in st:
                        uses.append(st[r] + min(k, 0))
        for t in fn.succ[a]:
            if t in skip or t not in fn.body:
                continue
            old = st_in.get(t)
            new = dict(state) if old is None else {r: min(old.get(r, v), v) for r, v in state.items()} | \
                {r: v for r, v in old.items() if r not in state}
            if new != old:
                visits[t] += 1
                if visits[t] > 6 and old is not None:   # keeps going down: no bound
                    new = {r: (NO_BOUND if r in old and v < old[r] else v) for r, v in new.items()}
                st_in[t] = new
                work.append(t)
    return uses


INF = 1 << 30
PF_RANGE = [int(x, 16) for x in os.environ.get('RECOMP_PTR_RANGE', '0-ffffffff').split('-')]   # RECOMP_PTR_RANGE=lo-hi: extents only for functions in [lo, hi) (bisecting)
PF_REPORT = {int(a, 16) for a in os.environ.get('RECOMP_PTR_REPORT', '').split(',') if a}   # RECOMP_PTR_REPORT=addr,..: per-frame detail


def native_ptr_ext():
    """tools/recomp/native_ptr_ext.tsv -> {native: {reg: byte ranges reached}} for the natives it lists
    without a `ret` row: value = (), ptr N = [0, N), unknown = [0, INF). A native not listed (or with a
    ret row) keeps the default rule: [0, INF) through the registers it reads (native_arg_reads)."""
    ext, ret = defaultdict(dict), set()
    for line in (ROOT / 'tools/recomp/native_ptr_ext.tsv').read_text(encoding='utf-8').splitlines():
        f = line.split('	')
        if line.startswith('#') or len(f) < 3 or f[0] == 'native':
            continue
        if f[1] == 'ret':
            ret.add(f[0])
            continue
        c = f[2].split()
        ext[f[0]][int(f[1][1:])] = () if c[0] == 'value' else ((0, int(c[1])),) if c[0] == 'ptr' else ((0, INF),)
    return {n: e for n, e in ext.items() if n not in ret}
# r[d] = r[s], r[s] +/- decimal, r[s] +/- r[x]: tracked offsets (hex constants are addis: generic)
PF_DERIV = re.compile(r'R\[(\d+)\](?: ([+-]) (-?\d+|R\[\d+\]))?u?|ea')
PF_ASSIGN = re.compile(r'(?:u32 (ea)|R\[(\d+)\]) = (.*)$')
PF_CALL = re.compile(r'\b(f_[0-9a-f]{8}|sdk_\w+|call_indirect|recomp_skipped)\(')
PF_CMP = re.compile(r'^(?:pv_)?cr\d = cmp_[su]\(')
# accesses through a register (rN + k) or through ea (an update or indexed form's address)
PF_ACC = re.compile(r'\b(?:(?:rd|wr)(8|16|32|64)_b\(R\[(\d+)\], (-?\d+)|psq_(?:ld|st)0_b\((?:[\w\[\].]+, )?R\[(\d+)\], (-?\d+), (\d)'
                    r'|(?:rd|wr)(8|16|32|64)\((ea)(?: \+ (\d+))?[,)]|psq_(?:ld|st)0?\((?:[\w\[\].]+, )?(ea), )')
PF_IDX = re.compile(r'\b(?:rd|wr)(?:8|16|32|64)\(R\[(\d+)\] \+ (R\[\d+\])')


def native_arg_reads():
    """-> {sdk_ native defined in runtime/*.c: the argument registers r3-r12 it (or a runtime function
    it calls, transitively) reads}. Printing (LOG / recomp_sdk_call / recomp_sdk_log_at) is not a
    read; gq_regs(fn, n) reads r3..r3+n-1; a computed index reads what INDEX_READS lists, else all of
    r3-r12; calling translated code (recomp_call_guest, f_) reads all. A register a native does not
    read is not reached through (ptr_flow), whatever it holds."""
    ALL = set(range(3, 13))
    defs, files = {}, {}
    for f in sorted((ROOT / 'runtime').glob('*.[ch]')):
        src = re.sub(r'/\*.*?\*/|//[^\n]*', '', f.read_text(encoding='utf-8'), flags=re.S)
        for m in re.finditer(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*\{', src, re.M):
            i, depth = m.end() - 1, 0
            for j in range(i, len(src)):
                depth += {'{': 1, '}': -1}.get(src[j], 0)
                if depth == 0:
                    break
            if m[1] not in ('if', 'for', 'while', 'switch'):
                defs.setdefault(m[1], []).append(src[i:j + 1])
                files[m[1]] = f.name
        for m in re.finditer(r'^WRAP\((\w+), (f_[0-9a-f]{8}), (.*)\)$', src, re.M):   # mtx.c natives
            defs.setdefault('sdk_' + m[1], []).append(m[0])
            files['sdk_' + m[1]] = f.name
    quiet = {'recomp_sdk_log_at', 'recomp_sdk_call', 'gq_regs'}
    own = {}
    for n, bodies in defs.items():
        r, b = set(), ' '.join(bodies)
        if n not in quiet:
            for x in re.finditer(r'\b(?:R|cpu\.gpr)\[([^\]]+)\](\s*=(?!=))?', b):
                if x[2]:
                    continue   # plain write
                idx = x[1].strip()
                if idx.isdigit():
                    r.add(int(idx))
                elif (files[n], idx) in INDEX_READS:
                    r |= {k for k in ALL if INDEX_READS[(files[n], idx)] & GB(k)}
                else:
                    r |= ALL
            for x in re.finditer(r'\bgq_regs\([^,]+,\s*(\d+)\)', b):
                r |= set(range(3, 3 + int(x[1])))
            if re.search(r'\bgq_regs\([^,]+,\s*[^\d\s]', b) or re.search(r'\brecomp_call_guest\b|\bcall_indirect\b|\bf_[0-9a-f]{8}\(', b):
                r |= ALL
        calls = {c for c in re.findall(r'\b([A-Za-z_]\w*)\s*\(', b) if c in defs and c != n and c not in quiet}
        own[n] = (r & ALL, calls)
    reads = {n: set(r) for n, (r, _) in own.items()}
    changed = True
    while changed:
        changed = False
        for n, (_, calls) in own.items():
            for c in calls:
                if not reads[c] <= reads[n]:
                    reads[n] |= reads[c]
                    changed = True
    return {n: r for n, r in reads.items() if n.startswith('sdk_')}


def pf_add(x, k):
    """offset + k, saturating at NO_BOUND / INF (else repeated composition keeps moving a bound)"""
    return x if x >= INF or x <= NO_BOUND else max(NO_BOUND, min(INF, x + k))


def pf_norm(L):
    """offset intervals [lo, hi] (inclusive) -> sorted, overlapping ones merged, at most 4 (else the hull)"""
    out = []
    for lo, hi in sorted(set(L)):
        if out and lo <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return tuple(out) if len(out) <= 4 else ((out[0][0], max(h for _, h in out)),)


def pf_join(a, b):
    """two values {origin: intervals} -> their union"""
    if a is None:
        return b
    if b is None:
        return a
    return {o: pf_norm(a.get(o, ()) + b.get(o, ())) for o in set(a) | set(b)}


PF_CACHE = {}


def pf_parse(fn, a):
    """the statements of instruction a, parsed once for ptr_flow -> list of ops (last two items: whether
    it is the stwu's `R[1] = ea`, the statement text): ('ret',), ('call', name), ('deriv', target,
    derivation), ('use', target or None, accesses [(reg or 'ea', lo, hi)], indexed bases, other
    register occurrences [(reg, k)], ea used otherwise)"""
    key = (fn.entry, a)
    if key in PF_CACHE:
        return PF_CACHE[key]
    ops = []
    for piece in ' '.join(fn.body[a]).split(';'):
        piece = re.sub(r'/\*.*?\*/|@\w+@', '', piece).strip(' {}')   # inlined leaves start with a comment
        if not piece:
            continue
        if re.search(r'(?<![=!<>])(?:\bR\[\d+\]|\bu32 ea) = ', piece) and not PF_ASSIGN.match(piece):
            sys.exit(f'recomp.py ptr_flow: assignment not at the start of a statement at {a:08x}: {piece}')
        if piece == 'return' or piece.endswith(' return') or piece.endswith('{ return'):
            ops.append(('ret', False, piece))
            continue
        mc = PF_CALL.search(piece)
        if mc:
            ops.append(('call', mc[1], False, piece))
            continue
        if PF_CMP.match(piece):   # compares are not uses
            continue
        m = PF_ASSIGN.match(piece)
        tgt = ('ea' if m[1] else int(m[2])) if m else None
        rhs = m[3].strip() if m else piece
        r1ea = piece.startswith('R[1] = ea')
        if m and not LOAD_RHS.search(rhs) and not re.search(r'\b(?:rd|wr)(?:8|16|32|64)', rhs):
            d = PF_DERIV.fullmatch(rhs)
            regs = [int(r) for r in re.findall(r'R\[(\d+)\]', rhs)]
            has_ea = bool(re.search(r'\bea\b', rhs))
            if d and d[0] == 'ea':
                dv = ('ea', None, None, None, None, regs, has_ea)
            elif d:
                x = int(d[3][2:-1]) if d[3] and d[3].startswith('R[') else None
                k = int(d[3]) * (1 if d[2] == '+' else -1) if d[3] and x is None else None
                dv = ('reg', int(d[1]), d[2], k, x, regs, has_ea)
            else:
                dv = ('other', None, None, None, None, regs, has_ea)
            ops.append(('deriv', tgt, dv, r1ea, piece))
            continue
        rest, accs, idxs = rhs, [], []
        for s in PF_ACC.finditer(rhs):
            if s[2] is not None:
                accs.append((int(s[2]), int(s[3]), int(s[3]) + SP_SIZE[s[1]]))
            elif s[4] is not None:
                accs.append((int(s[4]), int(s[5]), int(s[5]) + (4 if s[6] != '0' else 8)))
            elif s[8] is not None:
                k = int(s[9] or 0)
                accs.append(('ea', k, k + SP_SIZE[s[7]]))
            else:
                accs.append(('ea', 0, 8))
            rest = rest.replace(s[0], ' ' * len(s[0]), 1)
        for s in PF_IDX.finditer(rest):   # blank the base only: the index register stays a use
            idxs.append(int(s[1]))
            rest = rest[:s.start()] + ' ' * (s.start(2) - s.start()) + rest[s.start(2):]
        others = [(int(u[1]), int(u[2]) if u[2] else int(u[4]) * (1 if u[3] == '+' else -1) if u[3] else 0)
                  for u in re.finditer(r'\bR\[(\d+)\](?:, (-?\d+)| ([+-]) (\d+))?', rest)]
        ops.append(('use', tgt, accs, idxs, others, bool(re.search(r'\bea\b', rest)), r1ea, piece))
    PF_CACHE[key] = ops
    return ops


def ptr_flow(fn, stwu, summ, why=None):
    """Forward may-analysis of the pointers a function derives from its own frame (origin 0: r1 after
    the stwu at `stwu`, or at the entry when frameless) and from its argument registers (origins 3-10:
    the entry values of r3-r12 (r11/r12: MWCC helpers)). A register's value: per origin, offset intervals it may equal.
    -> (reach, ret, frame_ret): reach[origin] = byte ranges [a, b) (b may be INF) read or written
    through it, here or in a callee; ret = {3: .., 4: ..}: argument-derived values left in r3/r4 at a
    return (None: unknown); frame_ret: a frame pointer is left in r3/r4 at a return.
    A use: an access rN + k / through ea (its bytes); an indexed access, a store of the pointer, a
    call_indirect, or any other use: [lo + min(k, 0), INF) (the old rule: code reaches an object only
    from its address upward); a call: the callee's reach per argument register (summ). Derivations:
    rN +/- decimal and update forms keep exact offsets; + rX (index) has no upper bound; other
    arithmetic gets lo - 4096 and no upper bound; minus a register no bound. Compares are not uses.
    After a call registers keep their values (a callee may leave them) and r3/r4 may also hold the
    callee's ret. A loop that keeps moving a value widens it (no bound in that direction)."""
    reach, ret, fret = defaultdict(list), {3: set(), 4: set()}, [False]
    seeds = {r: {r: ((0, 0),)} for r in range(3, 13)}
    if stwu is None:
        seeds[1] = {0: ((0, 0),)}
    pos = {x: n for n, x in enumerate(fn.order)}   # worklist in program order (heap)
    st_in, visits, work, queued = {fn.entry: seeds}, defaultdict(int), [(pos.get(fn.entry, 0), fn.entry)], {fn.entry}
    if fn.entry not in fn.body:
        return reach, ret, False

    cur = [0, '']

    def use(v, a, b):
        for o, L in v.items():
            for lo, hi in L:
                reach[o].append((NO_BOUND if lo <= NO_BOUND else pf_add(lo, a), INF if b >= INF else pf_add(hi, b)))
                if why is not None and reach[o][-1][1] >= INF:   # unbounded uses (debugging)
                    why.append((o, cur[0], reach[o][-1][0], cur[1]))

    def escape(v, k=0):
        use(v, min(k, 0), INF)

    def call(name, state):
        key = int(name[2:], 16) if name.startswith('f_') else name
        s = summ.get(key) if name not in ('call_indirect', 'recomp_skipped') else None
        args = {r: state[r] for r in range(3, 13) if r in state}
        for r, v in args.items():
            if s is None:
                escape(v)
            else:
                for a, b in s[0].get(r, ()):
                    use(v, a, b)
        for rr in (3, 4):
            if s is None or s[1] is None:
                nv = None
                for v in args.values():
                    nv = pf_join(nv, {o: ((min(lo for lo, _ in L), INF),) for o, L in v.items()})
            else:
                nv = None
                for r, lo2, hi2 in s[1][rr]:
                    if r in args:
                        nv = pf_join(nv, {o: tuple((pf_add(lo, lo2), INF if hi2 >= INF else pf_add(hi, hi2)) for lo, hi in L)
                                          for o, L in args[r].items()})
            if nv is not None:
                state[rr] = pf_join(state.get(rr), nv)

    def value(d, state):   # d: pf_parse's derivation
        kind, s, sign, k, x, regs, has_ea = d
        if kind == 'ea':
            return state.get('ea')
        if kind == 'reg' and s in state:
            v = state[s]
            if sign is None:
                return v
            if x is None:
                return {o: tuple((pf_add(lo, k), pf_add(hi, k)) for lo, hi in L) for o, L in v.items()}
            x = state.get(x, {})   # either operand may be the pointer
            if sign == '-':
                return {o: ((NO_BOUND, INF),) for o in set(v) | set(x)}
            return pf_join({o: ((L[0][0], INF),) for o, L in v.items()},
                           {o: ((pf_add(L[0][0], -4096), INF),) for o, L in x.items()})
        srcs = [state[r] for r in regs if r in state]
        if has_ea and 'ea' in state:
            srcs.append(state['ea'])
        out = None
        for v in srcs:
            out = pf_join(out, {o: ((pf_add(L[0][0], -4096), INF),) for o, L in v.items()})
        return out

    while work:
        a = heapq.heappop(work)[1]
        queued.discard(a)
        if a not in fn.body:
            continue
        state = dict(st_in[a])
        state.pop('ea', None)
        for op in pf_parse(fn, a):
            cur[:] = a, op[-1]
            if op[0] == 'ret':
                for rr in (3, 4):
                    for o, L in state.get(rr, {}).items():
                        if o == 0:   # own frame: dead after the return (C); frameless: the caller's, an escape
                            fret[0] = True
                            if stwu is None:
                                escape({0: L})
                        else:
                            ret[rr] |= {(o, lo, hi) for lo, hi in L}
                continue
            if op[0] == 'call':
                call(op[1], state)
                continue
            tgt = op[1]
            if op[0] == 'deriv':   # a derivation (or a constant): no use
                v = value(op[2], state)
            else:
                for r, lo, hi in op[2]:   # accesses through rN + k / ea: their bytes
                    if r in state:
                        use(state[r], lo, hi)
                for r in op[3]:   # indexed accesses: from the base upward
                    if r in state:
                        escape(state[r])
                for r, k in op[4]:   # anything else: the old rule
                    if r != 1 and r in state:
                        escape(state[r], k)
                if op[5] and 'ea' in state:
                    escape(state['ea'])
                v = None   # loads are data
            if tgt is not None and tgt != 1:
                if v is None:
                    state.pop(tgt, None)
                else:
                    state[tgt] = v
            if op[-2] and a == stwu:
                state[1] = {0: ((0, 0),)}
        state.pop('ea', None)
        for t in fn.succ[a]:
            if t not in fn.body:
                continue
            old = st_in.get(t)
            new = dict(state) if old is None else {r: pf_join(old.get(r), state.get(r)) for r in set(old) | set(state)}
            if new != old:
                visits[t] += 1
                if visits[t] > 6 and old is not None:   # keeps moving: no bound in that direction
                    for r, v in new.items():
                        if r in old and v != old[r]:
                            wv = {}
                            for o, L in v.items():
                                lo, hi = L[0][0], max(h for _, h in L)
                                ol = old[r].get(o)
                                if ol:
                                    lo = NO_BOUND if lo < ol[0][0] else lo
                                    hi = INF if hi > max(h for _, h in ol) else hi
                                wv[o] = ((lo, hi),)
                            new[r] = wv
                st_in[t] = new
                if t not in queued:
                    queued.add(t)
                    heapq.heappush(work, (pos.get(t, 0), t))
    return reach, ret, fret[0]


def pf_ranges(L):
    """byte ranges [a, b) -> merged tuple"""
    out = []
    for a, b in sorted(set(L)):
        if out and a <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return tuple(out) if len(out) <= 8 else ((out[0][0], max(b for _, b in out)),)


def pf_rv(v):
    """returned values {(origin, lo, hi)} -> one interval per origin (their hull)"""
    h = {}
    for o, lo, hi in v:
        h[o] = (min(lo, h[o][0]), max(hi, h[o][1])) if o in h else (lo, hi)
    return frozenset((o, lo, hi) for o, (lo, hi) in h.items())


def ptr_summaries(fns, stwus, stubs):
    """Fixpoint over the call graph: per translated function, the bytes reached through each argument
    register r3-r12 and the argument-derived values it returns in r3/r4 (ptr_flow). Natives: the
    NATIVE_PTR_EXT table, MTX/MSL bodies: also their translated body's. Recursion that keeps growing a
    summary is widened (no upper bound / unknown return). -> (summaries, per function ptr_flow result)"""
    by = {fn.entry: fn for fn in fns}
    summ = {e: ({}, {3: frozenset(), 4: frozenset()}) for e in by}
    body_of = {n: a for a, n in stubs.items() if a in BODY_NATIVE}
    # natives: unlisted ones reach [0, INF) through the registers they read and may return those
    for n, rd in native_arg_reads().items():
        if n not in body_of:   # (the table below overrides)
            summ[n] = ({r: ((0, INF),) for r in rd}, {rr: frozenset((r, 0, INF) for r in rd) for rr in (3, 4)})
    table, reads = native_ptr_ext(), native_arg_reads()
    for n in set(table) | set(body_of):   # a register the native reads but the table omits: [0, INF)
        e = {r: ((0, INF),) for r in reads.get(n, ()) if n not in body_of} | table.get(n, {})
        summ[n] = ({r: pf_ranges(v) for r, v in e.items() if v}, {3: frozenset(), 4: frozenset()})
    callees = {}
    for fn in fns:
        cs = set()
        for c in set(re.findall(r'\b(f_[0-9a-f]{8}|sdk_\w+)\(', ' '.join(x for v in fn.body.values() for x in v))):
            c = int(c[2:], 16) if c.startswith('f_') else body_of.get(c)
            if c in by:
                cs.add(c)
        callees[fn.entry] = cs
    # strongly connected components, callees first (Tarjan, iterative)
    index, low, onst, stack, sccs, n = {}, {}, set(), [], [], [0]
    for root in sorted(by):
        if root in index:
            continue
        work = [(root, iter(sorted(callees[root])))]
        index[root] = low[root] = n[0]; n[0] += 1; stack.append(root); onst.add(root)
        while work:
            v, it = work[-1]
            w = next(it, None)
            if w is None:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        x = stack.pop(); onst.discard(x); comp.append(x)
                        if x == v:
                            break
                    sccs.append(comp)
            elif w not in index:
                index[w] = low[w] = n[0]; n[0] += 1; stack.append(w); onst.add(w)
                work.append((w, iter(sorted(callees[w]))))
            elif w in onst:
                low[v] = min(low[v], index[w])
    res, rounds = {}, defaultdict(int)
    big = sorted((len(c) for c in sccs), reverse=True)[:5]
    print(f"ptr summaries: {len(sccs)} call-graph components, largest {big}")
    for comp in sccs:
        work = set(comp)
        while work:
            e = work.pop()
            reach, ret, fret = ptr_flow(by[e], stwus.get(e), summ)
            res[e] = (reach, fret)
            ext = {r: pf_ranges(reach[r]) for r in range(3, 13) if reach.get(r)}
            rv = {rr: pf_rv(ret[rr]) for rr in (3, 4)}
            old = summ[e]   # summaries only grow (else a widened one can shrink back and oscillate)
            ext = {r: pf_ranges(list(ext.get(r, ())) + list(old[0].get(r, ()))) for r in set(ext) | set(old[0])}
            rv = {rr: pf_rv(rv[rr] | old[1][rr]) for rr in (3, 4)}
            if (ext, rv) == old:
                continue
            rounds[e] += 1
            if rounds[e] > 8:   # recursion that keeps growing: no bound in the direction it moves
                def wid(a, b, oa, ob):
                    return (NO_BOUND if oa is not None and a < oa else a, INF if ob is not None and b > ob else b)
                ext = {r: (wid(min(a for a, _ in Lr), max(b for _, b in Lr),
                               min((a for a, _ in old[0].get(r, ())), default=None), max((b for _, b in old[0].get(r, ())), default=None)),)
                       for r, Lr in ext.items()}
                ov = {rr: {o: (lo, hi) for o, lo, hi in old[1][rr]} for rr in (3, 4)}
                rv = {rr: frozenset((o, *wid(lo, hi, *ov[rr].get(o, (None, None)))) for o, lo, hi in v) for rr, v in rv.items()}
                if (ext, rv) == old:
                    continue
            summ[e] = (ext, rv)
            work |= {c for c in comp if e in callees[c]}
            for nm, ba in body_of.items():   # an MTX/MSL native: also its translated body's reach
                if ba == e:
                    t = table.get(nm, {})
                    summ[nm] = ({r: pf_ranges(list(ext.get(r, ())) + list(pf_ranges(t.get(r, ()))))
                                 for r in set(ext) | set(t)}, rv)
    return summ, res


def frame_prologue(fn):
    """the stwu: in the straight-line prologue (MWCC: mflr r0; stw r0, 4(r1); stwu r1, -N(r1))
    -> (STWU_RE match or None, prologue addresses up to and including it)"""
    m, pro, prev = None, set(), fn.entry - 4
    for a in fn.order[fn.order.index(fn.entry):] if fn.entry in fn.order else []:
        x = ' '.join(fn.body[a])
        if a != prev + 4 or (a in fn.labels and a != fn.entry):
            break
        m = STWU_RE.fullmatch(x)
        if m:
            pro.add(a)
            break
        if re.search(r'goto |return;|\bif \(|switch|recomp_|\bf_[0-9a-f]{8}\(|\bsdk_\w+\(|call_indirect|R\[1\] = ', x):
            break
        pro.add(a)
        prev = a
    return (m, pro) if m else (None, set())


def promote_stack_slots(fns, psq_ok=True, stubs=None, ptr_extent=False):
    """Guest stack words a function alone uses become C locals (sp_s<k>, the word's bits in host
    order): no address mask, byte swap or RAM round trip for MWCC's spills (stfs/lfs, stfd/lfd, the
    stw/stw/lfd int->double idiom, psq). A word r1 + k of a frame (one stwu at the entry, r1 written
    otherwise only by the epilogue `addi r1, r1, size`, no stack access after it) qualifies when:
    8 <= k (0 = back chain, 4 = callee LR save) and k + 4 <= the frame size; it lies below every
    address derived from r1 in the function, counting pointer arithmetic on those registers and
    negative constant offsets used with them (assumption: code reaches a local array or struct only
    from its address upward, as C does); no callee reaches into its caller's frame at that offset
    (callers' parameter areas: stack accesses above a callee's own frame, or pointers derived there,
    collected over all functions); and every access to it is a 32-bit, 64-bit or psq access that
    only touches promoted words. The local starts as the word's memory value after the stwu (a read
    before any write sees what it saw before; GCC drops the load where it is dead). The frame's
    memory keeps older contents in those words: only a reader of another function's dead stack, or
    of this frame through a pointer the rules above missed, sees a difference.
    psq_ok: psq accesses are GQR0 = 0 floats (no translated mtspr GQR0, as psq_ld0/psq_st0 assume).
    RECOMP_STACK_SLOTS=0 keeps every stack access in memory. ptr_extent (main(): RECOMP_PTR_EXTENT)
    replaces "below every address derived from r1" with the byte ranges reached through frame pointers
    (ptr_summaries: this function's accesses, its callees' per-argument extents, native_ptr_ext.tsv)."""
    if os.environ.get('RECOMP_STACK_SLOTS', '1') == '0':
        print('note: stack slots kept in memory (RECOMP_STACK_SLOTS=0)')
        return fns
    reads = [f'{f.name}:{n}' for f in sorted((ROOT / 'runtime').glob('*.c')) if f.name not in NATIVE_WRITE_OK
             for n, line in enumerate(f.read_text(encoding='utf-8').splitlines(), 1) if re.search(r'R\[1\] [-+]', line)]
    if len(reads) != sum(len(v) for v in NATIVE_STACK_READS.values()):
        sys.exit('runtime/ reads the guest stack through r1 (recomp.py NATIVE_STACK_READS lists each):\n' + '\n'.join(reads))
    stat = defaultdict(int)
    info = {}
    prologues = {fn.entry: frame_prologue(fn) for fn in fns}
    pf = None
    if ptr_extent:
        pf = ptr_summaries(fns, {e: max(p) if m else None for e, (m, p) in prologues.items()}, stubs or {})[1]
    else:
        print('note: frame pointers reach from their address upward (old rule)')
    # per function: words of the frame that transferred control to it (bl or tail b, same r1) it may
    # touch, and the lowest such offset a pointer it derives may reach
    g_ext, g_from, calls, framed, ind = {}, {}, {}, set(), set()
    for fn in fns:
        ge, gf = set(), 1 << 30
        m, pro = prologues[fn.entry]
        size = int(m[1]) if m else 0
        top = size if m else 0   # offsets >= top are the caller's frame
        lo, bad, acc, rng = 1 << 30, None, defaultdict(list), None
        if pf is None or not PF_RANGE[0] <= fn.entry < PF_RANGE[1]:
            uses = frame_pointers(fn, pro)
            lo = min(uses + [lo])
            gf = min([u - top for u in uses if u >= top + (0 if m else 8)] + [gf])
        else:   # byte ranges reached through frame pointers; an unbounded one from inside the frame stays there
            rng = pf_ranges(pf[fn.entry][0].get(0, ()))
            thr = top + (0 if m else 8)   # frameless: every range into the caller's frame counts
            gf = min([max(a, thr) - top for a, b in rng if a >= thr or thr < b < INF or not m and b > thr] + [gf])
            stat['frame_ret'] += pf[fn.entry][1]
        for a in fn.order:
            x = ' '.join(fn.body[a])
            if a in pro:   # before the stwu: r1 is the caller's (LR save at 4); the stwu itself
                if not STWU_RE.fullmatch(x):
                    for s in R1_ACC.finditer(x):
                        if int(s['k']) != 4:
                            ge.add(int(s['k']) & ~3)
                    if re.search(r'R\[\d+\] = R\[1\]', x):
                        bad = 'pointer before the stwu'
                continue
            if re.search(r'R\[1\] = ', x) and x.strip() != f'R[1] = R[1] + {size};':
                bad = 'r1 write'
            elif m and re.search(r'R\[1\] = ', x):   # the epilogue: no stack access until the return
                seen, todo = set(), list(fn.succ[a])
                while todo:
                    t = todo.pop()
                    if t in seen or t not in fn.body:
                        continue
                    seen.add(t)
                    y = ' '.join(fn.body[t])
                    if 'R[1]' in y or R1_ACC.search(y):
                        bad = 'stack access after the epilogue'
                    if 'return;' not in y:
                        todo += list(fn.succ[t])
            for s in R1_ACC.finditer(x):
                k = int(s['k'])
                w = SP_SIZE[s['w']] if s['w'] else 8 if s['q'] else 0
                acc[k].append((s['op'] or s['q'], w))
            # any other read of r1: a pointer the access patterns above do not see
            rest = R1_ACC.sub('', x)
            rest = re.sub(r'R\[\d+\] = R\[1\](?: [+-] \d+)?;', '', rest)
            if m:
                rest = rest.replace(f'R[1] = R[1] + {size};', '')
            if 'R[1]' in rest:
                bad = bad or 'other r1 use'
        for k, L in acc.items():
            if k >= top and k != top + 4:
                for op, w in L:
                    ge |= {c & ~3 for c in range(k - top, k - top + max(w, 1))}
        text = ' '.join(x for a in fn.order for x in fn.body[a])
        calls[fn.entry] = set(re.findall(r'\b(f_[0-9a-f]{8}|sdk_\w+)\(', text))
        if 'call_indirect' in text:
            ind.add(fn.entry)
        if m:
            framed.add(fn.entry)
        g_ext[fn.entry], g_from[fn.entry] = ge, gf
        if bad or not m:
            stat['skipped' if m else 'frameless'] += 1
            continue
        info[fn.entry] = (size, lo, acc, pro, rng)
    for e, n in NATIVE_STACK_READS.items():
        g_ext[e], g_from[e], calls[e] = set(n), 1 << 30, set()
    changed = True
    while changed:   # a frameless function leaves r1 alone: what its callees touch is its transferrer's too
        changed = False
        for e in g_ext:
            if e in framed or isinstance(e, str):
                continue
            for c in calls[e]:
                c = int(c[2:], 16) if c.startswith('f_') else c
                if c in g_ext and not (g_ext[c] <= g_ext[e] and g_from[c] >= g_from[e]):
                    g_ext[e] |= g_ext[c]
                    g_from[e] = min(g_from[e], g_from[c])
                    changed = True
    # call_indirect: any frameless function (conservative; frames move r1, their own words are theirs)
    ind_ext = set().union(*(g_ext[e] for e in g_ext if e not in framed and not isinstance(e, str)))
    ind_from = min([g_from[e] for e in g_ext if e not in framed and not isinstance(e, str)] + [1 << 30])
    for i, fn in enumerate(fns):
        if fn.entry not in info:
            continue
        size, lo, acc, pro, rng = info[fn.entry]
        stwu = max(pro)
        ext, ext_from = (set(ind_ext), ind_from) if fn.entry in ind else (set(), 1 << 30)
        for c in calls[fn.entry]:
            c = int(c[2:], 16) if c.startswith('f_') else c
            if c in g_ext:
                ext |= g_ext[c]
                ext_from = min(ext_from, g_from[c])
        stat['ext'] += bool(ext) or ext_from < size
        limit = min(size, lo, ext_from)
        words = set()
        for k, L in acc.items():
            for op, w in L:
                for kk in range(k, k + max(w, 4), 4):
                    words.add(kk)
        def qualify(limit, rng):
            ok = {k for k in words if k % 4 == 0 and 8 <= k and k + 4 <= limit and k not in ext
                  and not any(a < k + 4 and b > k for a, b in rng)}
            if not psq_ok:
                ok -= {kk for k, L in acc.items() for op, w in L if op in ('ld', 'st') for kk in (k, k + 4)}
            changed = True
            while changed:   # an access counts only if it covers whole promoted words
                changed = False
                for k, L in acc.items():
                    for op, w in L:
                        cover = {c & ~3 for c in range(k, k + max(w, 1))}
                        if (w < 4 or k % 4 or not cover <= ok) and cover & ok:
                            ok -= cover
                            changed = True
            return ok
        ok = qualify(limit, rng or ())
        free = qualify(min(size, ext_from), ())   # the words frame pointers alone keep in memory: free - ok
        stat['ptr_blocked'] += len(free - ok)
        if fn.entry in PF_REPORT:
            print(f'  f_{fn.entry:08x}: frame {size}, {len(ok)} words promoted, {len(free - ok)} kept for frame pointers '
                  f'{sorted(free - ok)}; frame pointer ranges {list(rng) if rng is not None else f"from {lo}"}')
        if not ok:
            continue
        body = {a: list(v) for a, v in fn.body.items()}

        def rewrite(x):
            res, j = [], 0
            for s in R1_ACC.finditer(x):
                if s.start() < j:
                    continue
                k = int(s['k'])
                st = s.start()
                op, w = s['op'] or s['q'], SP_SIZE[s['w']] if s['w'] else 8
                is_q = bool(s['q'])
                head = x.index('(', st) + 1
                args, end = call_args(x, head)
                W = int(args[-1]) if is_q else 0
                cover = [k] if (is_q and W) or w == 4 else [k, k + 4]
                if k % 4 or w < 4:
                    continue
                if not all(c in ok for c in cover):
                    continue
                if op in ('rd', 'ld'):
                    if is_q:
                        rep = f'psq_ld0_v(sp_s{k}, {f"sp_s{k + 4}" if not W else "0u"}, {W})'
                    elif w == 4:
                        rep = f'sp_s{k}'
                    else:
                        rep = f'((u64)sp_s{k} << 32 | sp_s{k + 4})'
                    res.append((st, end, rep))
                else:
                    if not x[end:].startswith(';'):
                        continue
                    if is_q:
                        rep = f'{{ PS sv_ = {args[0]}; sp_s{k} = f2u((float)sv_.ps0);' + ('' if W else f' sp_s{k + 4} = f2u((float)sv_.ps1);') + ' }'
                    elif w == 4:
                        rep = f'sp_s{k} = {args[2]};'
                    else:
                        rep = f'{{ u64 sv_ = {args[2]}; sp_s{k} = (u32)(sv_ >> 32); sp_s{k + 4} = (u32)sv_; }}'
                    res.append((st, end + 1, rep))
                j = end
                stat['accesses'] += 1
            for st, end, rep in reversed(res):
                x = x[:st] + rep + x[end:]
            return x
        for a in fn.order:
            if a not in pro:
                body[a] = [rewrite(x) for x in body[a]]
        body[stwu] = body[stwu] + [f'u32 sp_s{k} = rd32_sp(R[1], {k});' for k in sorted(ok)]
        stat['functions'] += 1
        stat['words'] += len(ok)
        fns[i] = Func(fn.entry, fn.order, body, fn.labels, fn.gotos)
    print(f"stack slots in locals: {stat['words']} words in {stat['functions']} functions, {stat['accesses']} accesses; "
          f"{stat['skipped']} frames left in memory (other r1 use), {stat['ext']} frames with words their callees reach, "
          f"{stat['ptr_blocked']} words kept for frame pointers, {stat['frame_ret']} functions return a frame pointer in r3/r4")
    return fns


def check_gqr_writes():
    """psq_ld0/psq_st0 (ppc.h) assume GQR0 = 0: runtime/*.c may only write 0 to cpu.gqr[0], and a
    computed index only where listed (GQR_INDEX_OK: thp.c sets GQR2-5)."""
    bad = []
    for f in sorted((ROOT / 'runtime').glob('*.c')):
        for n, line in enumerate(f.read_text(encoding='utf-8').splitlines(), 1):
            for m in re.finditer(r'\bgqr\[([^\]]*)\]\s*[-+*/|&^]?=(?!=)\s*([^;]*)', line):
                i, v = m.group(1).strip(), m.group(2).strip()
                if i == '0' and v != '0' or not i.isdigit() and (f.name, i) not in GQR_INDEX_OK:
                    bad.append(f'{f.name}:{n}: {line.strip()}')
    if bad:
        sys.exit('GQR0 may be written non-zero by runtime/ (psq_ld0/psq_st0 assume 0, recomp.py):\n' + '\n'.join(bad))


GQR_INDEX_OK = {('thp.c', 'i')}


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
    """C of one instruction (locals) -> (uses, kills, writes), statement by statement: a
    statement's reads count as before its own writes, but not after an earlier statement
    of the instruction wrote the whole register (record forms: `r0 = ..; cr0 = cmp_s(r0..)`
    does not read the old r0). A write to one half of a paired single also reads the register
    unless the other half is written too. Instruction C is straight-line apart from branch
    lines, which write no register."""
    kill = write = use = 0
    halves = defaultdict(set)

    def d(m):
        nonlocal kill, write
        b = bit(m.group(1))
        write |= b
        if m.group(2):
            halves[b].add(m.group(2))
            if len(halves[b]) == 2:
                kill |= b
        else:
            kill |= b
        return '@'
    for s in text.split(';'):
        before = kill
        t = DEF_RE.sub(d, s)
        if '&ca' in t:
            t = t.replace('&ca', '@')
            kill |= CA; write |= CA
        if re.search(r'(?<![\w.])ctr--', t):
            kill |= CTR; write |= CTR
        for m in NAME_RE.finditer(t):
            use |= bit(m.group(1)) & ~before
    for b, hs in halves.items():
        if len(hs) == 1 and not kill & b:
            use |= b
    return use, kill, write


class Func:
    """One translated function: per-instruction C (guest registers as locals, events as
    @S<n>@ / @L<n>@ markers) plus what the register analysis needs."""

    def __init__(self, entry, order, body, labels, gotos):
        self.entry, self.order, self.labels, self.gotos = entry, order, labels, gotos
        self.body = body
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
    whose `cpu` value F may change. Least fixpoint over the call graph. out(F): RETU registers
    F stores at its returns (solve_out)."""

    def __init__(self, p, funcs):
        self.p, self.funcs = p, {f.entry: f for f in funcs}
        self.s = {e: (0, 0) for e in self.funcs}
        self.out = {e: RETU for e in self.funcs}
        self.native_in, self.ind_in = NATIVE_IN, 0
        self.name_addr = {n: a for a, n in p.stubs.items()}
        self.callers = defaultdict(set)
        for f in self.funcs.values():
            for c in f.callees():
                d = 'call_indirect' if c == 'call_indirect' else self.dep(c)
                if d is not None:
                    self.callers[d].add(f.entry)

    def get(self, callee):
        if callee.startswith('f_'):
            return self.s.get(int(callee[2:], 16), (CALLU, RELU))
        a = self.name_addr.get(callee)
        if a in MTX_NATIVE and a in self.s:
            i, c = self.s[a]
            return i | MTX_REGS[0], c | MTX_REGS[1]
        if a in MSL_NATIVE and a in self.s:   # libm.c reads f1, writes a subset of the body's clob
            return self.s[a]
        if callee == 'call_indirect':   # its targets: p.taken (functable.c logs any other) and natives
            return self.native_in | self.ind_in, RELU
        return NATIVE_REGS.get(callee, (self.native_in, NATIVE_CLOB))

    def dep(self, callee):
        """translated function whose summary get(callee) reads"""
        if callee.startswith('f_'):
            return int(callee[2:], 16)
        a = self.name_addr.get(callee)
        return a if a in BODY_NATIVE else None

    def solve(self, work=None):
        """work: functions to (re)analyze, default all; their callers follow as summaries change"""
        work = sorted(self.funcs if work is None else work)
        inq = set(work)
        while work:
            e = work.pop()
            inq.discard(e)
            new = analyze(self.funcs[e], self.get, self.out[e])[:2]
            if new != self.s[e]:
                self.s[e] = new
                up = self.callers[e]
                if e in self.p.taken and new[0] & ~self.ind_in:
                    self.ind_in |= new[0]
                    up = up | self.callers['call_indirect']
                for c in up:
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


def dirty_sites(fn, get, live, out):
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
                # ctr, ca, cr0, cr5-7 too (until 2026-10-22 they were reloaded after every call;
                # the campaign DL rebuild blamed on that was the build-dependent DL cache conflict)
                rl = RELU & cc & used & live_out[a]
                after = (d & ~sp & ~cc) | (cc & W & ~RELU)
                d = after | d if cond else after
            elif k == 'poll':
                sp, rl = d & W & GB(1), 0
            else:
                sp = d & W & (RETU & out if k == 'ret' else ALLREGS)
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


def analyze(fn, get, out=RETU):
    """-> (in, clob, {event: (store mask, reload mask)}) for fn under callee summaries get(),
    storing out (Summaries.out) of the dirty RETU registers at its returns.
    A RELU register the callee may change is reloaded only if a later read or store needs
    it; otherwise its value stays in cpu and the stale local counts as clean (never stored).
    Stores therefore count as reads for the reload decision (iterated until stable); the
    in-set uses plain reads only (a store of a clean local writes back the entry value)."""
    order, ev, events = fn.order, fn.ev, fn.events
    live_in = liveness(fn, get, {})
    extra = {}
    while True:
        live = liveness(fn, get, extra) if extra else live_in
        sites = dirty_sites(fn, get, live, out)
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
    fn.entry_live, fn.live = live[order[0]], live
    return live_in[order[0]] & CALLU, clob, sites


def reach_ret(fn):
    """per instruction: registers that some path from its start keeps unwritten (no full write)
    up to a return, tail or sysret event, where the value then in cpu leaves the function"""
    rr = {a: 0 for a in fn.order}
    changed = True
    while changed:
        changed = False
        for a in reversed(fn.order):
            if any(fn.events[i][0] in ('ret', 'tail', 'sysret') for i in fn.ev[a]):
                v = ALLREGS
            else:
                v = 0
                for t in fn.succ[a]:
                    v |= rr[t]
                v &= ~fn.kill[a]
            if v != rr[a]:
                rr[a], changed = v, True
    return rr


def solve_out(p, fns, sm):
    """out(F): the RETU registers whose cpu value right after F returns may be read before it is
    overwritten; F's returns store only those (were all of RETU). Least fixpoint, grown from
    out = RETU for p.rtcalled (runtime/ reads the results) and 0 else: a direct call s in C adds
    (live after s | what reaches C's return unwritten & out(C)) & RETU, where live counts C's reads,
    later callees' in-sets (SDK in-sets keep the logged r3-r6) and later stores; a tail call adds
    out(C); every call_indirect site adds the same to all of p.taken (recomp_call_guest restores
    cpu after its call: nothing read). Polls: callbacks read only r1/r3/r4 (as before)."""
    init = {fn.entry: RETU if fn.entry in p.rtcalled else 0 for fn in fns}
    taken = p.taken & set(init)
    sm.out = dict(init)
    sm.solve()
    rounds = 0
    while True:
        rounds += 1
        need, ind = dict(init), 0
        for fn in fns:
            analyze(fn, sm.get, sm.out[fn.entry])
            live, rr, oc = fn.live, reach_ret(fn), sm.out[fn.entry]
            for a in fn.order:
                lo = ro = 0
                for t in fn.succ[a]:
                    lo |= live[t]
                    ro |= rr[t]
                for i in fn.ev[a]:
                    k, c, _ = fn.events[i]
                    if k not in ('call', 'tail') or not c.startswith(('f_', 'call_indirect')):
                        continue
                    m = ((lo | (ro & oc)) if k == 'call' else oc) & RETU
                    if c == 'call_indirect':
                        ind |= m
                    elif int(c[2:], 16) in need:
                        need[int(c[2:], 16)] |= m
        for e in taken:
            need[e] |= ind
        grew = [e for e in need if need[e] & ~sm.out[e]]
        if not grew:
            break
        for e in grew:
            sm.out[e] |= need[e]
        sm.solve(grew)
    n = sum(popc(sm.out[fn.entry]) for fn in fns)
    names = [f'r{k}' for k in range(32)] + [f'f{k}' for k in range(32)] + [f'cr{k}' for k in range(8)] + ['ctr', 'ca', 'so']
    print(f'out(): {rounds} rounds, {len(taken)} indirect targets get ' + ','.join(names[b] for b in range(75) if ind >> b & 1) +
          f'; {n} return registers (all of RETU: {popc(RETU) * len(fns)})')


def check_call_reads(fns):
    """EABI check for the registers a caller keeps in locals although a callee may write them in its
    own locals without storing them (not RETU): ctr, ca and cr0 must never be read after a call
    before being written (the caller would see its own value, hardware the callee's). cr5-7 are read
    after calls only through mfcr (5 functions, 2026-10-22), whose result feeds a cr0 single-bit
    extract (srwi/rlwinm .., 31, 31): counted, not an error."""
    bad, cr57 = [], 0
    for fn in fns:
        live = liveness(fn, lambda c: (0, 0), {})
        for a in fn.order:
            if not any(fn.events[i][0] == 'call' for i in fn.ev[a]):
                continue
            m = 0
            for t in fn.succ[a]:
                m |= live[t]
            if m & (CTR | CA | CB(0)):
                bad.append(f'{a:08x}')
            cr57 += bool(m & CB(5, 6, 7))
    print(f'calls followed by a read of cr5-7 (mfcr): {cr57}' +
          (f'; NOTE ctr/ca/cr0 read after a call (EABI broken, see check_call_reads): {", ".join(bad)}' if bad else ''))


def emit(fn, sites, prof=None):
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
        # xer_so is 0 or 1 (set_xer, mcrxr); `& 1u` tells GCC, so `cmp | so` branches test the compare only
        out += [f'{n} = cpu.{v};' for n, v in (('ctr', 'ctr'), ('ca', 'xer_ca'), ('so', 'xer_so & 1u'))
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
    if prof is not None:
        out[-1] += f' SITEC({len(prof)});'
        prof.append(('entry', entry, '', fn.entry_live & fn.used, 0, len(fn.order)))
    for a in fn.order:
        if a in fn.labels:
            out.append(f'L_{a:08x}:;')
        out.extend('    ' + x for x in fn.lines[a])
        if a in fn.gotos:
            out.append(f'    goto L_{a + 4:08x};')
    out.append('}')
    src = '\n'.join(out)

    def site(i):
        k, c, _ = fn.events[i]
        if prof is None or k not in ('call', 'tail', 'ret'):
            return spill(sites[i][0])
        prof.append((k, entry, c or '', sites[i][0], sites[i][1], 0))
        return f'SITEC({len(prof) - 1}); ' + spill(sites[i][0])
    return re.sub(r'@([SL])(\d+)@', lambda m: (site(int(m.group(2))) if m.group(1) == 'S'
                                                else reload(sites[int(m.group(2))][1])), src)


def main():
    # a regex escape lost to a shell heredoc becomes a control character and silently matches nothing
    bad = [n for n, line in enumerate(Path(__file__).read_bytes().split(b'\n'), 1) if re.search(rb'[\x00-\x08\x0b\x0c\x0e-\x1f]', line)]
    if bad:
        sys.exit(f'recomp.py has control characters on lines {bad}')
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
    # first pass finds the leaves to inline; the second translates with them inlined
    keep = (copy.deepcopy(p.unk), list(p.gqr0_written), getattr(p, 'sda2_folds', 0))
    p.inline = inline_leaves(p, fold_so(fold_base_regs([translate_function(p, a) for a in tr], quiet=True), quiet=True))
    p.unk, p.gqr0_written, p.sda2_folds = keep
    p.covered = set()
    fns = [translate_function(p, a) for a in tr]
    fns = fold_base_regs(fns)
    fns = fold_so(fns)
    fns = drop_nv_saves(fns)
    # pointer-extent promotion (RECOMP_PTR_EXTENT): 'dual' (default) emits both bodies of every function
    # whose C differs, f_X dispatching to fp_X at entry while recomp_ptr_extent is set (runtime/main.c:
    # sdmc:/cr_ptrext_off.txt, button "Ptr extent"); '1' = extent bodies only, '0' = the old rule only
    pe_mode = os.environ.get('RECOMP_PTR_EXTENT', 'dual')
    if pe_mode not in ('0', '1', 'dual'):
        sys.exit('RECOMP_PTR_EXTENT must be 0, 1 or dual')
    fns_pe = promote_stack_slots(list(fns), not p.gqr0_written, p.stubs, True) if pe_mode == 'dual' else None
    fns = promote_stack_slots(fns, not p.gqr0_written, p.stubs, pe_mode == '1')
    n = sum(len(re.findall(r'/\* inlined f_', x)) for fn in fns for v in fn.body.values() for x in v)
    print(f'{n} calls to {len(p.inline)} leaf functions inlined')
    for fn in fns:
        if fn.order[0] != fn.entry: print(f"note: f_{fn.entry:08x} starts at {fn.order[0]:08x}")
    check_native_writes()
    check_gqr_writes()
    funcs_pe = None
    if fns_pe is not None:   # first: Func objects both lists share keep the default pass's analysis
        sm_pe = Summaries(p, fns_pe)
        sm_pe.native_in = native_reads()
        solve_out(p, fns_pe, sm_pe)
        funcs_pe = [emit(fn, analyze(fn, sm_pe.get, sm_pe.out[fn.entry])[2]) for fn in fns_pe]
    sm = Summaries(p, fns)
    sm.native_in = native_reads()
    solve_out(p, fns, sm)
    if fns_pe is not None:   # a caller of either body sees the same register interface (bodies mix at run time)
        bad = [f'{e:08x}' for e in sm.s if sm.s[e] != sm_pe.s[e] or sm.out[e] != sm_pe.out[e]]
        if bad:
            sys.exit('RECOMP_PTR_EXTENT=dual: register summaries differ between the bodies of ' + ', '.join(bad[:20]))
    check_call_reads(fns)
    funcs, n = [], [0] * 5
    prof = [] if os.environ.get('RECOMP_SITEPROF') == '1' else None
    for fn in fns:
        sites = analyze(fn, sm.get, sm.out[fn.entry])[2]
        funcs.append(emit(fn, sites, prof))
        for i, (sp, rl) in sites.items():
            if fn.events[i][0] == 'call':
                n = [x + y for x, y in zip(n, (1, popc(sp), popc(rl), popc(fn.W & CALLU), popc(RELU & fn.used)))]
    print(f'{n[0]} call sites: {n[1]} register stores (fixed sets: {n[3]}), {n[2]} reloads ({n[4]})')
    pe_body = {}   # fns index -> fp_<entry> C (pointer-extent body), for functions whose C differs
    if funcs_pe is not None:
        every = os.environ.get('RECOMP_PTR_EXTENT_ALL') == '1'
        differ = [i for i, fn in enumerate(fns) if funcs_pe[i] != funcs[i]]
        for i in differ:
            fn = fns[i]
            if every or fn.entry in PTR_EXTENT_DUAL:
                h0 = f'void f_{fn.entry:08x}(void) {{'
                pe_body[i] = funcs_pe[i].replace(h0, f'void fp_{fn.entry:08x}(void) {{', 1)
                funcs[i] = funcs[i].replace(h0, h0 + f'\n    if (recomp_ptr_extent) {{ fp_{fn.entry:08x}(); return; }}', 1)
        miss = PTR_EXTENT_DUAL - {fns[i].entry for i in differ}
        if miss:
            print('note: PTR_EXTENT_DUAL functions whose bodies no longer differ: ' + ', '.join(f'{a:08x}' for a in sorted(miss)))
        print(f'pointer-extent promotion: {len(pe_body)} of {len(differ)} differing functions get a second body fp_ '
              f'(RECOMP_PTR_EXTENT=dual{", _ALL=1" if every else ""}, run-time recomp_ptr_extent)')
    else:
        print(f'pointer-extent promotion: {"extent bodies only" if pe_mode == "1" else "off"} (RECOMP_PTR_EXTENT={pe_mode})')
    hdr = ['/* generated by tools/recomp/recomp.py -- do not edit */', '#pragma once']
    if p.gqr0_written:   # psq_ld0/psq_st0 (ppc.h) fall back to the general GQR path
        print('note: mtspr GQR0 at ' + ', '.join(f'{a:08x}' for a in p.gqr0_written) + ': RECOMP_GQR0_WRITTEN')
        hdr.append('#define RECOMP_GQR0_WRITTEN 1')
    hdr.append('#include "ppc.h"')
    if prof is not None:
        hdr += [f'extern u32 recomp_site_cnt[{len(prof)}];', '#define SITEC(k) (recomp_site_cnt[k]++)']
        rows = ['k\tkind\tfn\tcallee\tstore\treload\tninsn']
        rows += [f'{k}\t{r[0]}\t{r[1]:08x}\t{r[2]}\t{r[3]:x}\t{r[4]:x}\t{r[5]}' for k, r in enumerate(prof)]
        write_if_changed(out / 'sites.tsv', '\n'.join(rows) + '\n')
    hdr += [f'void f_{a:08x}(void);' for a in tr]
    hdr += [f'void fp_{fns[i].entry:08x}(void);' for i in sorted(pe_body)]
    hdr += [f'void {n}(void);' for n in sorted(set(p.stubs.values()))]
    write_if_changed(out / 'funcs.h', '\n'.join(hdr) + '\n')
    # selective inlining: each listed bl calls fi_<callee>, a copy defined at the top of the caller's file.
    # RECOMP_INLINE_SITES: 'dual' (default) = `if (recomp_inline_sites) fi_..(); else f_..();` (runtime/main.c:
    # sdmc:/cr_inline_off.txt, button "Inline sites"); '1' = always the copy; '0' = plain calls
    in_mode = os.environ.get('RECOMP_INLINE_SITES', 'dual')
    if in_mode not in ('0', '1', 'dual'):
        sys.exit('RECOMP_INLINE_SITES must be 0, 1 or dual')
    copies = defaultdict(dict)   # caller function index -> {sort key: C}
    if in_mode != '0':
        idx = {fn.entry: i for i, fn in enumerate(fns)}
        inl = 'static inline __attribute__((always_inline)) void'
        for a, t in INLINE_SITES:
            rx = re.compile(rf'(cpu\.lr = 0x{a + 4:08x}u; [^\n]*?)\bf_{t:08x}\(\);')
            call = (f'{{ if (recomp_inline_sites) fi_{t:08x}(); else f_{t:08x}(); }}' if in_mode == 'dual'
                    else f'fi_{t:08x}();')
            for i in [i for i, fn in enumerate(fns) if a in fn.body]:   # every body that holds the bl
                for d in (funcs, pe_body):   # a caller with two bodies holds the site in both
                    if d is funcs or i in pe_body:
                        assert len(rx.findall(d[i])) == 1, f'inline site {a:08x} -> {t:08x} not found once'
                        d[i] = rx.sub(lambda m: m.group(1) + call, d[i])
                j, cp = idx[t], copies[i]
                cp[f'{t:08x}b'] = funcs[j].replace(f'void f_{t:08x}(void) {{', f'{inl} fi_{t:08x}(void) {{', 1)
                if j in pe_body:   # a callee with two bodies: its extent body is inlined there too
                    cp[f'{t:08x}b'] = cp[f'{t:08x}b'].replace(f'{{ fp_{t:08x}(); return; }}', f'{{ fpi_{t:08x}(); return; }}', 1)
                    cp[f'{t:08x}a'] = pe_body[j].replace(f'void fp_{t:08x}(void) {{', f'{inl} fpi_{t:08x}(void) {{', 1)
        print(f'{len(INLINE_SITES)} call sites to {len({t for _, t in INLINE_SITES})} functions inlined (INLINE_SITES, '
              f'RECOMP_INLINE_SITES={in_mode}{", run-time recomp_inline_sites" if in_mode == "dual" else ""})')
    else:
        print('note: INLINE_SITES off (RECOMP_INLINE_SITES=0)')
    # Store checks (runtime/ppc.h STORE_HW): a function whose C sets a hardware address (0xCC/0xCD...... literal, e.g.
    # lis 0xCC01 for the GX write-gather pipe), or that holds an inline copy of one, goes to recomp_hw_*.c, where
    # every store tests for hardware addresses; the other files are RECOMP_STORE_FAST (stores write RAM untested)
    hwc = re.compile(r'\b0xc[cd][0-9a-f]{6}u\b', re.I)
    hw = {i for i in range(len(funcs)) if hwc.search(funcs[i]) or (i in pe_body and hwc.search(pe_body[i]))}
    hw |= {i for i, cp in copies.items() if any(hwc.search(c) for c in cp.values())}
    print(f'store checks kept in {len(hw)} functions that set a hardware address (recomp_hw_*.c)')
    names = set()
    for prefix, fast, part in (('recomp_', True, [i for i in range(len(funcs)) if i not in hw]),
                               ('recomp_hw_', False, sorted(hw))):
        for k in range(0, len(part), FUNCS_PER_FILE):
            name = f'{prefix}{k // FUNCS_PER_FILE:03d}.c'
            src = '/* generated by tools/recomp/recomp.py -- do not edit */\n'
            src += '#define RECOMP_STORE_FAST 1\n' if fast else '/* functions that set a hardware address: stores keep the check */\n'
            src += '#include "funcs.h"\n\n'
            ix = part[k:k + FUNCS_PER_FILE]
            src += ''.join(c + '\n\n' for _, c in sorted({key: c for i in ix for key, c in copies[i].items()}.items()))
            names.add(name)
            body = [funcs[i] + (f'\n\n{pe_body[i]}' if i in pe_body else '') for i in ix]
            write_if_changed(out / name, src + '\n\n'.join(body) + '\n')
    for f in out.glob('recomp_*.c'):   # left over from a larger split
        if f.name not in names: f.unlink()
    # SDK stubs: weak, they only report the call; runtime/ provides the real ones
    st = ['/* generated: weak logging SDK stubs, overridden by runtime/ */', '#include "funcs.h"']
    for a, n in sorted(p.stubs.items()):
        body = f'f_{a:08x}();' if a in BODY_NATIVE else f'recomp_sdk_call("{n[4:]}");'
        st.append(f'__attribute__((weak)) void {n}(void) {{ {body} }}  /* 0x{a:08x} {p.module(a)} */')
    write_if_changed(out / 'sdk_stubs.c', '\n'.join(dict.fromkeys(st)) + '\n')
    # function table for bctrl/blrl/computed tail calls
    skip = sorted(a for a in p.starts if p.kind(a) == 'skip')
    ft = ['/* generated: guest address -> host function */', '#include "funcs.h"']
    ft += [f'static void skip_{a:08x}(void) {{ recomp_skipped(0x{a:08x}); }}' for a in skip]
    # starts recomp.py saw no pointer to (not p.taken): out() gave their returns only the stores
    # their known callers read, so an indirect call to one is logged (its results may be stale)
    ft.append('''static void untaken(u32 a) {
    static u32 n;
    if (n++ < 16) recomp_log("# call_indirect %08lx: no pointer to it seen by recomp.py, return stores per out() (recomp_unknowns.md)\\n", (unsigned long)a);
}''')
    ft += [f'static void u_{a:08x}(void) {{ untaken(0x{a:08x}u); f_{a:08x}(); }}' for a in tr
           if a not in p.taken and a not in BODY_NATIVE]
    # MTX bodies are reached through their sdk_ natives (weak stubs), as direct calls are
    ent = ([(a, f'f_{a:08x}' if a in p.taken else f'u_{a:08x}') for a in tr if a not in BODY_NATIVE] +
           list(p.stubs.items()) + [(a, f'skip_{a:08x}') for a in skip])
    ent.sort()
    # open addressing, linear probing, at most half full (was a binary search: ~12 probes per call,
    # 1.3k calls per battle frame); slot 0 address = empty (no function starts at 0)
    bits = (2 * len(ent) - 1).bit_length()
    slot = lambda a: (((a >> 2) * 0x9E3779B1) & 0xFFFFFFFF) >> (32 - bits)
    tab, probes = [None] * (1 << bits), 0
    for a, n in ent:
        k, j = slot(a), 1
        while tab[k]:
            k, j = (k + 1) & ((1 << bits) - 1), j + 1
        tab[k], probes = (a, n), max(probes, j)
    ft.append(f'/* {len(ent)} entries, longest probe sequence {probes} */')
    ft.append(f'static const struct {{ u32 a; GuestFn f; }} table[{1 << bits}] = {{')
    ft += [f'    [{k}] = {{0x{e[0]:08x}u, {e[1]}}},' for k, e in enumerate(tab) if e]
    ft.append('};')
    ft.append(f'''void call_indirect(u32 t) {{
    for (u32 k = ((t >> 2) * 0x9E3779B1u) >> {32 - bits}; table[k].a; k = (k + 1) & {(1 << bits) - 1}u)
        if (table[k].a == t) {{ table[k].f(); return; }}
    recomp_bad(t, "call_indirect: no function at this address");
}}''')
    if prof is not None:
        ft.append(f'u32 recomp_site_cnt[{len(prof)}];')
        ft.append(f'''void recomp_site_dump(u32 retrace) {{
    static u32 last[{len(prof)}];
    for (u32 k = 0; k < {len(prof)}u; k++) if (recomp_site_cnt[k] != last[k]) {{
        recomp_log("# site %lu %lu %lu\\n", (unsigned long)retrace, (unsigned long)k, (unsigned long)(recomp_site_cnt[k] - last[k]));
        last[k] = recomp_site_cnt[k];
    }}
}}''')
    write_if_changed(out / 'functable.c', '\n'.join(ft) + '\n')
    for a, w in p.all_text_words():
        if w and p.kind(a) == 'tr' and a not in p.covered and decode(w):
            p.unk['valid code not reached from any function start'].append((a, ''))
    write_unknowns(p, len(tr), len(set(p.stubs.values())), len(skip))
    print(f'translated {len(tr)} functions, {len(p.stubs)} sdk stubs, {len(skip)} skipped, '
          f'{len(p.jt)} jump tables, {sum(len(v) for v in p.unk.values())} unknown sites')
    if p.sda2:
        print(f'r2 = 0x{p.sda2[0]:08x}: {getattr(p, "sda2_folds", 0)} sdata2 loads folded to constants')


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
