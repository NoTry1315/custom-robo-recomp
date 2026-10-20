/* MSL C library functions replaced natively (recomp.py NATIVE): same results
 * as the guest code, checked against the disassembly. Not logged (hot). */
#include <3ds.h>
#include <string.h>
#include <math.h>
#include "ppc.h"

/* guest range [a, a+n) lies in RAM without wrapping the mirror */
static int ram_range(u32 a, u32 n) { return !IS_HW(a) && (a & RAM_MASK) + n <= RAM_MASK + 1; }

/* 0x800054f4 memcpy(dst r3, src r4, n r5) -> dst: byte copy, backward when
 * src < dst, so it behaves as memmove */
void sdk_memcpy(void) {
    u32 d = R[3], s = R[4], n = R[5];
    if (!n) return;
    if (ram_range(d, n) && ram_range(s, n)) { recomp_mark_ram(d, n); memmove(ram + (d & RAM_MASK), ram + (s & RAM_MASK), n); return; }
    if (s < d) for (u32 i = n; i--; ) wr8(d + i, rd8(s + i));
    else for (u32 i = 0; i < n; i++) wr8(d + i, rd8(s + i));
}

/* 0x8000540c memset(dst r3, c r4, n r5) -> dst (via __fill_mem) */
void sdk_memset(void) {
    u32 d = R[3], n = R[5];
    u8 c = (u8)R[4];
    if (!n) return;
    if (ram_range(d, n)) { recomp_mark_ram(d, n); memset(ram + (d & RAM_MASK), c, n); return; }
    for (u32 i = 0; i < n; i++) wr8(d + i, c);
}

/* 0x80169168 sqrt(f1) -> f1, correctly rounded like the guest's software
 * sqrt. Guest special cases: +-0 -> x; x < 0 -> errno = EDOM (33) and the
 * NaN constant at 0x8044eea8; inf/NaN -> x*x+x and errno = EDOM too. */
#define GUEST_ERRNO 0x8044fce8u
#define GUEST_SQRT_NAN 0x8044eea8u
void sdk_sqrt(void) {
    double x = F[1].ps0;
    u64 b = d2u(x);
    if ((b & 0x7fffffffffffffffull) == 0) return;
    if ((b >> 52 & 0x7ff) == 0x7ff) { wr32(GUEST_ERRNO, 33); F[1].ps0 = x * x + x; return; }
    if (b >> 63) { wr32(GUEST_ERRNO, 33); F[1].ps0 = u2d((u64)rd32(GUEST_SQRT_NAN) << 32 | rd32(GUEST_SQRT_NAN + 4)); return; }
    F[1].ps0 = __builtin_sqrt(x);
}
