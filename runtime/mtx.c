/* MTX library (mtx.c, mtxvec.c, vec.c; 0x801394c4-0x8013a2c0) replaced natively.
 * recomp.py MTX_NATIVE sends the game's calls to these sdk_ functions; the translated
 * bodies (f_8013xxxx) are still built: the bench compares against them, and the weak gen/
 * stubs call them for anything not overridden here.
 * Arithmetic: ppc.h SADD/SMUL/SMADD.. in the guest's paired-single order (mtx_impl.h),
 * so results are bit-identical to the translated code in each FLOATOPS mode.
 * Assumes GQR0 = 0 (plain floats), as the SDK keeps it. Copies move raw words.
 * sdmc:/cr_bench.txt: startup "# bench" lines (ulp checks on random inputs, timings). */
#include <3ds.h>
#include <stdio.h>
#include "ppc.h"

#define MTX_K01   0x8044ed88u   /* {0.0f, 1.0f} pair read by PSMTXConcat (r6) */
#define MTX_KHALF 0x80451ce8u   /* {0.5f, 3.0f} at r2-25784 (PSVECNormalize, PSVECMag) */
#define MTX_K1    0x80451cb0u   /* 1.0f at r2-25840 (PSMTXIdentity/Trans) */
#define MTX_K0    0x80451cb4u   /* 0.0f at r2-25836 (PSMTXIdentity/Trans/Scale/Transpose) */

static inline double L(u32 a) { return u2f(rd32_ram(a)); }
static inline float LF(u32 a) { return u2f(rd32_ram(a)); }
static inline void wrm(u32 a, u32 v) { WR_MARK(a); wr32_ram(a, v); }   /* bench fills */
#define S(a, v) wr32_ram((a), f2u((float)(v)))   /* no write mark: see MARK */
/* write marks of the n-byte destination at a (n <= 1 KB, one page): its first and last word
 * cover the pages of every word in it, the same marks as one WR_MARK per store */
#define MARK(a, n) (WR_MARK(a), WR_MARK((a) + (n) - 4))

/* selected mode: exactly the translated code's ops. With FLOATOPS every op is a float op
 * whose double result is exact, so values stay float (no double round trips): add/sub of
 * floats in double then rounded to float equals the float op (53 >= 2 * 24 + 2 bits). */
#define MX(n) nat_##n
#ifdef RECOMP_FLOATOPS
static inline float F_ADD(float a, float b) { return a + b; }
static inline float F_SUB(float a, float b) { return a - b; }
static inline float F_MUL(float a, float c) { return a * c; }
static inline float F_MADD(float a, float c, float b) { return a * c + b; }
static inline float F_MSUB(float a, float c, float b) { return a * c - b; }
#define MT float
#define LD LF
#define LDK LF
#define ADD F_ADD
#define SUB F_SUB
#define MUL F_MUL
#define MADD F_MADD
#define MSUB F_MSUB
#else
#define MT double
#define LD L
#define LDK L
#define ADD SADD
#define SUB SSUB
#define MUL SMUL
#define MADD SMADD
#define MSUB SMSUB
#endif
#include "mtx_impl.h"
#undef MX
#undef LD
#undef S

/* flat variant fp_: the same bodies on addresses already masked into ram[] (constants
 * excepted, LDK) whose whole range lies inside it, so no mask per word. The wrappers below
 * pick it when every range is flat (always, in practice: the game's matrices live in the
 * 24 MB of real RAM), else nat_ (a range wrapping the end of the mirror). */
