/* MTX/VEC bodies for runtime/mtx.c, included once per rounding mode: the caller defines
 * MX(name) (function name), MT (value type), LD(addr) / LDK(guest address of a constant)
 * (loads as MT), ADD SUB MUL MADD MSUB
 * (single-precision ops with ppc.h's SADD.. signatures: a, c[, b]). S(addr, v) stores without a
 * write mark; each function marks its destination range once (MARK). Same operation order and
 * operands as the guest's paired-single code (gen/ f_801394c4..f_8013a284); a pair is written as two scalars.
 * Matrices are 3x4 floats, row-major: m[i][j] at m + 16*i + 4*j. */
#define NNEG(x) fnoneg(x)

/* 0x80139524 PSMTXConcat(a r3, b r4, ab r5): ab = a * b. All loads before the first store. */
static void MX(concat)(u32 a, u32 b, u32 ab) {
    MT A[12], B[12], k0 = LDK(MTX_K01), k1 = LDK(MTX_K01 + 4);
#pragma GCC unroll 12
    for (int i = 0; i < 12; i++) { A[i] = LD(a + 4 * i); B[i] = LD(b + 4 * i); }   /* unrolled: A, B stay in registers */
#pragma GCC unroll 3
    for (int i = 0; i < 3; i++) {
        const MT *r = A + 4 * i;
        MT o0 = MADD(B[8], r[2], MADD(B[4], r[1], MUL(B[0], r[0])));
        MT o1 = MADD(B[9], r[2], MADD(B[5], r[1], MUL(B[1], r[0])));
        MT o2 = MADD(k0, r[3], MADD(B[10], r[2], MADD(B[6], r[1], MUL(B[2], r[0]))));
        MT o3 = MADD(k1, r[3], MADD(B[11], r[2], MADD(B[7], r[1], MUL(B[3], r[0]))));
        S(ab + 16 * i, o0); S(ab + 16 * i + 4, o1); S(ab + 16 * i + 8, o2); S(ab + 16 * i + 12, o3);
    }
    MARK(ab, 48);
}

/* 0x80139eb4 PSMTXMultVec(m r3, src r4, dst r5). Row 2 is loaded after dst[0] is stored. */
static void MX(multvec)(u32 m, u32 src, u32 dst) {
    MT v0 = LD(src), v1 = LD(src + 4), v2 = LD(src + 8), r[8];
    for (int i = 0; i < 8; i++) r[i] = LD(m + 4 * i);
    MT s0 = ADD(MADD(r[2], v2, MUL(r[0], v0)), MADD(r[3], 1.0, MUL(r[1], v1)));
    MT s1 = ADD(MADD(r[6], v2, MUL(r[4], v0)), MADD(r[7], 1.0, MUL(r[5], v1)));
    S(dst, s0);
    MT r8 = LD(m + 32), r9 = LD(m + 36), r10 = LD(m + 40), r11 = LD(m + 44);
    MT s2 = ADD(MADD(r10, v2, MUL(r8, v0)), MADD(r11, 1.0, MUL(r9, v1)));
    S(dst + 4, s1); S(dst + 8, s2);
    MARK(dst, 12);
}

/* 0x80139f08 PSMTXMultVecSR(m r3, src r4, dst r5): 3x3 part only */
static void MX(multvecsr)(u32 m, u32 src, u32 dst) {
    MT v0 = LD(src), v1 = LD(src + 4), v2 = LD(src + 8), o[3];
    for (int i = 0; i < 3; i++) {
        u32 r = m + 16 * i;
        o[i] = MADD(LD(r + 8), v2, ADD(MUL(LD(r), v0), MUL(LD(r + 4), v1)));
    }
    S(dst, o[0]); S(dst + 4, o[1]); S(dst + 8, o[2]);
    MARK(dst, 12);
}

