"""Per-scene averages of the '# perf' windows (runtime/perfctr.c: ARM11 core 0 counters per frame, sdmc:/cr_perfctr.txt or
the bottom-screen button "Perf counters"), next to the '# time' game ms of the same window.
Windows with a bottom-screen toggle inside ('# menu r<n>:') are dropped, as in time_avg.py.
    PYTHONUTF8=1 python tools/perf_avg.py logs/<log> [--from 4300]
--from N: only windows ending at retrace >= N."""
import re, sys, collections

PERF = re.compile(r'^# perf r(\d+): core 0 per frame \((\d+) frames, (\d+) dropped\): cycles ([\d.]+) \(ccnt MHz ([\d.]+), '
                  r'wall ([\d.]+) ms\), inst ([\d.]+) \(ipc ([\d.]+)\), icache miss ([\d.]+) \(([\d.]+)/k inst\), dcache reads ([\d.]+), '
                  r'read miss ([\d.]+) \(([\d.]+)/k inst\), write miss ([\d.]+) \(([\d.]+)/k inst\), stall inst ([\d.]+) \(([\d.]+)%\), '
                  r'stall data ([\d.]+) \(([\d.]+)%\), branch mispredict ([\d.]+) \(([\d.]+)/k inst\); svc ([\d.]+) us')
TIME = re.compile(r'^# time r(\d+): [\d.]+ fps, frame ([\d.]+) ms \(worst [\d.]+\): game ([\d.]+)')
MENU = re.compile(r'^# menu r(\d+): ')
SCENE = re.compile(r'^# retrace (\d+) scene (\w+) sub (\w+)')
COLS = [('game', None, 'game ms'), ('wall', 5, 'wall ms'), ('cyc', 3, 'Mcycles'), ('mhz', 4, 'ccntMHz'), ('inst', 6, 'Minst'),
        ('ipc', 7, 'ipc'), ('imk', 9, 'imiss/k'), ('rmk', 12, 'rmiss/k'), ('wmk', 14, 'wmiss/k'), ('si', 16, 'stInst%'),
        ('sd', 18, 'stData%'), ('bmk', 20, 'brmis/k'), ('svc', 21, 'svc us')]   # index into PERF's groups()


def main():
    args, frm = sys.argv[1:], 0
    if '--from' in args:
        i = args.index('--from'); frm = int(args[i + 1]); del args[i:i + 2]
    for path in args:
        scene, game, menu, rows = {}, {}, [], {}
        for line in open(path, encoding='utf-8', errors='replace'):
            if m := MENU.match(line):
                menu.append(int(m[1]))
            elif m := SCENE.match(line):
                scene[int(m[1])] = f'{m[2]}.{m[3]}'
            elif m := TIME.match(line):
                game[int(m[1])] = float(m[3])
            elif m := PERF.match(line):
                rows[int(m[1])] = [float(x) for x in m.groups()]
        agg = collections.defaultdict(list)
        for r, g in rows.items():
            if r < frm or any(r - 60 < t <= r for t in menu):
                continue
            d = {'game': game.get(r, 0.0)}
            for k, i, _ in COLS[1:]:
                d[k] = g[i]
            d['cyc'] /= 1e6; d['inst'] /= 1e6
            agg[scene.get(r) or scene.get(r - 30) or '?'].append(d)
        print(f'== {path}')
        print(f'{"scene":6s} {"n":>3s} ' + ' '.join(f'{h:>8s}' for _, _, h in COLS))
        for s, ds in sorted(agg.items()):
            print(f'{s:6s} {len(ds):3d} ' + ' '.join(f'{sum(d[k] for d in ds) / len(ds):8.2f}' for k, _, _ in COLS))


if __name__ == '__main__':
    main()