#define MX(n) fp_##n
#define LD(x) LF_FLAT(x)
#define S(x, v) ({ u32 v_ = __builtin_bswap32(f2u((float)(v))); memcpy(ram + (x), &v_, 4); })
static inline MT LF_FLAT(u32 x) { u32 v; memcpy(&v, ram + x, 4); return u2f(__builtin_bswap32(v)); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "mtx_impl.h"
#pragma GCC diagnostic pop
#undef MX
#undef LD
#undef S
#define S(a, v) wr32_ram((a), f2u((float)(v)))
#undef MT
#undef LDK
#undef ADD
#undef SUB
#undef MUL
#undef MADD
#undef MSUB

/* guest range [a, a+n) as one block of ram[] (no wrap at the end of the mirror) */
static inline int flat(u32 a, u32 n) { return (a & RAM_MASK) <= RAM_MASK + 1 - n; }
#define FL(a) ((a) & RAM_MASK)
#ifdef RECOMP_FLOATOPS
/* word-aligned flat variants wp_: hand-scheduled ARM (fp_ is ~186 instructions for a concat: one
 * ldr -> rev -> vmov chain per word, and copies for the destructive vmla). Here ldm / stm move 8 words
 * at a time, vmov moves two floats at a time, and the rows' independent chains are interleaved. The same
 * float operations as mtx_impl.h / fp_ (vmul, then vmla per MADD, vadd per ADD, operands only swapped
 * where the operation is commutative: FPSCR has default NaN on, so the bits are the same), so the same
 * results. All loads happen before the first store (ab may be a or b); multvec writes dst[0] before it
 * reads row 2, as the guest code does. Needs 4-byte aligned matrices (ldm/stm), else the fp_ body. */
void wa_concat(const u32 *a, const u32 *b, u32 *ab, const u32 *k);
void wa_multvec(const u32 *m, const u32 *s, u32 *d);
__asm__(
    ".text\n.arm\n.align 2\n.type wa_concat, %function\nwa_concat:\n"
    "push {r4-r11}\n"
    "vpush {d8-d15}\n"
    "ldm r1, {r4-r11}\n"                                   /* B row 0, 1 */
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\nrev r8, r8\nrev r9, r9\nrev r10, r10\nrev r11, r11\n"
    "vmov s0, s1, r4, r5\nvmov s2, s3, r6, r7\nvmov s4, s5, r8, r9\nvmov s6, s7, r10, r11\n"
    "add ip, r1, #32\nldm ip, {r4-r7}\nldm r3, {r8, r9}\n"  /* B row 2, k0 k1 */
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\nrev r8, r8\nrev r9, r9\n"
    "vmov s8, s9, r4, r5\nvmov s10, s11, r6, r7\nvmov s12, s13, r8, r9\n"
    "ldm r0, {r4-r11}\n"                                   /* A row 0 -> s14-s17, row 1 -> s22-s25 */
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\nrev r8, r8\nrev r9, r9\nrev r10, r10\nrev r11, r11\n"
    "vmov s14, s15, r4, r5\nvmov s16, s17, r6, r7\nvmov s22, s23, r8, r9\nvmov s24, s25, r10, r11\n"
    "add ip, r0, #32\nldm ip, {r4-r7}\n"                   /* A row 2 (before any store) */
    /* rows 0 (s18-s21) and 1 (s26-s29): B[j]*r0, + B[4+j]*r1, + B[8+j]*r2, (+ k*r3 for j = 2, 3) */
    "vmul.f32 s18, s0, s14\nvmul.f32 s19, s1, s14\nvmul.f32 s20, s2, s14\nvmul.f32 s21, s3, s14\n"
    "vmul.f32 s26, s0, s22\nvmul.f32 s27, s1, s22\nvmul.f32 s28, s2, s22\nvmul.f32 s29, s3, s22\n"
    "vmla.f32 s18, s4, s15\nvmla.f32 s19, s5, s15\nvmla.f32 s20, s6, s15\nvmla.f32 s21, s7, s15\n"
    "vmla.f32 s26, s4, s23\nvmla.f32 s27, s5, s23\nvmla.f32 s28, s6, s23\nvmla.f32 s29, s7, s23\n"
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\n"
    "vmla.f32 s18, s8, s16\nvmla.f32 s19, s9, s16\nvmla.f32 s20, s10, s16\nvmla.f32 s21, s11, s16\n"
    "vmla.f32 s26, s8, s24\nvmla.f32 s27, s9, s24\nvmla.f32 s28, s10, s24\nvmla.f32 s29, s11, s24\n"
    "vmla.f32 s20, s12, s17\nvmla.f32 s21, s13, s17\nvmla.f32 s28, s12, s25\nvmla.f32 s29, s13, s25\n"
    /* row 2: A in s14-s17, outputs s30 s31 s22 s23 */
    "vmov s14, s15, r4, r5\nvmov s16, s17, r6, r7\n"
    "vmul.f32 s30, s0, s14\nvmul.f32 s31, s1, s14\nvmul.f32 s22, s2, s14\nvmul.f32 s23, s3, s14\n"
    "vmla.f32 s30, s4, s15\nvmla.f32 s31, s5, s15\nvmla.f32 s22, s6, s15\nvmla.f32 s23, s7, s15\n"
    "vmov r4, r5, s18, s19\nvmov r6, r7, s20, s21\n"
    "vmla.f32 s30, s8, s16\nvmla.f32 s31, s9, s16\nvmla.f32 s22, s10, s16\nvmla.f32 s23, s11, s16\n"
    "vmov r8, r9, s26, s27\nvmov r10, r11, s28, s29\n"
    "vmla.f32 s22, s12, s17\nvmla.f32 s23, s13, s17\n"
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\nrev r8, r8\nrev r9, r9\nrev r10, r10\nrev r11, r11\n"
    "stm r2, {r4-r11}\n"
    "vmov r4, r5, s30, s31\nvmov r6, r7, s22, s23\n"
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\n"
    "add ip, r2, #32\nstm ip, {r4-r7}\n"
    "vpop {d8-d15}\n"
    "pop {r4-r11}\n"
    "bx lr\n"
    ".size wa_concat, .-wa_concat\n"
    /* multvec: s = (r0*v0 + r2*v2) + (r3 + r1*v1) per row (mtx_impl.h; GCC folds r3 * 1.0f to r3) */
    ".align 2\n.type wa_multvec, %function\nwa_multvec:\n"
    "push {r4-r11, lr}\n"
    "ldm r1, {r4-r6}\n"
    "ldm r0, {r3, r7-r11, ip, lr}\n"                       /* m0..m7 */
    "rev r4, r4\nrev r5, r5\nrev r6, r6\n"
    "rev r3, r3\nrev r7, r7\nrev r8, r8\nrev r9, r9\nrev r10, r10\nrev r11, r11\nrev ip, ip\nrev lr, lr\n"
    "vmov s0, s1, r4, r5\nvmov s2, r6\n"                    /* v0 v1 v2 */
    "vmov s4, s5, r3, r7\nvmov s6, s7, r8, r9\nvmov s8, s9, r10, r11\nvmov s10, s11, ip, lr\n"
    "vmul.f32 s12, s4, s0\nvmul.f32 s13, s8, s0\n"
    "vmla.f32 s7, s5, s1\nvmla.f32 s11, s9, s1\n"
    "vmla.f32 s12, s6, s2\nvmla.f32 s13, s10, s2\n"
    "vadd.f32 s12, s12, s7\nvadd.f32 s13, s13, s11\n"
    "vmov r3, s12\nrev r3, r3\nstr r3, [r2]\n"              /* dst[0] before row 2 */
    "add ip, r0, #32\nldm ip, {r4-r7}\n"
    "rev r4, r4\nrev r5, r5\nrev r6, r6\nrev r7, r7\n"
    "vmov s4, s5, r4, r5\nvmov s6, s7, r6, r7\n"
    "vmul.f32 s14, s4, s0\nvmla.f32 s7, s5, s1\nvmla.f32 s14, s6, s2\nvadd.f32 s14, s14, s7\n"
    "vmov r4, r5, s13, s14\nrev r4, r4\nrev r5, r5\nstr r4, [r2, #4]\nstr r5, [r2, #8]\n"
    "pop {r4-r11, pc}\n"
    ".size wa_multvec, .-wa_multvec\n");
#define WP(x) ((u32 *)(ram + (x)))
static void wp_concat(u32 a, u32 b, u32 ab) { wa_concat(WP(a), WP(b), WP(ab), WP(MTX_K01 & RAM_MASK)); MARK(ab, 48); }
static void wp_multvec(u32 m, u32 s, u32 d) { wa_multvec(WP(m), WP(s), WP(d)); MARK(d, 12); }
#define WPOK(x) (!((x) & 3))   /* else the fp_ body */
#else
#define WPOK(x) 0
#define wp_concat fp_concat
#define wp_multvec fp_multvec
#endif
static void d_concat(u32 a, u32 b, u32 ab) {
    if (flat(a, 48) && flat(b, 48) && flat(ab, 48)) {
        if (WPOK(a | b | ab)) wp_concat(FL(a), FL(b), FL(ab)); else fp_concat(FL(a), FL(b), FL(ab));
    } else nat_concat(a, b, ab);
}
static void d_multvec(u32 m, u32 s, u32 d) {
    if (flat(m, 48) && flat(s, 12) && flat(d, 12)) {
        if (WPOK(m | s | d)) wp_multvec(FL(m), FL(s), FL(d)); else fp_multvec(FL(m), FL(s), FL(d));
    } else nat_multvec(m, s, d);
}
static void d_multvecsr(u32 m, u32 s, u32 d) {
    if (flat(m, 48) && flat(s, 12) && flat(d, 12)) fp_multvecsr(FL(m), FL(s), FL(d)); else nat_multvecsr(m, s, d);
}
static u32 d_invxpose(u32 m, u32 d) { return flat(m, 48) && flat(d, 48) ? fp_invxpose(FL(m), FL(d)) : nat_invxpose(m, d); }
static u32 d_inverse(u32 m, u32 d) { return flat(m, 48) && flat(d, 48) ? fp_inverse(FL(m), FL(d)) : nat_inverse(m, d); }
#ifdef RECOMP_FLOATOPS
/* reference for the bench: default (double + fsgl/f25) rounding */
static inline double E_ADD(double a, double b) { return fsgl(a + b); }
static inline double E_SUB(double a, double b) { return fsgl(a - b); }
static inline double E_MUL(double a, double c) { return fsgl(a * f25(c)); }
static inline double E_MADD(double a, double c, double b) { return fsgl(fmadd_s(a, f25(c), b)); }
static inline double E_MSUB(double a, double c, double b) { return fsgl(fmadd_s(a, f25(c), -b)); }
#define MX(n) ex_##n
#define MT double
#define LD L
#define LDK L
#define ADD E_ADD
#define SUB E_SUB
#define MUL E_MUL
#define MADD E_MADD
#define MSUB E_MSUB
#include "mtx_impl.h"
#undef MX
#undef ADD
#undef SUB
#undef MUL
#undef MADD
#undef MSUB
#undef MT
#undef LD
#undef LDK
#endif


void f_801394c4(void); void f_801394f0(void); void f_80139524(void); void f_801395f0(void);
void f_80139640(void); void f_80139738(void); void f_80139a40(void); void f_80139a74(void);
void f_80139eb4(void); void f_80139f08(void); void f_8013a160(void); void f_8013a184(void);
void f_8013a1a8(void); void f_8013a1c4(void); void f_8013a208(void); void f_8013a220(void);
void f_8013a264(void); void f_8013a284(void);
void f_8001e134(void);  /* gmtx_mul (game, scalar fmuls/fmadds): bench only */

/* no arithmetic: stores of .sdata2 constants and arguments */
static void nat_identity(u32 m) {
    u32 z = rd32_ram(MTX_K0), o = rd32_ram(MTX_K1);
    for (int i = 0; i < 12; i++) wr32_ram(m + 4 * i, (i % 5) ? z : o);
    MARK(m, 48);
}
static void nat_copy(u32 s, u32 d) {
    if (s == d) return;
    u32 t[12];
    for (int i = 0; i < 12; i++) t[i] = rd32_ram(s + 4 * i);
    for (int i = 0; i < 12; i++) wr32_ram(d + 4 * i, t[i]);
    MARK(d, 48);
}
static void nat_transpose(u32 s, u32 d) {
    u32 t[9], z = rd32_ram(MTX_K0);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[3 * i + j] = rd32_ram(s + 16 * j + 4 * i);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) wr32_ram(d + 16 * i + 4 * j, t[3 * i + j]);
        wr32_ram(d + 16 * i + 12, z);
    }
    MARK(d, 48);
}
static void nat_trans(u32 m, double x, double y, double z) {
    u32 k0 = rd32_ram(MTX_K0), k1 = rd32_ram(MTX_K1);
    for (int i = 0; i < 12; i++) wr32_ram(m + 4 * i, (i % 5) ? k0 : k1);
    S(m + 12, x); S(m + 28, y); S(m + 44, z);
    MARK(m, 48);
}
static void nat_scale(u32 m, double x, double y, double z) {
    u32 k0 = rd32_ram(MTX_K0);
    for (int i = 0; i < 12; i++) wr32_ram(m + 4 * i, k0);
    S(m, x); S(m + 20, y); S(m + 40, z);
    MARK(m, 48);
}