/* cofactors shared by PSMTXInverse / PSMTXInvXpose (identical guest sequence up to det) */
#define MTX_COFACTORS(m)                                                                    \
    MT m00 = LD(m), m01 = LD(m + 4), m02 = LD(m + 8), m10 = LD(m + 16), m11 = LD(m + 20),     \
           m12 = LD(m + 24), m20 = LD(m + 32), m21 = LD(m + 36), m22 = LD(m + 40);               \
    MT c11a = MSUB(m01, m12, MUL(m11, m02)), c11b = MSUB(m02, m10, MUL(m12, m00));       \
    MT c13a = MSUB(m11, m22, MUL(m21, m12)), c13b = MSUB(m12, m20, MUL(m22, m10));       \
    MT c12a = MSUB(m21, m02, MUL(m01, m22)), c12b = MSUB(m22, m00, MUL(m02, m20));       \
    MT c10 = MSUB(m10, m21, MUL(m11, m20)), c9 = MSUB(m01, m20, MUL(m00, m21));          \
    MT c8 = MSUB(m00, m11, MUL(m01, m10));                                                \
    MT det = MADD(m20, c11a, MADD(m10, c12a, MUL(m00, c13a)));

/* 1/det: fres estimate + one Newton step, -(det*r*r - 2r) */
#define MTX_INVDET(det) ({ MT r_ = fsgl(ppc_fres(det)); NNEG(MSUB(det, MUL(r_, r_), ADD(r_, r_))); })

/* 0x80139738 PSMTXInvXpose(src r3, dst r4) -> r3 0 if det == 0 (dst untouched), else 1 */
static u32 MX(invxpose)(u32 m, u32 d) {
    MTX_COFACTORS(m)
    MT zero = SUB(m01, m01);
    if (det == zero) return 0;
    MT inv = MTX_INVDET(det);
    S(d + 12, zero); S(d + 28, zero); S(d + 44, zero);
    S(d, MUL(c13a, inv)); S(d + 4, MUL(c13b, inv));
    S(d + 16, MUL(c12a, inv)); S(d + 20, MUL(c12b, inv));
    S(d + 32, MUL(c11a, inv)); S(d + 36, MUL(c11b, inv));
    S(d + 8, MUL(c10, inv)); S(d + 24, MUL(c9, inv)); S(d + 40, MUL(c8, inv));
    MARK(d, 48);
    return 1;
}

/* 0x80139640 PSMTXInverse(src r3, dst r4) -> r3 0 if det == 0 (dst untouched), else 1 */
static u32 MX(inverse)(u32 m, u32 d) {
    MTX_COFACTORS(m)
    MT zero = SUB(m02, m02);
    if (det == zero) return 0;
    MT inv = MTX_INVDET(det);
    MT m03 = LD(m + 12), m13 = LD(m + 28), m23 = LD(m + 44);
    MT a0 = MUL(c13a, inv), a1 = MUL(c13b, inv), b0 = MUL(c12a, inv), b1 = MUL(c12b, inv);
    MT e0 = MUL(c11a, inv), e1 = MUL(c11b, inv);
    MT g10 = MUL(c10, inv), g9 = MUL(c9, inv), g8 = MUL(c8, inv);
    MT t0 = NNEG(MADD(e0, m23, MADD(b0, m13, MUL(a0, m03))));
    MT t1 = NNEG(MADD(e1, m23, MADD(b1, m13, MUL(a1, m03))));
    MT t2 = NNEG(MADD(g8, m23, MADD(g9, m13, MUL(g10, m03))));
    S(d, a0); S(d + 4, b0); S(d + 16, a1); S(d + 20, b1);
    S(d + 32, g10); S(d + 36, g9); S(d + 40, g8);
    S(d + 8, e0); S(d + 12, t0); S(d + 24, e1); S(d + 28, t1); S(d + 44, t2);
    MARK(d, 48);
    return 1;
}

/* 0x8013a1c4 PSVECNormalize(src r3, dst r4): frsqrte + one Newton step (0.5, 3.0 from .sdata2) */
static void MX(vecnormalize)(u32 s, u32 d) {
    MT half = LDK(MTX_KHALF), three = LDK(MTX_KHALF + 4);
    MT x = LD(s), y = LD(s + 4), z = LD(s + 8);
    MT xx = MUL(x, x), yy = MUL(y, y);
    MT m2 = ADD(MADD(z, z, xx), yy);
    MT rs = ppc_frsqrte(m2);
    MT k = MUL(NNEG(MSUB(MUL(rs, rs), m2, three)), MUL(rs, half));
    S(d, MUL(x, k)); S(d + 4, MUL(y, k)); S(d + 8, MUL(z, k));
    MARK(d, 12);
}

