"""Hot-code order for the linker (tools/recomp/hot_order.ld, used by `make` unless HOTORDER=0).

The 2v2's hot code (~185 translated functions entered >= 10 times a frame, ~526 KB) is spread over ~5.7 MB of .text
and waits ~28% of core 0's cycles for instructions (docs/performance.md "Open"). This packs the functions a frame
enters next to the ones that call them, hottest clusters first, so hot code shares cache lines and L2 pages with hot
code instead of cold neighbours: C3 clustering (Ottoni & Maher, CGO 2017): functions in decreasing order of heat
join the cluster of their most frequent caller while the cluster stays under CAP bytes; clusters are then sorted by
heat per byte. Heat of a translated function = entries per frame x guest instructions; of a native = calls x 20.

From a RECOMP_SITEPROF=1 replay (as tools/recomp/inline_sites.py) and the map of a normal build (section sizes):
    PYTHONUTF8=1 python tools/recomp/hot_order.py <sites.tsv> <siteprof log> <r0> <r1> build/custom-robo-recomp.map
Only names change between builds of main, not guest addresses, so an older profile still orders the same functions;
sections it names that a build lacks are ignored by ld."""
import collections, re, sys
from pathlib import Path

OUT = Path(__file__).with_name('hot_order.ld')
CAP = 16 * 1024          # bytes per cluster: the I-cache size
MIN_ENTRIES = 0.5        # per frame, to be placed at all
NATIVE_HEAT = 20         # guest-instruction equivalent of one native call
EXTRA = ('call_indirect', 'recomp_poll_events')   # runtime helpers every frame reaches


def section_sizes(mapfile):
    sizes, name = {}, None
    rx = re.compile(r'\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s')
    for line in open(mapfile, encoding='utf-8', errors='replace'):
        m = re.match(r'^ (\.text\.\S+)(.*)$', line)
        if m:
            name, rest = m[1], m[2] + ' '
            s = rx.match(rest)
            if s:
                sizes[name] = int(s[1], 16); name = None
            continue
        if name:
            s = rx.match(line)
            if s: sizes[name] = int(s[1], 16)
            name = None
    return sizes


def main():
    sites, log, r0, r1, mapfile = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    sizes = section_sizes(mapfile)
    rows = {}
    for line in open(sites, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f[0] != 'k': rows[int(f[0])] = f
    cnt = collections.Counter()
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.match(r'# site (\d+) (\d+) (\d+)', line)
        if m and r0 < int(m[1]) <= r1: cnt[int(m[2])] += int(m[3])
    frames = r1 - r0
    entries, ninsn, edge = collections.Counter(), {}, collections.Counter()
    for k, f in rows.items():
        n = cnt[k] / frames
        if f[1] == 'entry':
            fn = 'f_' + f[2]; entries[fn] += n; ninsn[fn] = int(f[6])
        elif f[1] in ('call', 'tail') and (f[3].startswith('f_') or f[3].startswith('sdk_')) and n > 0:
            edge[('f_' + f[2], f[3])] += n
    for (c, t), n in edge.items():
        if t.startswith('sdk_'): entries[t] += n
    heat = {fn: n * (ninsn.get(fn, NATIVE_HEAT)) for fn, n in entries.items()
            if n >= MIN_ENTRIES and '.text.' + fn in sizes}
    callers = collections.defaultdict(list)
    for (c, t), n in edge.items():
        if c in heat and t in heat and c != t: callers[t].append((n, c))
    size = lambda fn: sizes['.text.' + fn]
    cl = {fn: [fn] for fn in heat}            # cluster of each function (list, shared object)
    for fn in sorted(heat, key=lambda x: -heat[x]):
        if not callers[fn]: continue
        _, c = max(callers[fn])
        a, b = cl[c], cl[fn]
        if a is b or sum(map(size, a)) + sum(map(size, b)) > CAP: continue
        a.extend(b)
        for x in b: cl[x] = a
    clusters = {id(v): v for v in cl.values()}.values()
    order = sorted(clusters, key=lambda v: -sum(heat[x] for x in v) / max(1, sum(map(size, v))))
    names = [x for v in order for x in v]
    for x in EXTRA:
        if '.text.' + x in sizes and x not in names: names.insert(0, x)
    total = sum(size(x) for x in names)
    lines = [f'/* tools/recomp/hot_order.py: {Path(log).name} r{r0}-{r1}, {len(names)} functions, {total} bytes, '
             f'{len(order)} clusters (cap {CAP}); placed first in .text by `make` (HOTORDER=0: link order) */',
             '.text : {',
             '    KEEP(*(.crt0))   /* 3dsxtool: the entry point must stay at the start of .text (3dsx.ld) */',
             '    KEEP(*(.init))']
    lines += [f'    *(.text.{x})' for x in names]
    lines.append('}')
    OUT.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'{len(names)} functions ({sum(1 for x in names if x.startswith("f_"))} translated), {total} bytes, '
          f'{len(order)} clusters -> {OUT}')


if __name__ == '__main__':
    main()