#ifdef RECOMP_FNPROF
void recomp_fnprof_native(const char *name);
#define NATIVE_PROF(name) recomp_fnprof_native(name)
#else
#define NATIVE_PROF(name) ((void)0)
#endif
#define WRAP(name, fn, body) void sdk_##name(void) { NATIVE_PROF(#name); (void)fn; body; }
WRAP(PSMTXIdentity, f_801394c4, nat_identity(R[3]))
WRAP(PSMTXCopy, f_801394f0, nat_copy(R[3], R[4]))
WRAP(PSMTXConcat, f_80139524, d_concat(R[3], R[4], R[5]); R[6] = MTX_K01)  /* r6 as the guest leaves it (SDK logs) */
WRAP(PSMTXTranspose, f_801395f0, nat_transpose(R[3], R[4]))
WRAP(PSMTXInverse, f_80139640, R[3] = d_inverse(R[3], R[4]))
WRAP(PSMTXInvXpose, f_80139738, R[3] = d_invxpose(R[3], R[4]))
WRAP(PSMTXTrans, f_80139a40, nat_trans(R[3], F[1].ps0, F[2].ps0, F[3].ps0))
WRAP(PSMTXScale, f_80139a74, nat_scale(R[3], F[1].ps0, F[2].ps0, F[3].ps0))
WRAP(PSMTXMultVec, f_80139eb4, d_multvec(R[3], R[4], R[5]))
WRAP(PSMTXMultVecSR, f_80139f08, d_multvecsr(R[3], R[4], R[5]))
WRAP(PSVECAdd, f_8013a160, nat_vecadd(R[3], R[4], R[5]))
WRAP(PSVECSubtract, f_8013a184, nat_vecsub(R[3], R[4], R[5]))
WRAP(PSVECScale, f_8013a1a8, nat_vecscale(R[3], R[4], F[1].ps0))
WRAP(PSVECNormalize, f_8013a1c4, nat_vecnormalize(R[3], R[4]))
WRAP(PSVECSquareMag, f_8013a208, nat_vecsquaremag(R[3]))
WRAP(PSVECMag, f_8013a220, nat_vecmag(R[3]))
WRAP(PSVECDotProduct, f_8013a264, nat_vecdot(R[3], R[4]))
WRAP(PSVECCrossProduct, f_8013a284, nat_veccross(R[3], R[4], R[5]))