/* 0x8013a208 PSVECSquareMag(v r3) -> f1 (ps1 = y*y as the guest leaves it) */
static void MX(vecsquaremag)(u32 v) {
    MT x = LD(v), y = LD(v + 4), z = LD(v + 8), yy = MUL(y, y);
    F[1].ps0 = ADD(MADD(z, z, MUL(x, x)), yy); F[1].ps1 = fsgl(yy);
}

/* 0x8013a220 PSVECMag(v r3) -> f1 */
static void MX(vecmag)(u32 v) {
    MT half = LDK(MTX_KHALF), x = LD(v), y = LD(v + 4), z = LD(v + 8), yy = MUL(y, y);
    MT zero = SUB(half, half);
    MT m2 = ADD(MADD(z, z, MUL(x, x)), yy);
    if (m2 == zero) { F[1].ps0 = m2; F[1].ps1 = fsgl(yy); return; }
    MT rs = ppc_frsqrte(m2), three = LDK(MTX_KHALF + 4);
    MT r = MUL(m2, MUL(NNEG(MSUB(MUL(rs, rs), m2, three)), MUL(rs, half)));
    F[1].ps0 = r; F[1].ps1 = r;
}

/* 0x8013a264 PSVECDotProduct(a r3, b r4) -> f1 = (a0*b0 + a1*b1) + a2*b2 */
static void MX(vecdot)(u32 a, u32 b) {
    MT a0 = LD(a), a1 = LD(a + 4), a2 = LD(a + 8), b0 = LD(b), b1 = LD(b + 4), b2 = LD(b + 8);
    MT p1 = MUL(a1, b1), p2 = MUL(a2, b2);
    F[1].ps0 = ADD(MADD(a0, b0, p1), p2); F[1].ps1 = fsgl(p2);
}

/* 0x8013a284 PSVECCrossProduct(a r3, b r4, axb r5) */
static void MX(veccross)(u32 a, u32 b, u32 d) {
    MT b0 = LD(b), b1 = LD(b + 4), a2 = LD(a + 8), a0 = LD(a), a1 = LD(a + 4), b2 = LD(b + 8);
    MT x = MSUB(a1, b2, MUL(b1, a2));      /* fp5.ps1 */
    MT y = MSUB(a0, b2, MUL(b0, a2));      /* fp5.ps0, stored negated */
    MT z = MSUB(a1, b0, MUL(b1, a0));      /* fp8.ps1, stored negated */
    S(d, x); S(d + 4, -y); S(d + 8, -z);
    MARK(d, 12);
}

/* 0x8013a1a8 PSVECScale(src r3, dst r4, s f1) */
static void MX(vecscale)(u32 s, u32 d, double k) {
    MT x = LD(s), y = LD(s + 4), z = LD(s + 8);
    S(d, MUL(x, k)); S(d + 4, MUL(y, k)); S(d + 8, MUL(z, k));
    MARK(d, 12);
}

/* 0x8013a160 PSVECAdd / 0x8013a184 PSVECSubtract(a r3, b r4, dst r5) */
static void MX(vecadd)(u32 a, u32 b, u32 d) {
    MT x = ADD(LD(a), LD(b)), y = ADD(LD(a + 4), LD(b + 4));
    S(d, x); S(d + 4, y);
    S(d + 8, ADD(LD(a + 8), LD(b + 8)));
    MARK(d, 12);
}
static void MX(vecsub)(u32 a, u32 b, u32 d) {
    MT x = SUB(LD(a), LD(b)), y = SUB(LD(a + 4), LD(b + 4));
    S(d, x); S(d + 4, y);
    S(d + 8, SUB(LD(a + 8), LD(b + 8)));
    MARK(d, 12);
}
#undef NNEG
