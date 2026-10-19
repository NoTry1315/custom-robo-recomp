/* MTX/VEC bodies for runtime/mtx.c, included once per rounding mode: the caller defines
 * MX(name) (function name), ADD SUB MUL MADD MSUB (single-precision ops with ppc.h's
 * SADD.. signatures: a, c[, b]). Same operation order and operands as the guest's
 * paired-single code (gen/ f_801394c4..f_8013a284); a pair is written as two scalars.
 * Matrices are 3x4 floats, row-major: m[i][j] at m + 16*i + 4*j. */
#define NNEG(x) fnoneg(x)

/* 0x80139524 PSMTXConcat(a r3, b r4, ab r5): ab = a * b. All loads before the first store. */
static void MX(concat)(u32 a, u32 b, u32 ab) {
    double A[12], B[12], k0 = L(MTX_K01), k1 = L(MTX_K01 + 4);
    for (int i = 0; i < 12; i++) { A[i] = L(a + 4 * i); B[i] = L(b + 4 * i); }
    for (int i = 0; i < 3; i++) {
        const double *r = A + 4 * i;
        double o0 = MADD(B[8], r[2], MADD(B[4], r[1], MUL(B[0], r[0])));
        double o1 = MADD(B[9], r[2], MADD(B[5], r[1], MUL(B[1], r[0])));
        double o2 = MADD(k0, r[3], MADD(B[10], r[2], MADD(B[6], r[1], MUL(B[2], r[0]))));
        double o3 = MADD(k1, r[3], MADD(B[11], r[2], MADD(B[7], r[1], MUL(B[3], r[0]))));
        S(ab + 16 * i, o0); S(ab + 16 * i + 4, o1); S(ab + 16 * i + 8, o2); S(ab + 16 * i + 12, o3);
    }
}

/* 0x80139eb4 PSMTXMultVec(m r3, src r4, dst r5). Row 2 is loaded after dst[0] is stored. */
static void MX(multvec)(u32 m, u32 src, u32 dst) {
    double v0 = L(src), v1 = L(src + 4), v2 = L(src + 8), r[8];
    for (int i = 0; i < 8; i++) r[i] = L(m + 4 * i);
    double s0 = ADD(MADD(r[2], v2, MUL(r[0], v0)), MADD(r[3], 1.0, MUL(r[1], v1)));
    double s1 = ADD(MADD(r[6], v2, MUL(r[4], v0)), MADD(r[7], 1.0, MUL(r[5], v1)));
    S(dst, s0);
    double r8 = L(m + 32), r9 = L(m + 36), r10 = L(m + 40), r11 = L(m + 44);
    double s2 = ADD(MADD(r10, v2, MUL(r8, v0)), MADD(r11, 1.0, MUL(r9, v1)));
    S(dst + 4, s1); S(dst + 8, s2);
}

/* 0x80139f08 PSMTXMultVecSR(m r3, src r4, dst r5): 3x3 part only */
static void MX(multvecsr)(u32 m, u32 src, u32 dst) {
    double v0 = L(src), v1 = L(src + 4), v2 = L(src + 8), o[3];
    for (int i = 0; i < 3; i++) {
        u32 r = m + 16 * i;
        o[i] = MADD(L(r + 8), v2, ADD(MUL(L(r), v0), MUL(L(r + 4), v1)));
    }
    S(dst, o[0]); S(dst + 4, o[1]); S(dst + 8, o[2]);
}

/* cofactors shared by PSMTXInverse / PSMTXInvXpose (identical guest sequence up to det) */
#define MTX_COFACTORS(m)                                                                    \
    double m00 = L(m), m01 = L(m + 4), m02 = L(m + 8), m10 = L(m + 16), m11 = L(m + 20),     \
           m12 = L(m + 24), m20 = L(m + 32), m21 = L(m + 36), m22 = L(m + 40);               \
    double c11a = MSUB(m01, m12, MUL(m11, m02)), c11b = MSUB(m02, m10, MUL(m12, m00));       \
    double c13a = MSUB(m11, m22, MUL(m21, m12)), c13b = MSUB(m12, m20, MUL(m22, m10));       \
    double c12a = MSUB(m21, m02, MUL(m01, m22)), c12b = MSUB(m22, m00, MUL(m02, m20));       \
    double c10 = MSUB(m10, m21, MUL(m11, m20)), c9 = MSUB(m01, m20, MUL(m00, m21));          \
    double c8 = MSUB(m00, m11, MUL(m01, m10));                                                \
    double det = MADD(m20, c11a, MADD(m10, c12a, MUL(m00, c13a)));

/* 1/det: fres estimate + one Newton step, -(det*r*r - 2r) */
#define MTX_INVDET(det) ({ double r_ = fsgl(ppc_fres(det)); NNEG(MSUB(det, MUL(r_, r_), ADD(r_, r_))); })

/* 0x80139738 PSMTXInvXpose(src r3, dst r4) -> r3 0 if det == 0 (dst untouched), else 1 */
static u32 MX(invxpose)(u32 m, u32 d) {
    MTX_COFACTORS(m)
    double zero = SUB(m01, m01);
    if (det == zero) return 0;
    double inv = MTX_INVDET(det);
    S(d + 12, zero); S(d + 28, zero); S(d + 44, zero);
    S(d, MUL(c13a, inv)); S(d + 4, MUL(c13b, inv));
    S(d + 16, MUL(c12a, inv)); S(d + 20, MUL(c12b, inv));
    S(d + 32, MUL(c11a, inv)); S(d + 36, MUL(c11b, inv));
    S(d + 8, MUL(c10, inv)); S(d + 24, MUL(c9, inv)); S(d + 40, MUL(c8, inv));
    return 1;
}

