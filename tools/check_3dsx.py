#!/usr/bin/env python3
"""Fail if a .3dsx has a relocation the 3DS loaders reject.

    python3 tools/check_3dsx.py custom-robo-recomp.3dsx

The 3dsx format keeps a relocation's sub-type in the top 4 bits of the patched
word, so an absolute relocation whose linked value is >= 0x10000000 (symbol plus
a large addend, e.g. `ram - 0x80000000 + k` folded by GCC's IVOPTS) cannot be
loaded: Citra stops with "Error while loading ROM!" (Load3DSXFile, sub_type != 0).
Checked the same way as the loader: absolute words need sub-type 0, relative
words 0 or 1 (prel31)."""
import struct
import sys


def check(path):
    d = open(path, 'rb').read()
    magic, hsize, rsize, _ver, _flags, code, ro, data, bss = struct.unpack_from('<4sHHIIIIII', d, 0)
    if magic != b'3DSX':
        return [f'{path}: not a 3dsx']
    ntab = rsize // 4
    counts = [struct.unpack_from(f'<{ntab}I', d, hsize + rsize * i) for i in range(3)]
    off = hsize + rsize * 3
    segs = []
    for size in (code, ro, data - bss):
        segs.append(d[off:off + size])
        off += size
    base = 0x00100000
    starts = [base, base + ((code + 0xfff) & ~0xfff)]
    starts.append(starts[1] + ((ro + 0xfff) & ~0xfff))
    bad = []
    for s in range(3):
        for t in range(ntab):
            n = counts[s][t]
            ents = [struct.unpack_from('<HH', d, off + 4 * k) for k in range(n)]
            off += 4 * n
            if t >= 2:
                continue
            pos, end = 0, len(segs[s]) // 4
            for skip, patch in ents:
                pos += skip
                while patch > 0 and pos < end:
                    w = struct.unpack_from('<I', segs[s], 4 * pos)[0]
                    if (w >> 28) not in ((0,) if t == 0 else (0, 1)):
                        bad.append(f'{path}: segment {s} {"absolute" if t == 0 else "relative"} relocation '
                                   f'at {starts[s] + 4 * pos:#x} = {w:#010x}')
                    pos += 1
                    patch -= 1
    return bad


if __name__ == '__main__':
    errs = [e for p in sys.argv[1:] for e in check(p)]
    for e in errs[:20]:
        print(e)
    if errs:
        sys.exit(f'{len(errs)} relocation(s) a 3DS loader rejects (see tools/check_3dsx.py)')
