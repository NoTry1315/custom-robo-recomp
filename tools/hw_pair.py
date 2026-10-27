"""Paired A/B of two hardware replay logs (same replay, same route): per `# time` window game ms and per `# perf`
window core-0 busy cycles, B - A, per scene. Pairing by window removes the route from the comparison; on the 2v2
replay two runs of one build differ by sd ~0.13 ms per window (runs 37b/39a, 2026-10-26).
    PYTHONUTF8=1 python tools/hw_pair.py logs/<A>.log logs/<B>.log [scene ...]   (default scenes 4.2 8.3)"""
import re, sys, statistics as st


def rd(p):
    t, c, sc, cur = {}, {}, {}, '?'
    for l in open(p, encoding='utf-8', errors='replace'):
        m = re.match(r'# retrace (\d+) scene (\w+) sub (\w+)', l)
        if m: cur = f'{m[2]}.{m[3]}'
        m = re.match(r'# time r(\d+): [\d.]+ fps, frame ([\d.]+) ms \(worst ([\d.]+)\): game ([\d.]+)', l)
        if m: t[int(m[1])] = (float(m[4]), float(m[3]), float(m[2])); sc[int(m[1])] = cur
        m = re.match(r'# perf r(\d+): core 0 per frame .*?cycles ([\d.]+)', l)
        if m: c[int(m[1])] = float(m[2])
    return t, c, sc


A, B = rd(sys.argv[1]), rd(sys.argv[2])
for scene in sys.argv[3:] or ['4.2', '8.3']:
    ks = [k for k in A[0] if k in B[0] and A[2][k] == scene]
    if len(ks) < 3: continue
    d = [B[0][k][0] - A[0][k][0] for k in ks]
    dc = [(B[1][k] - A[1][k]) / 1e6 for k in ks if k in A[1] and k in B[1]]
    ga, gb = st.mean(A[0][k][0] for k in ks), st.mean(B[0][k][0] for k in ks)
    fa, fb = st.mean(A[0][k][2] for k in ks), st.mean(B[0][k][2] for k in ks)
    print(f'scene {scene}: n {len(d)}, game {ga:.2f} -> {gb:.2f} ms (B-A mean {st.mean(d):+.2f}, median {st.median(d):+.2f}, '
          f'sd {st.stdev(d):.2f}, B faster in {sum(v < 0 for v in d)}), frame {fa:.2f} -> {fb:.2f} ms ({1000 / fa:.1f} -> {1000 / fb:.1f} fps)'
          + (f'; cycles B-A {st.mean(dc):+.3f}M (sd {st.stdev(dc):.3f}, fewer in {sum(v < 0 for v in dc)}/{len(dc)})' if len(dc) > 2 else ''))
    wa = sorted(A[0][k][1] for k in ks)[-1]; wb = sorted(B[0][k][1] for k in ks)[-1]
    print(f'  worst frame A {wa:.1f} ms, B {wb:.1f} ms')