/* ---------------------------------------------------------------- bench */
#define B_BASE 0x81700000u   /* scratch, saved and restored around the bench */
#define B_SIZE 0x10000u
#define B_A (B_BASE)
#define B_B (B_BASE + 0x40)
#define B_O1 (B_BASE + 0x80)
#define B_O2 (B_BASE + 0xc0)
#define G_MTX 0x8031fe68u    /* gmtx_mul's G (lis -0x7fce; addi -0x198) */

static u32 rng = 12345;
static float rnd(void) { rng = rng * 1664525u + 1013904223u; return ((s32)rng >> 8) * (4.0f / 8388608.0f); }
static u32 ulp(u32 a, u32 b) {
    if ((a & 0x7fffffffu) > 0x7f800000u && (b & 0x7fffffffu) > 0x7f800000u) return 0;
    s32 x = (a >> 31) ? (s32)(0x80000000u - a) : (s32)a, y = (b >> 31) ? (s32)(0x80000000u - b) : (s32)b;
    s64 d = (s64)x - y; return (u32)(d < 0 ? -d : d);
}
static u32 cmp_out(u32 n) { u32 m = 0; for (u32 i = 0; i < n; i++) { u32 u = ulp(rd32_ram(B_O1 + 4 * i), rd32_ram(B_O2 + 4 * i)); if (u > m) m = u; } return m; }
static void fill(u32 a, int n) { for (int i = 0; i < n; i++) wrm(a + 4 * i, f2u(rnd())); }
static void clear_out(void) { for (int i = 0; i < 16; i++) { wrm(B_O1 + 4 * i, 0); wrm(B_O2 + 4 * i, 0); } }