/* 0x80139640 PSMTXInverse(src r3, dst r4) -> r3 0 if det == 0 (dst untouched), else 1 */
static u32 MX(inverse)(u32 m, u32 d) {
    MTX_COFACTORS(m)
    double zero = SUB(m02, m02);
    if (det == zero) return 0;
    double inv = MTX_INVDET(det);
    double m03 = L(m + 12), m13 = L(m + 28), m23 = L(m + 44);
    double a0 = MUL(c13a, inv), a1 = MUL(c13b, inv), b0 = MUL(c12a, inv), b1 = MUL(c12b, inv);
    double e0 = MUL(c11a, inv), e1 = MUL(c11b, inv);
    double g10 = MUL(c10, inv), g9 = MUL(c9, inv), g8 = MUL(c8, inv);
    double t0 = NNEG(MADD(e0, m23, MADD(b0, m13, MUL(a0, m03))));
    double t1 = NNEG(MADD(e1, m23, MADD(b1, m13, MUL(a1, m03))));
    double t2 = NNEG(MADD(g8, m23, MADD(g9, m13, MUL(g10, m03))));
    S(d, a0); S(d + 4, b0); S(d + 16, a1); S(d + 20, b1);
    S(d + 32, g10); S(d + 36, g9); S(d + 40, g8);
    S(d + 8, e0); S(d + 12, t0); S(d + 24, e1); S(d + 28, t1); S(d + 44, t2);
    return 1;
}

/* 0x8013a1c4 PSVECNormalize(src r3, dst r4): frsqrte + one Newton step (0.5, 3.0 from .sdata2) */
static void MX(vecnormalize)(u32 s, u32 d) {
    double half = L(MTX_KHALF), three = L(MTX_KHALF + 4);
    double x = L(s), y = L(s + 4), z = L(s + 8);
    double xx = MUL(x, x), yy = MUL(y, y);
    double m2 = ADD(MADD(z, z, xx), yy);
    double rs = ppc_frsqrte(m2);
    double k = MUL(NNEG(MSUB(MUL(rs, rs), m2, three)), MUL(rs, half));
    S(d, MUL(x, k)); S(d + 4, MUL(y, k)); S(d + 8, MUL(z, k));
}

/* 0x8013a208 PSVECSquareMag(v r3) -> f1 (ps1 = y*y as the guest leaves it) */
static void MX(vecsquaremag)(u32 v) {
    double x = L(v), y = L(v + 4), z = L(v + 8), yy = MUL(y, y);
    F[1].ps0 = ADD(MADD(z, z, MUL(x, x)), yy); F[1].ps1 = fsgl(yy);
}

/* 0x8013a220 PSVECMag(v r3) -> f1 */
static void MX(vecmag)(u32 v) {
    double half = L(MTX_KHALF), x = L(v), y = L(v + 4), z = L(v + 8), yy = MUL(y, y);
    double zero = SUB(half, half);
    double m2 = ADD(MADD(z, z, MUL(x, x)), yy);
    if (m2 == zero) { F[1].ps0 = m2; F[1].ps1 = fsgl(yy); return; }
    double rs = ppc_frsqrte(m2), three = L(MTX_KHALF + 4);
    double r = MUL(m2, MUL(NNEG(MSUB(MUL(rs, rs), m2, three)), MUL(rs, half)));
    F[1].ps0 = r; F[1].ps1 = r;
}

/* 0x8013a264 PSVECDotProduct(a r3, b r4) -> f1 = (a0*b0 + a1*b1) + a2*b2 */
static void MX(vecdot)(u32 a, u32 b) {
    double a0 = L(a), a1 = L(a + 4), a2 = L(a + 8), b0 = L(b), b1 = L(b + 4), b2 = L(b + 8);
    double p1 = MUL(a1, b1), p2 = MUL(a2, b2);
    F[1].ps0 = ADD(MADD(a0, b0, p1), p2); F[1].ps1 = fsgl(p2);
}

/* 0x8013a284 PSVECCrossProduct(a r3, b r4, axb r5) */
static void MX(veccross)(u32 a, u32 b, u32 d) {
    double b0 = L(b), b1 = L(b + 4), a2 = L(a + 8), a0 = L(a), a1 = L(a + 4), b2 = L(b + 8);
    double x = MSUB(a1, b2, MUL(b1, a2));      /* fp5.ps1 */
    double y = MSUB(a0, b2, MUL(b0, a2));      /* fp5.ps0, stored negated */
    double z = MSUB(a1, b0, MUL(b1, a0));      /* fp8.ps1, stored negated */
    S(d, x); S(d + 4, -y); S(d + 8, -z);
}

/* 0x8013a1a8 PSVECScale(src r3, dst r4, s f1) */
static void MX(vecscale)(u32 s, u32 d, double k) {
    double x = L(s), y = L(s + 4), z = L(s + 8);
    S(d, MUL(x, k)); S(d + 4, MUL(y, k)); S(d + 8, MUL(z, k));
}

/* 0x8013a160 PSVECAdd / 0x8013a184 PSVECSubtract(a r3, b r4, dst r5) */
static void MX(vecadd)(u32 a, u32 b, u32 d) {
    double x = ADD(L(a), L(b)), y = ADD(L(a + 4), L(b + 4));
    S(d, x); S(d + 4, y);
    S(d + 8, ADD(L(a + 8), L(b + 8)));
}
static void MX(vecsub)(u32 a, u32 b, u32 d) {
    double x = SUB(L(a), L(b)), y = SUB(L(a + 4), L(b + 4));
    S(d, x); S(d + 4, y);
    S(d + 8, SUB(L(a + 8), L(b + 8)));
}
#undef NNEG
