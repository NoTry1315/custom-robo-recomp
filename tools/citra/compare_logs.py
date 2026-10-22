"""Are two replay runs the same guest run? Compares the deterministic lines of two cr_autotest logs
(SDK calls with r3-r6/lr, pad, scene, DONE, summary counts) and their framebuffer dumps
(<log stem>-cr_fb_<n>.bmp next to each log, as tools/citra/replay.ps1 writes them).
    PYTHONUTF8=1 python tools/citra/compare_logs.py logs/perf/base-battle.log logs/perf/new-battle.log
Exit status 1 when anything differs. Known non-deterministic values are masked: the host wall clock
in OSTicksToCalendarTime (r3, r4) and a leftover r4 at VIGetRetraceCount. Summary rows are compared
as a set (qsort orders equal counts differently per binary)."""
import difflib, hashlib, re, sys
from pathlib import Path


def key_lines(path):
    out, summ, in_sum = [], [], False
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('# summary'):
            in_sum = True
        elif line.startswith('# last calls'):
            in_sum = False
        if line.startswith('sdk '):
            if line.startswith('sdk OSTicksToCalendarTime'):
                line = re.sub(r'r3=\S+ r4=\S+', 'r3=* r4=*', line)
            elif line.startswith('sdk VIGetRetraceCount'):
                line = re.sub(r'r4=\S+', 'r4=*', line)
            out.append(line)
        elif line.startswith(('# pad r', '# retrace ', '# DONE')):
            out.append(line)
        elif in_sum and line.startswith('#  '):
            summ.append(line)
    return out + sorted(summ)


def main():
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    la, lb = key_lines(a), key_lines(b)
    diff = [d for d in difflib.unified_diff(la, lb, a.name, b.name, n=0, lineterm='')
            if not d.startswith(('---', '+++', '@@'))]
    print(f'{a.name} vs {b.name}: {len(la)} / {len(lb)} key lines, {len(diff)} differing')
    for d in diff[:12]:
        print('   ', d[:200])
    bad = bool(diff) or not la
    for fa in sorted(a.parent.glob(a.stem + '-cr_fb_*.bmp')):
        fb = b.parent / (b.stem + fa.name[len(a.stem):])
        same = fb.exists() and hashlib.md5(fa.read_bytes()).digest() == hashlib.md5(fb.read_bytes()).digest()
        bad |= not same
        print(f'  {fa.name[len(a.stem) + 1:]}: {"identical" if same else "DIFFERENT" if fb.exists() else "missing"}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