enum { T_CONCAT, T_MULTVEC, T_MULTVECSR, T_INVXPOSE, T_INVERSE, T_NORM, T_DOT, T_CROSS, T_MAG, T_SQMAG,
       T_ADD, T_SUB, T_SCALE, T_IDENT, T_COPY, T_TRANSPOSE, T_TRANS, T_MSCALE, T_N };
static const char *t_name[T_N] = { "PSMTXConcat", "PSMTXMultVec", "PSMTXMultVecSR", "PSMTXInvXpose",
    "PSMTXInverse", "PSVECNormalize", "PSVECDotProduct", "PSVECCrossProduct", "PSVECMag", "PSVECSquareMag",
    "PSVECAdd", "PSVECSubtract", "PSVECScale", "PSMTXIdentity", "PSMTXCopy", "PSMTXTranspose",
    "PSMTXTrans", "PSMTXScale" };

/* mode 0: translated f_ -> B_O1; 1: native -> B_O2; 2: exact native (FLOATOPS) -> B_O2.
 * Returns the f1 / r3 result bits (0 if none). */
static u32 run(int t, int mode) {
    u32 o = mode ? B_O2 : B_O1;
    double s1 = F[1].ps0, s2 = F[2].ps0, s3 = F[3].ps0;
    if (mode == 0) {
        static void (*const f[T_N])(void) = { f_80139524, f_80139eb4, f_80139f08, f_80139738, f_80139640,
            f_8013a1c4, f_8013a264, f_8013a284, f_8013a220, f_8013a208, f_8013a160, f_8013a184, f_8013a1a8,
            f_801394c4, f_801394f0, f_801395f0, f_80139a40, f_80139a74 };
        R[1] = B_BASE + 0xf000; R[3] = B_A; R[4] = B_B; R[5] = o;
        if (t == T_MULTVEC || t == T_MULTVECSR) { R[4] = B_B; }
        if (t == T_INVXPOSE || t == T_INVERSE || t == T_NORM || t == T_SCALE || t == T_COPY || t == T_TRANSPOSE) R[4] = o;
        if (t == T_IDENT || t == T_TRANS || t == T_MSCALE) R[3] = o;
        f[t]();
        F[2].ps0 = s2; F[3].ps0 = s3;
        u32 r = (t == T_INVXPOSE || t == T_INVERSE) ? R[3] : (t >= T_DOT && t <= T_SQMAG && t != T_CROSS) ? f2u((float)F[1].ps0) : 0;
        F[1].ps0 = s1;
        return r;
    }
#ifdef RECOMP_FLOATOPS
#define CALL(n, ...) (mode == 2 ? ex_##n(__VA_ARGS__) : nat_##n(__VA_ARGS__))
#define CALLD(n, ...) (mode == 2 ? ex_##n(__VA_ARGS__) : d_##n(__VA_ARGS__))   /* as the game calls it */
#else
#define CALL(n, ...) nat_##n(__VA_ARGS__)
#define CALLD(n, ...) d_##n(__VA_ARGS__)
#endif
    u32 r = 0;
    switch (t) {
    case T_CONCAT: CALLD(concat, B_A, B_B, o); break;
    case T_MULTVEC: CALLD(multvec, B_A, B_B, o); break;
    case T_MULTVECSR: CALLD(multvecsr, B_A, B_B, o); break;
    case T_INVXPOSE: r = CALLD(invxpose, B_A, o); break;
    case T_INVERSE: r = CALLD(inverse, B_A, o); break;
    case T_NORM: CALL(vecnormalize, B_A, o); break;
    case T_DOT: CALL(vecdot, B_A, B_B); r = f2u((float)F[1].ps0); break;
    case T_CROSS: CALL(veccross, B_A, B_B, o); break;
    case T_MAG: CALL(vecmag, B_A); r = f2u((float)F[1].ps0); break;
    case T_SQMAG: CALL(vecsquaremag, B_A); r = f2u((float)F[1].ps0); break;
    case T_ADD: CALL(vecadd, B_A, B_B, o); break;
    case T_SUB: CALL(vecsub, B_A, B_B, o); break;
    case T_SCALE: CALL(vecscale, B_A, o, s1); break;
    case T_IDENT: nat_identity(o); break;
    case T_COPY: nat_copy(B_A, o); break;
    case T_TRANSPOSE: nat_transpose(B_A, o); break;
    case T_TRANS: nat_trans(o, s1, s2, s3); break;
    case T_MSCALE: nat_scale(o, s1, s2, s3); break;
    }
    F[1].ps0 = s1;
    return r;
}

