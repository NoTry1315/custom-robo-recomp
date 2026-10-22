/* MTX library (mtx.c, mtxvec.c, vec.c; 0x801394c4-0x8013a2c0) replaced natively.
 * recomp.py MTX_NATIVE sends the game's calls to these sdk_ functions; the translated
 * bodies (f_8013xxxx) are still built: sdmc:/cr_mtx_off.txt routes every call back to
 * them, and the weak gen/ stubs call them for anything not overridden here.
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
static void d_concat(u32 a, u32 b, u32 ab) {
    if (flat(a, 48) && flat(b, 48) && flat(ab, 48)) fp_concat(FL(a), FL(b), FL(ab)); else nat_concat(a, b, ab);
}
static void d_multvec(u32 m, u32 s, u32 d) {
    if (flat(m, 48) && flat(s, 12) && flat(d, 12)) fp_multvec(FL(m), FL(s), FL(d)); else nat_multvec(m, s, d);
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

static int mtx_off;

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
#define WRAP(name, fn, body) void sdk_##name(void) { NATIVE_PROF(#name); if (mtx_off) { fn(); return; } body; }
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
    mtx_off = exists("sdmc:/cr_mtx_off.txt");
#ifdef RECOMP_FLOATOPS
    const char *fp = "float (FLOATOPS)";
#else
    const char *fp = "double + fsgl/f25";
#endif
    recomp_log("# mtx: %s (sdmc:/cr_mtx_off.txt), single ops: %s\n", mtx_off ? "translated" : "native", fp);
    if (exists("sdmc:/cr_bench.txt")) bench();
}
