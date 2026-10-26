"""Per-scene averages of the '# time' windows in cr_autotest logs, split by GX thread mode, gpukick and the
campaign-gpu flags (cull, vsh, mipmaps, tex 16-bit, vram tex) and native sin/cos (libm) when the split line has them.
Reads both '# time' formats (inline: game, gx vertex..; GX thread: game, gx wait..; gx thread busy..).
Windows with a toggle inside (mixed, or a '# menu r<n>:' bottom-screen flag toggle) are dropped.
    PYTHONUTF8=1 python tools/time_avg.py logs/perf/base-battle.log logs/perf/new-battle.log [--from 2460]
--from N: only windows ending at retrace >= N (skip a scene's first windows, e.g. loading)."""
import re, sys, collections

OLD = re.compile(r'^# time r(\d+): ([\d.]+) fps, frame ([\d.]+) ms \(worst ([\d.]+)\): game ([\d.]+), gx vertex ([\d.]+), '
                 r'gx tex ([\d.]+), gx state ([\d.]+), gx end/wait ([\d.]+), gx sync ([\d.]+), audio ([\d.]+)')
NEW = re.compile(r'^# time r(\d+): ([\d.]+) fps, frame ([\d.]+) ms \(worst ([\d.]+)\): game ([\d.]+), gx wait ([\d.]+), '
                 r'audio ([\d.]+) .*gx thread busy ([\d.]+): vertex ([\d.]+), tex ([\d.]+), state ([\d.]+), end/wait ([\d.]+), sync ([\d.]+)')
SPLIT = re.compile(r'^# time r(\d+) split: .*gpu ([\d.]+) \(tail.*gpu span ([\d.]+).*gpukick (\w+)(?:, gx thread (\w+))?')
GPUF = [('cull', re.compile(r'cull (\w+)')), ('vsh', re.compile(r'vsh (\w+)')), ('mip', re.compile(r'mipmaps (\w+)')),
        ('t16', re.compile(r'tex 16-bit (\w+)')), ('vram', re.compile(r'vram tex (\w+)')),
        ('vpack', re.compile(r'vtx pack (\w+)')),
        ('libm', re.compile(r'libm (\w+)')), ('lazyd', re.compile(r'lazy desc (\w+)')),
        ('ptrx', re.compile(r'ptr extent (\w+)')), ('inl', re.compile(r'inline sites (\w+)'))]   # A/B flags; absent in older logs
MENU = re.compile(r'^# menu r(\d+): ')
SCENE = re.compile(r'^# retrace (\d+) scene (\w+) sub (\w+)')
KEYS = ['frame', 'worst', 'game', 'gxwait', 'busy', 'vtx', 'tex', 'state', 'sync', 'audio', 'gpu', 'span']


def windows(path):
    scene, rows, menu = {}, {}, []
    for line in open(path, encoding='utf-8', errors='replace'):
        if m := MENU.match(line):
            menu.append(int(m[1]))
        elif m := SCENE.match(line):
            scene[int(m[1])] = f'{m[2]}.{m[3]}'
        elif m := NEW.match(line):
            g = [float(m[i]) for i in range(2, 14)]
            rows[int(m[1])] = dict(frame=g[1], worst=g[2], game=g[3], gxwait=g[4], audio=g[5], busy=g[6], vtx=g[7],
                                   tex=g[8], state=g[9], sync=g[11], thread='on')
        elif m := OLD.match(line):
            g = [float(m[i]) for i in range(2, 12)]
            rows[int(m[1])] = dict(frame=g[1], worst=g[2], game=g[3], gxwait=0, busy=0, vtx=g[4], tex=g[5], state=g[6],
                                   sync=g[8], audio=g[9], thread='off')
        elif (m := SPLIT.match(line)) and int(m[1]) in rows:
            d = rows[int(m[1])]
            d.update(gpu=float(m[2]), span=float(m[3]), kick=m[4])
            if m[5]:
                d['thread'] = m[5]
            d['flags'] = ' '.join(f'{k}={g[1]}' for k, rx in GPUF if (g := rx.search(line)))
    for r, d in rows.items():
        d['scene'] = scene.get(r) or scene.get(r - 30) or '?'
        d['r'] = r
        if any(r - 60 < t <= r for t in menu):
            d['kick'] = 'mixed'
    return list(rows.values())


def main():
    args, frm = sys.argv[1:], 0
    if '--from' in args:
        i = args.index('--from'); frm = int(args[i + 1]); del args[i:i + 2]
    for path in args:
        agg = collections.defaultdict(list)
        for d in windows(path):
            if d['r'] < frm or 'mixed' in (d.get('kick'), d.get('thread')) or 'mixed' in d.get('flags', ''):
                continue
            agg[(d['scene'], d['thread'], d.get('kick', '?'), d.get('flags', ''))].append(d)
        print(f'== {path}')
        print(f'{"scene":6s} {"gxthr":5s} {"kick":4s} {"n":>3s} ' + ' '.join(f'{k:>6s}' for k in KEYS) + '    fps  speed  flags')
        for (s, t, k, f), ds in sorted(agg.items()):
            avg = {key: sum(d.get(key, 0) for d in ds) / len(ds) for key in KEYS}
            print(f'{s:6s} {t:5s} {k:4s} {len(ds):3d} ' + ' '.join(f'{avg[key]:6.1f}' for key in KEYS) +
                  f'  {1000 / avg["frame"]:5.1f}  {min(100, 100000 / avg["frame"] / 60):4.0f}%  {f}')


if __name__ == '__main__':
    main()
