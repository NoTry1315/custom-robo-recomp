"""Call-boundary cost of the translated functions: every ARM instruction of each f_XXXXXXXX in a build is put in a
class by the gen/ line it comes from (objdump -l), and the call-boundary classes are weighted by RECOMP_SITEPROF
counts (calls per call site / function entry / return, '# site' lines of a replay).

Classes: frame (push/vpush/sub sp, pop/vpop/add sp), entry (the reload of R[]/F[]/cpu at function entry), site (one
call site's line: register stores to cpu before the call, reloads after it), ret (return stores), call (the bl/b to
f_/sdk_/call_indirect), cstack (other loads/stores through sp: C locals GCC spilled), body (the rest, inlined ppc.h
helpers included). Body and cstack are static only: site counts do not say how often a loop body runs.

Workflow (2026-10-26; docs/recomp_unknowns.md "Call-boundary cost on the ARM side"):
    RECOMP_PTR_EXTENT=0 RECOMP_INLINE_SITES=0 PYTHONUTF8=1 python tools/recomp/recomp.py      # plain gen/
    copy gen/*.c to <plain>; make; copy custom-robo-recomp.elf to <elf>
    arm-none-eabi-objdump -d -l --no-show-raw-insn <elf> > <dis>                               # in the build container
    RECOMP_SITEPROF=1 RECOMP_PTR_EXTENT=0 RECOMP_INLINE_SITES=0 python tools/recomp/recomp.py  # same line numbers
    copy gen/*.c and gen/sites.tsv to <site>; make; tools/citra/replay.ps1 -Name 2v2 -Tag siteprof
    PYTHONUTF8=1 python tools/callcost.py <dis> <plain> <site> logs/perf/siteprof-2v2.log 4500 8400
Afterwards regenerate the default gen/ (and delete gen/sites.tsv)."""
import collections, os, re, sys

FRAME_IN, FRAME_OUT = {'push', 'vpush'}, {'pop', 'vpop'}
INS = re.compile(r'^\s+([0-9a-f]+):\t(\S+)\s*(.*)$')
CALL = re.compile(r'<(f_[0-9a-f]{8}|sdk_\w+|call_indirect)>')
KIND = {'entry': 'entry reload', 'call': 'call-site spill/reload', 'tail': 'call-site spill/reload', 'ret': 'return stores'}


def lines_of(d, f):
    with open(os.path.join(d, f), encoding='utf-8') as h:
        return h.read().split('\n')


def opclass(op):
    if op in ('vstr', 'vstm', 'vstmia'): return 'FPR store'
    if op in ('vldr', 'vldm', 'vldmia'): return 'FPR load'
    if op in ('str', 'strd', 'stm', 'stmia'): return 'GPR store'
    if op in ('ldr', 'ldrd', 'ldm', 'ldmia'): return 'GPR load'
    return 'VFP other' if op.startswith('v') else 'other'


