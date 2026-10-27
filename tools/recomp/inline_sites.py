"""Hot call sites to small translated functions, for recomp.py's selective inlining (tools/recomp/inline_sites.tsv).

From a RECOMP_SITEPROF=1 generation and a replay of it (docs/recomp_unknowns.md "Profile-guided inlining"):
    RECOMP_SITEPROF=1 PYTHONUTF8=1 python tools/recomp/recomp.py      # gen/ with SITEC counters + gen/sites.tsv
    make; tools/citra/replay.ps1 -Name 2v2 -Tag siteprof              # '# site <retrace> <k> <count>' lines
    PYTHONUTF8=1 python tools/recomp/inline_sites.py gen logs/perf/siteprof-2v2.log 4800 8400 [maxinsn] [mincalls]
Then regenerate the default gen/ (recomp.py deletes nothing: remove gen/sites.tsv by hand).

A site qualifies when it is a direct call (bl, not a tail call) to a translated function of at most maxinsn guest
instructions (default 64) that runs at least mincalls times per frame (default 10) in the window. Callees that are
callers of another qualifying site are left out, so an inline copy never holds inline copies itself; so are
self-calls. Writes one line per site: bl address, callee, calls per frame, callee size, caller."""
import collections, re, sys
from pathlib import Path

OUT = Path(__file__).with_name('inline_sites.tsv')


def main():
    gen, log, r0, r1 = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    maxn = int(sys.argv[5]) if len(sys.argv) > 5 else 64
    minc = float(sys.argv[6]) if len(sys.argv) > 6 else 10
    rows, size = {}, {}
    for line in open(gen / 'sites.tsv', encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'k':
            continue
        rows[int(f[0])] = f
        if f[1] == 'entry':
            size[int(f[2], 16)] = int(f[6])
    # SITEC(k) of a call site -> its bl address (the cpu.lr store in front of it is bl + 4)
    bl = {}
    rx = re.compile(r'cpu\.lr = 0x([0-9a-f]{8})u; SITEC\((\d+)\);[^\n]*?\bf_([0-9a-f]{8})\(\);')
    for p in sorted(gen.glob('recomp_*.c')):
        for m in rx.finditer(p.read_text(encoding='utf-8')):
            bl[int(m[2])] = (int(m[1], 16) - 4, int(m[3], 16))
    cnt = collections.Counter()
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.match(r'# site (\d+) (\d+) (\d+)', line)
        if m and r0 < int(m[1]) <= r1:
            cnt[int(m[2])] += int(m[3])
    frames = r1 - r0
    sel = {}
    for k, n in cnt.items():
        f = rows[k]
        if f[1] != 'call' or not f[3].startswith('f_') or k not in bl:
            continue
        a, t = bl[k]
        caller = int(f[2], 16)
        assert t == int(f[3][2:], 16), (k, f, hex(t))
        if t == caller or size.get(t, 1 << 30) > maxn or n / frames < minc:
            continue
        sel[a] = (t, n / frames, size[t], caller)
    callers = {c for _, _, _, c in sel.values()}
    sel = {a: v for a, v in sel.items() if v[0] not in callers}
    lines = ['# bl\tcallee\tcalls_per_frame\tcallee_insns\tcaller   (tools/recomp/inline_sites.py: '
             f'{Path(log).name} r{r0}-{r1}, callee <= {maxn} guest instructions, >= {minc:g} calls per frame)']
    for a, (t, n, sz, c) in sorted(sel.items(), key=lambda x: -x[1][1]):
        lines.append(f'{a:08x}\t{t:08x}\t{n:.1f}\t{sz}\t{c:08x}')
    OUT.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'{len(sel)} sites, {sum(v[1] for v in sel.values()):.0f} calls per frame, '
          f'{sum(v[2] for v in sel.values())} guest instructions copied -> {OUT}')


if __name__ == '__main__':
    main()