static void bench(void) {
    static u8 save_ram[B_SIZE]; static u8 save_g[48]; static CPU save_cpu;
    memcpy(save_ram, ram + (B_BASE & RAM_MASK), B_SIZE);
    memcpy(save_g, ram + (G_MTX & RAM_MASK), 48);
    save_cpu = cpu;
    R[2] = 0x804581a0u; R[13] = 0x804543c0u; cpu.gqr[0] = 0;
    recomp_log("# bench consts k0 %08lx k1 %08lx k01 %08lx %08lx khalf %08lx %08lx\n", rd32_ram(MTX_K0), rd32_ram(MTX_K1),
               rd32_ram(MTX_K01), rd32_ram(MTX_K01 + 4), rd32_ram(MTX_KHALF), rd32_ram(MTX_KHALF + 4));
#ifdef RECOMP_FLOATOPS
    const int modes = 2;
#else
    const int modes = 1;
#endif
    for (int t = 0; t < T_N; t++) {
        u32 mx[2] = {0, 0}, bad[2] = {0, 0};
        for (int it = 0; it < 2000; it++) {
            fill(B_A, 12); fill(B_B, 12);
            F[1].ps0 = rnd(); F[2].ps0 = rnd(); F[3].ps0 = rnd();
            if ((it & 7) == 0) F[1].ps0 = F[1].ps0 * (1.0 + 1e-12);   /* a double-precision f1 */
            for (int m = 0; m < modes; m++) {
                clear_out();
                u32 r0, r1;
                if (!m) { r0 = run(t, 0); r1 = run(t, 1); }
                else {     /* float vs exact: native in both roundings */
                    r0 = run(t, 1); memcpy(ram + (B_O1 & RAM_MASK), ram + (B_O2 & RAM_MASK), 64);
                    r1 = run(t, 2);
                }
                u32 u = cmp_out(12), v = (t == T_INVXPOSE || t == T_INVERSE) ? (r0 != r1) : ulp(r0, r1);
                if (t == T_INVXPOSE || t == T_INVERSE) bad[m] += v; else if (v > u) u = v;
                if (u > mx[m]) mx[m] = u;
            }
        }
        recomp_log("# bench ulp %s native-vs-translated %lu%s", t_name[t], mx[0], bad[0] ? " (r3 differs)" : "");
        if (modes > 1) recomp_log(", float-vs-exact %lu%s", mx[1], bad[1] ? " (r3 differs)" : "");
        recomp_log("\n");
    }
    /* timings: N calls on one fixed input */
    const int N = 20000;
    fill(B_A, 12); fill(B_B, 12); F[1].ps0 = 1.5;
    static const int tt[] = { T_CONCAT, T_MULTVEC, T_INVXPOSE, T_NORM, T_DOT };
    for (unsigned k = 0; k < sizeof tt / sizeof *tt; k++) {
        u64 ts[2];
        for (int m = 0; m < 2; m++) {
            u64 t0 = svcGetSystemTick();
            for (int i = 0; i < N; i++) run(tt[k], m);
            ts[m] = svcGetSystemTick() - t0;
        }
        recomp_log("# bench time %s translated %.0f ns native %.0f ns per call\n", t_name[tt[k]],
                   ts[0] * 1e9 / SYSCLOCK_ARM11 / N, ts[1] * 1e9 / SYSCLOCK_ARM11 / N);
    }
    {   /* gmtx_mul: translated game code with scalar fmuls/fmadds (compare across builds) */
        fill(G_MTX, 12);
        u64 t0 = svcGetSystemTick();
        for (int i = 0; i < N; i++) { if ((i & 1023) == 0) fill(G_MTX, 12); R[1] = B_BASE + 0xf000; R[3] = B_A; f_8001e134(); }
        u64 dt = svcGetSystemTick() - t0;
        recomp_log("# bench time gmtx_mul(0x8001e134) translated %.0f ns per call\n", dt * 1e9 / SYSCLOCK_ARM11 / N);
    }
#ifdef RECOMP_FLOATOPS
    recomp_log("# bench mode: FLOATOPS (single ops in float)\n");
#else
    recomp_log("# bench mode: default (double + fsgl/f25)\n");
#endif
    memcpy(ram + (B_BASE & RAM_MASK), save_ram, B_SIZE);
    memcpy(ram + (G_MTX & RAM_MASK), save_g, 48);
    cpu = save_cpu;
}

static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

void recomp_mtx_init(void) {
#ifdef RECOMP_FLOATOPS
    const char *fp = "float (FLOATOPS)";
#else
    const char *fp = "double + fsgl/f25";
#endif
    recomp_log("# mtx: native, single ops: %s\n", fp);
    if (exists("sdmc:/cr_bench.txt")) bench();
}