def main():
    dis, plain, site, slog, r0, r1 = sys.argv[1:5] + [int(x) for x in sys.argv[5:7]]
    src = {}

    def text(f, n):
        if f not in src: src[f] = lines_of(plain, f)
        return src[f][n - 1] if 0 < n <= len(src[f]) else ''

    # (file, line) -> site indices, from the SITEPROF gen/ (same line numbers as the plain one)
    sites, site_at = {}, {}
    with open(os.path.join(site, 'sites.tsv'), encoding='utf-8') as h:
        next(h)
        for row in h:
            k, kind, fn, callee = row.rstrip('\n').split('\t')[:4]
            sites[int(k)] = (kind, fn, callee)
    for f in os.listdir(site):
        if f.startswith('recomp_') and f.endswith('.c'):
            a = lines_of(site, f)
            if len(a) != len(lines_of(plain, f)):
                sys.exit(f'{f}: the SITEPROF and plain gen/ differ in line count (not the same generation)')
            for i, t in enumerate(a):
                for m in re.finditer(r'SITEC\((\d+)\)', t):
                    site_at.setdefault((f, i + 1), []).append(int(m[1]))

    cnt, wins = collections.Counter(), set()   # '# site <retrace> <k> <n>': counts since the previous dump (300 retraces)
    for t in open(slog, encoding='utf-8', errors='replace'):
        if (m := re.match(r'# site r?(\d+) (\d+) (\d+)', t)) and r0 < int(m[1]) <= r1:
            cnt[int(m[2])] += int(m[3]); wins.add(int(m[1]))
    per = {k: v / (300 * len(wins)) for k, v in cnt.items()} if wins else {}

    F = collections.defaultdict(lambda: dict(cat=collections.Counter(), lines=collections.Counter(),
                                             pro=0, epi=0, rets=0, push=0, vpush=0, n=0))
    lops = collections.defaultdict(collections.Counter)
    fun, cur = None, None
    for t in open(dis, encoding='utf-8', errors='replace'):
        if m := re.match(r'^[0-9a-f]+ <(\w+)>:', t):
            fun = m[1] if re.fullmatch(r'f_[0-9a-f]{8}', m[1]) else None
            cur = None
            continue
        if fun is None: continue
        if m := re.match(r'^/proj/(\S+?):(\d+)', t):
            cur = (m[1][4:], int(m[2])) if m[1].startswith('gen/') else None
            continue
        if not (m := INS.match(t)): continue
        op, args = m[2], m[3]
        d = F[fun]; d['n'] += 1
        line = text(*cur) if cur else ''
        if op in ('pop', 'ldm', 'ldmia') and 'pc' in args or op == 'bx' and args.startswith('lr'): d['rets'] += 1
        if op in FRAME_IN or re.match(r'sub\s+sp, sp', f'{op} {args}'):
            c = 'frame'; d['pro'] += 1
            if op == 'push': d['push'] += args.count(',') + 1
            if op == 'vpush':
                r = re.search(r'd(\d+)-d(\d+)', args)
                d['vpush'] += int(r[2]) - int(r[1]) + 1 if r else args.count(',') + 1
        elif op in FRAME_OUT or re.match(r'add\s+sp, sp', f'{op} {args}'):
            c = 'frame'; d['epi'] += 1
        elif op in ('bl', 'blx', 'b') and CALL.search(args):
            c = 'call'
        elif 'FNPROF(0x' in line and '= R[' in line and (cur not in site_at or sites[site_at[cur][0]][0] == 'entry'):
            c = 'entry'
        elif 'FNPROF(0);' in line:
            c = 'ret' if 'return;' in line else 'site'
        elif '[sp' in args and opclass(op) != 'other':
            c = 'cstack'
        else:
            c = 'body'
        d['cat'][c] += 1
        if c in ('entry', 'site', 'ret'):
            d['lines'][cur] += 1; lops[cur][opclass(op)] += 1

    tot = collections.Counter()
    for d in F.values(): tot.update(d['cat'])
    n = sum(tot.values())
    print(f'static: {len(F)} functions, {n} ARM instructions')
    for c, v in tot.most_common(): print(f'  {c:7s} {v:9d} {100 * v / n:5.1f}%')
    if not per:
        return

    ent = {'f_' + fn: per.get(k, 0) for k, (kind, fn, _) in sites.items() if kind == 'entry'}
    dyn, ops, byfn = collections.Counter(), collections.Counter(), collections.Counter()
    for name, d in F.items():
        e = ent.get(name, 0)
        if not e: continue
        fr = e * (d['pro'] + d['epi'] / max(d['rets'], 1))
        dyn['frame instructions'] += fr; byfn[name] += fr
        dyn['  GPRs pushed + popped'] += e * 2 * d['push']; dyn['  D registers vpushed + vpopped'] += e * 2 * d['vpush']
        for ln, k in d['lines'].items():
            ks = site_at.get(ln, [])
            c = sum(per.get(i, 0) for i in ks)
            key = KIND.get(sites[ks[0]][0] if ks else '', 'unmapped')
            dyn[key] += c * k; byfn[name] += c * k
            for oc, m in lops[ln].items(): ops[f'{key}: {oc}'] += c * m
    calls = sum(v for k, v in per.items() if sites[k][0] in ('call', 'tail'))
    print(f'\nper frame ({len(wins)} dumps in r{r0}-{r1}): {sum(ent.values()):.0f} translated entries, {calls:.0f} calls')
    for k, v in dyn.items(): print(f'  {k:40s} {v:12,.0f}')
    for k, v in sorted(ops.items()): print(f'    {k:46s} {v:10,.0f}')
    print('\ntop functions, call-boundary instructions per frame (frame + entry + sites + returns):')
    for name, v in byfn.most_common(25):
        d = F[name]
        print(f'  {name} {v:9,.0f}  entries {ent.get(name, 0):6.0f}  push {d["push"]} vpush d{d["vpush"]}  '
              f'size {d["n"]}  cstack {d["cat"]["cstack"]}')


if __name__ == '__main__':
    main()
