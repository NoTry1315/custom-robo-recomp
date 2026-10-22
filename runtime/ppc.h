/* Gekko CPU state and helpers used by the translated code in gen/.
 * Semantics follow Dolphin's interpreter (the oracle); deviations are listed
 * in docs/recomp_unknowns.md. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

typedef struct { double ps0, ps1; } PS;

typedef struct {
    u32 gpr[32];
    PS  fpr[32];
    u32 cr;                 /* PPC order: cr0 in bits 31..28 */
    u32 lr, ctr;
    u32 xer_so, xer_ov, xer_ca, xer_bc;
    u32 fpscr;
    u32 gqr[8];
    u32 msr;
    u32 spr[1024];          /* everything not given its own field */
} CPU;

extern CPU cpu;
extern u8 ram[];            /* 0x80000000 mirror, RAM_MASK+1 bytes (+8 pad) */
#define RAM_MASK 0x01FFFFFFu
#define STACK_TOP 0x80462ac0u     /* initial r1 (crt0 table) = linker __ArenaLo */
/* the 16 KB locked cache (0xE0000000 on the GameCube) lives in RAM above the guest's 24 MB:
 * THPInit hands the THP decoders this address instead (runtime/thp.c, os.c LC*) */
#define LC_BASE 0x81E00000u

#define R cpu.gpr
#define F cpu.fpr

/* ---- memory (big-endian guest) ---- */
u32  hw_read(u32 a, int size);
void hw_write(u32 a, u32 v, int size);
#define IS_HW(a) (((a) & 0xFE000000u) == 0xCC000000u)
/* guest-write epochs per 1 KB page (wr8/16/32 and the runtime's bulk writers): the GX display-list cache
 * skips re-hashing a list/array range whose pages were not written since it was last hashed. Stores
 * through r1/r11 (wr*_ram) are not marked; the cache hashes ranges near the stack. */
#define WR_PAGE_SHIFT 10
extern u32 ram_wr[(RAM_MASK + 1) >> WR_PAGE_SHIFT];
extern u32 recomp_wr_epoch;
#define WR_MARK(a) (ram_wr[((a) & RAM_MASK) >> WR_PAGE_SHIFT] = recomp_wr_epoch)
void recomp_mark_ram(u32 a, u32 n);   /* bulk writers */

/* forced inline: in gen/'s large functions GCC stopped inlining these (37.5k out-of-line calls) */
#define RI static inline __attribute__((always_inline))
RI u8  rd8 (u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,1); return ram[a & RAM_MASK]; }
RI u16 rd16(u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,2); u16 v; memcpy(&v, ram + (a & RAM_MASK), 2); return __builtin_bswap16(v); }
RI u32 rd32(u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,4); u32 v; memcpy(&v, ram + (a & RAM_MASK), 4); return __builtin_bswap32(v); }
RI u64 rd64(u32 a){ return ((u64)rd32(a) << 32) | rd32(a + 4); }
RI void wr8 (u32 a, u8 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,1); return; } WR_MARK(a); ram[a & RAM_MASK] = v; }
RI void wr16(u32 a, u16 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,2); return; } WR_MARK(a); v = __builtin_bswap16(v); memcpy(ram + (a & RAM_MASK), &v, 2); }
RI void wr32(u32 a, u32 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,4); return; } WR_MARK(a); v = __builtin_bswap32(v); memcpy(ram + (a & RAM_MASK), &v, 4); }
RI void wr64(u32 a, u64 v){ wr32(a, (u32)(v >> 32)); wr32(a + 4, (u32)v); }

/* r1-relative (stack) accesses never reach hardware registers: no IS_HW check */
RI u8  rd8_ram (u32 a){ return ram[a & RAM_MASK]; }
RI u16 rd16_ram(u32 a){ u16 v; memcpy(&v, ram + (a & RAM_MASK), 2); return __builtin_bswap16(v); }
RI u32 rd32_ram(u32 a){ u32 v; memcpy(&v, ram + (a & RAM_MASK), 4); return __builtin_bswap32(v); }
RI u64 rd64_ram(u32 a){ return ((u64)rd32_ram(a) << 32) | rd32_ram(a + 4); }
RI void wr8_ram (u32 a, u8 v){ ram[a & RAM_MASK] = v; }
RI void wr16_ram(u32 a, u16 v){ v = __builtin_bswap16(v); memcpy(ram + (a & RAM_MASK), &v, 2); }
RI void wr32_ram(u32 a, u32 v){ v = __builtin_bswap32(v); memcpy(ram + (a & RAM_MASK), &v, 4); }
RI void wr64_ram(u32 a, u64 v){ wr32_ram(a, (u32)(v >> 32)); wr32_ram(a + 4, (u32)v); }

/* gen/'s r1/r11-relative accesses with a constant offset (recomp.py stack_access): ram + (b & RAM_MASK)
 * + k, so GCC masks each r1 value once and folds k into the load/store (rd32_ram(b + k) masked every
 * address, often precomputed and spilled). Same bytes as rd32_ram(b + k) while b & RAM_MASK is more
 * than 32 KB from both ends of the mirror: the stack lies below STACK_TOP (recomp_call_guest checks). */
RI u8 *sp_at(u32 b, s32 k){ return ram + (b & RAM_MASK) + k; }
RI u8  rd8_sp (u32 b, s32 k){ return *sp_at(b, k); }
RI u16 rd16_sp(u32 b, s32 k){ u16 v; memcpy(&v, sp_at(b, k), 2); return __builtin_bswap16(v); }
RI u32 rd32_sp(u32 b, s32 k){ u32 v; memcpy(&v, sp_at(b, k), 4); return __builtin_bswap32(v); }
RI u64 rd64_sp(u32 b, s32 k){ return ((u64)rd32_sp(b, k) << 32) | rd32_sp(b, k + 4); }
RI void wr8_sp (u32 b, s32 k, u8 v){ *sp_at(b, k) = v; }
RI void wr16_sp(u32 b, s32 k, u16 v){ v = __builtin_bswap16(v); memcpy(sp_at(b, k), &v, 2); }
RI void wr32_sp(u32 b, s32 k, u32 v){ v = __builtin_bswap32(v); memcpy(sp_at(b, k), &v, 4); }
RI void wr64_sp(u32 b, s32 k, u64 v){ wr32_sp(b, k, (u32)(v >> 32)); wr32_sp(b, k + 4, (u32)v); }

RI float  u2f(u32 v){ float f; memcpy(&f, &v, 4); return f; }
RI u32    f2u(float f){ u32 v; memcpy(&v, &f, 4); return v; }
RI double u2d(u64 v){ double d; memcpy(&d, &v, 8); return d; }
RI u64    d2u(double d){ u64 v; memcpy(&v, &d, 8); return v; }

/* ---- condition register ---- */
#define CRBIT(n) ((cpu.cr >> (31 - (n))) & 1u)
static inline void setcrbit(int n, u32 v){ cpu.cr = (cpu.cr & ~(1u << (31 - n))) | ((v & 1u) << (31 - n)); }
static inline void setcrf(int f, u32 v){ int sh = 28 - 4 * f; cpu.cr = (cpu.cr & ~(0xFu << sh)) | (v << sh); }
static inline void cmps(int f, s32 a, s32 b){ setcrf(f, (a < b ? 8 : a > b ? 4 : 2) | cpu.xer_so); }
static inline void cmpu(int f, u32 a, u32 b){ setcrf(f, (a < b ? 8 : a > b ? 4 : 2) | cpu.xer_so); }
#define CR0(x) cmps(0, (s32)(x), 0)
static inline void fcmp(int f, double a, double b){ setcrf(f, (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1); }
void ppc_fp_record(void);   /* Rc=1 float ops: cr1 <- FPSCR[FX..OX] */
/* gen/ keeps each CR field in a local (cr0..cr7, low 4 bits) and XER[CA] in `ca` */
static inline u32 cmp_s(s32 a, s32 b){ return a < b ? 8 : a > b ? 4 : 2; }
static inline u32 cmp_u(u32 a, u32 b){ return a < b ? 8 : a > b ? 4 : 2; }
static inline u32 fcmp_v(double a, double b){ return (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1; }

/* ---- integer helpers ---- */
static inline u32 rotl(u32 v, int n){ n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 cntlzw(u32 v){ return v ? __builtin_clz(v) : 32; }
static inline u32 slw(u32 a, u32 b){ return (b & 0x20) ? 0 : a << (b & 31); }
static inline u32 srw(u32 a, u32 b){ return (b & 0x20) ? 0 : a >> (b & 31); }
static inline u32 sraw(u32 a, u32 b){
    s32 s = (s32)a;
    if (b & 0x20) { cpu.xer_ca = s < 0; return s < 0 ? 0xFFFFFFFFu : 0; }
    b &= 31; cpu.xer_ca = s < 0 && b && (a & ((1u << b) - 1)); return (u32)(s >> b);
}
static inline u32 srawi(u32 a, int n){
    s32 s = (s32)a; cpu.xer_ca = s < 0 && n && (a & ((1u << n) - 1)); return (u32)(s >> n);
}
static inline u32 addc(u32 a, u32 b, u32 c){ u64 s = (u64)a + b + c; cpu.xer_ca = (u32)(s >> 32); return (u32)s; }
static inline u32 addc_l(u32 *ca, u32 a, u32 b, u32 c){ u64 s = (u64)a + b + c; *ca = (u32)(s >> 32); return (u32)s; }
static inline u32 sraw_l(u32 *ca, u32 a, u32 b){
    s32 s = (s32)a;
    if (b & 0x20) { *ca = s < 0; return s < 0 ? 0xFFFFFFFFu : 0; }
    b &= 31; *ca = s < 0 && b && (a & ((1u << b) - 1)); return (u32)(s >> b);
}
static inline u32 srawi_l(u32 *ca, u32 a, int n){
    s32 s = (s32)a; *ca = s < 0 && n && (a & ((1u << n) - 1)); return (u32)(s >> n);
}
/* Dolphin: divide by zero / overflow results */
static inline u32 divw(u32 a, u32 b){
    s32 sa = (s32)a, sb = (s32)b;
    if (sb == 0 || (a == 0x80000000u && sb == -1)) return sa < 0 ? 0xFFFFFFFFu : 0;
    return (u32)(sa / sb);
}
static inline u32 divwu(u32 a, u32 b){ return b ? a / b : 0; }
static inline u32 mulhw(u32 a, u32 b){ return (u32)(((s64)(s32)a * (s32)b) >> 32); }
static inline u32 mulhwu(u32 a, u32 b){ return (u32)(((u64)a * b) >> 32); }

static inline u32 get_xer(void){ return (cpu.xer_so << 31) | (cpu.xer_ov << 30) | (cpu.xer_ca << 29) | cpu.xer_bc; }
static inline void set_xer(u32 v){ cpu.xer_so = v >> 31; cpu.xer_ov = (v >> 30) & 1; cpu.xer_ca = (v >> 29) & 1; cpu.xer_bc = v & 0x7F; }

/* ---- floating point (Dolphin's ForceSingle / Force25Bit) ---- */
static inline double fsgl(double d){ return (double)(float)d; }
static inline double f25(double d){ u64 b = d2u(d); b = (b & 0xFFFFFFFFF8000000ull) + (b & 0x8000000ull); return u2d(b); }
static inline double fnoneg(double d){ return isnan(d) ? d : -d; }
/* fctiw/fctiwz result bits (Dolphin): saturated to s32, NaN -> 0x80000000, -0 quirk in the upper
 * word. ARM: vcvt (toward zero) / vcvtr (FPSCR mode, nearest like rint) saturate the same way and
 * give 0 for NaN, so only NaN is special-cased; was libm trunc + an out-of-line call per site. */
RI u64 fctiw_bits(double b, int tz){
    s32 r;
#ifdef __arm__
    float t;
    if (tz) __asm__("vcvt.s32.f64 %0, %P1" : "=t"(t) : "w"(b));
    else __asm__("vcvtr.s32.f64 %0, %P1" : "=t"(t) : "w"(b));
    memcpy(&r, &t, 4);
#else
    double t = tz ? trunc(b) : rint(b);
    if (t <= -2147483648.0) r = (s32)0x80000000; else if (t >= 2147483647.0) r = 0x7FFFFFFF; else r = (s32)t;
#endif
    if (isnan(b)) r = (s32)0x80000000;
    u64 v = 0xFFF8000000000000ull | (u32)r;
    if (r == 0 && signbit(b)) v |= 0x100000000ull;
    return v;
}
double ppc_fres(double b);      /* Gekko table estimates; see recomp_unknowns.md */
double ppc_frsqrte(double b);
/* single-precision madd (fmadds, ps_madd...): a is single, c is cut to 25 bits (f25), so
 * a*c is exact in a double and a*c + b rounds once, like fma(). fma() itself is a
 * software libm call on the 3DS (VFPv2 has no fused multiply-add). */
static inline double fmadd_s(double a, double c, double b){ return a * c + b; }
static inline double fsel(double a, double b, double c){ return (a >= -0.0) ? b : c; }
/* single-precision ops as gen/ emits them (fadds..fnmsubs, ps_add..ps_madds1), result rounded
 * to single. Default: Dolphin's double + fsgl with c cut by f25 and one rounding per madd.
 * make FLOATOPS=1 (RECOMP_FLOATOPS): mul/madd/msub in float (inputs rounded to float first,
 * no f25, madd rounds twice). add/sub/div stay double + fsgl: the int->float idiom subtracts
 * a double magic constant (fsubs), and rounding those inputs broke it. recomp_unknowns.md "Float ops". */
static inline double SADD(double a, double b){ return fsgl(a + b); }
static inline double SSUB(double a, double b){ return fsgl(a - b); }
static inline double SDIV(double a, double b){ return fsgl(a / b); }
#ifdef RECOMP_FLOATOPS
static inline double SMUL(double a, double c){ return (float)a * (float)c; }
static inline double SMADD(double a, double c, double b){ return (float)a * (float)c + (float)b; }
static inline double SMSUB(double a, double c, double b){ return (float)a * (float)c - (float)b; }
#else
static inline double SMUL(double a, double c){ return fsgl(a * f25(c)); }
static inline double SMADD(double a, double c, double b){ return fsgl(fmadd_s(a, f25(c), b)); }
static inline double SMSUB(double a, double c, double b){ return fsgl(fmadd_s(a, f25(c), -b)); }
#endif

/* ---- paired-single quantization (GQR) ---- */
static inline double dequant(u32 a, int type, int scale, int *size){
    float s = ldexpf(1.0f, -((scale << 26) >> 26));
    switch (type) {
    case 4: *size = 1; return (double)((float)rd8(a) * s);
    case 5: *size = 2; return (double)((float)rd16(a) * s);
    case 6: *size = 1; return (double)((float)(s8)rd8(a) * s);
    case 7: *size = 2; return (double)((float)(s16)rd16(a) * s);
    default: *size = 4; return (double)u2f(rd32(a));
    }
}
static inline void psq_load(int fd, u32 a, int w, int i){
    u32 g = cpu.gqr[i]; int type = (g >> 16) & 7, scale = (g >> 24) & 0x3F, sz;
    double p0 = dequant(a, type, scale, &sz);
    double p1 = w ? 1.0 : dequant(a + sz, type, scale, &sz);
    F[fd].ps0 = p0; F[fd].ps1 = p1;
}
static inline void quant(u32 a, double v, int type, int scale, int *size){
    float f = (float)v * u2f((u32)(127 + ((scale << 26) >> 26)) << 23);
    switch (type) {
    case 4: *size = 1; wr8 (a, (u8) (f <= 0 ? 0 : f >= 255.0f ? 255 : (int)f)); return;
    case 5: *size = 2; wr16(a, (u16)(f <= 0 ? 0 : f >= 65535.0f ? 65535 : (int)f)); return;
    case 6: *size = 1; wr8 (a, (u8) (s8) (f <= -128.0f ? -128 : f >= 127.0f ? 127 : (int)f)); return;
    case 7: *size = 2; wr16(a, (u16)(s16)(f <= -32768.0f ? -32768 : f >= 32767.0f ? 32767 : (int)f)); return;
    default: *size = 4; wr32(a, f2u((float)v)); return;
    }
}
static inline void psq_store(int fs, u32 a, int w, int i){
    u32 g = cpu.gqr[i]; int type = g & 7, scale = (g >> 8) & 0x3F, sz;
    quant(a, F[fs].ps0, type, scale, &sz);
    if (!w) quant(a + sz, F[fs].ps1, type, scale, &sz);
}

/* by value for gen/ (FPRs are C locals there). 2^n scale built from float bits:
 * ldexpf was a libm call per element */
static inline float qscale(int n){ return u2f((u32)(127 + n) << 23); }
static inline PS psq_ld(u32 a, int w, int i){
    u32 g = cpu.gqr[i]; int type = (g >> 16) & 7;
    PS r;
    if (type == 0) { r.ps0 = u2f(rd32(a)); r.ps1 = w ? 1.0 : u2f(rd32(a + 4)); return r; }
    float s = qscale(-((((int)(g >> 24) & 0x3F) << 26) >> 26));
    switch (type) {
    case 4: r.ps0 = (float)rd8(a) * s;      r.ps1 = w ? 1.0 : (float)rd8(a + 1) * s; break;
    case 5: r.ps0 = (float)rd16(a) * s;     r.ps1 = w ? 1.0 : (float)rd16(a + 2) * s; break;
    case 6: r.ps0 = (float)(s8)rd8(a) * s;  r.ps1 = w ? 1.0 : (float)(s8)rd8(a + 1) * s; break;
    case 7: r.ps0 = (float)(s16)rd16(a) * s; r.ps1 = w ? 1.0 : (float)(s16)rd16(a + 2) * s; break;
    default: r.ps0 = u2f(rd32(a)); r.ps1 = w ? 1.0 : u2f(rd32(a + 4)); break;
    }
    return r;
}
static inline void psq_st(PS v, u32 a, int w, int i){
    u32 g = cpu.gqr[i]; int type = g & 7, sz;
    if (type == 0) { wr32(a, f2u((float)v.ps0)); if (!w) wr32(a + 4, f2u((float)v.ps1)); return; }
    int scale = (g >> 8) & 0x3F;
    quant(a, v.ps0, type, scale, &sz);
    if (!w) quant(a + sz, v.ps1, type, scale, &sz);
}

/* psq_l/psq_st with GQR0 (I = 0: MWCC's f14-f31 saves, PSMTX-style code): GQR0 is 0 (float, no
 * scale), so no GQR load or type switch; _ram = r1-relative (no IS_HW check, no WR_MARK, like
 * wr32_ram). recomp.py guards the assumption: a translated mtspr GQR0 makes funcs.h define
 * RECOMP_GQR0_WRITTEN (the general path below), check_gqr_writes() fails on runtime/ writers. */
#ifndef RECOMP_GQR0_WRITTEN
RI PS psq_ld0(u32 a, int w){ PS r; r.ps0 = u2f(rd32(a)); r.ps1 = w ? 1.0 : u2f(rd32(a + 4)); return r; }
RI PS psq_ld0_ram(u32 a, int w){ PS r; r.ps0 = u2f(rd32_ram(a)); r.ps1 = w ? 1.0 : u2f(rd32_ram(a + 4)); return r; }
RI void psq_st0(PS v, u32 a, int w){ wr32(a, f2u((float)v.ps0)); if (!w) wr32(a + 4, f2u((float)v.ps1)); }
RI void psq_st0_ram(PS v, u32 a, int w){ wr32_ram(a, f2u((float)v.ps0)); if (!w) wr32_ram(a + 4, f2u((float)v.ps1)); }
RI PS psq_ld0_sp(u32 b, s32 k, int w){ PS r; r.ps0 = u2f(rd32_sp(b, k)); r.ps1 = w ? 1.0 : u2f(rd32_sp(b, k + 4)); return r; }
RI void psq_st0_sp(PS v, u32 b, s32 k, int w){ wr32_sp(b, k, f2u((float)v.ps0)); if (!w) wr32_sp(b, k + 4, f2u((float)v.ps1)); }
#else
#define psq_ld0(a, w) psq_ld(a, w, 0)
#define psq_ld0_ram(a, w) psq_ld(a, w, 0)
#define psq_st0(v, a, w) psq_st(v, a, w, 0)
#define psq_st0_ram(v, a, w) psq_st(v, a, w, 0)
#define psq_ld0_sp(b, k, w) psq_ld((b) + (k), w, 0)
#define psq_st0_sp(v, b, k, w) psq_st(v, (b) + (k), w, 0)
#endif

#ifdef RECOMP_FNPROF               /* make FNPROF=1: sampler counts the running guest function */
extern volatile u32 recomp_cur_fn;
#define FNPROF(x) (recomp_cur_fn = (x))
#else
#define FNPROF(x) ((void)0)
#endif

/* ---- system ---- */
u32  ppc_mfspr(int n);
void ppc_mtspr(int n, u32 v);
u32  ppc_mftb(int tbr);
void ppc_mtmsr(u32 v);
void ppc_sc(u32 pc);
void ppc_rfi(u32 pc);
void ppc_trap(u32 pc);
void ppc_mtfsf(u32 mask, u64 v);
static inline void dcbz(u32 a){ a &= ~31u; for (int k = 0; k < 32; k += 4) wr32(a + k, 0); }

/* ---- control flow ---- */
typedef void (*GuestFn)(void);
void call_indirect(u32 target);       /* function table lookup */
void recomp_bad(u32 pc, const char *why);
void recomp_skipped(u32 addr);        /* MetroTRK and other skipped code */
void recomp_sdk_call(const char *name); /* logs an SDK call that runtime/ does not implement */

/* ---- runtime services (runtime/log.c, runtime/dol.c) ---- */
void recomp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void recomp_sdk_log(const char *name);  /* same report for hand-written SDK functions */
void recomp_done(const char *why) __attribute__((noreturn)); /* ends the guest run */
int  recomp_load_dol(void);             /* main.dol + boot info into ram; 0 on success */
void recomp_call_guest(u32 fn, u32 arg); /* interrupt-style call: guest state saved around it */
void recomp_call_guest2(u32 fn, u32 arg, u32 arg2);
void recomp_dvd_service(void);          /* runs completed DVD callbacks */
void recomp_ar_service(void);           /* runs completed ARQ callbacks */
extern volatile u32 recomp_event_pending; /* an ARQ completion is in flight */
void recomp_poll_events(void);          /* delivers due events if MSR[EE] */
#define RECOMP_POLL() do { if (__builtin_expect(recomp_event_pending, 0)) recomp_poll_events(); } while (0)
u32  recomp_retrace_count(void);         /* frames so far (VI retraces) */
void recomp_ax_service(void);           /* AX frame callbacks due since the last retrace */
#define AX_AI_CALLBACK 0x80145a00u        /* __AXOutAiCallback: AX's AI DMA callback */
extern u32 recomp_ai_dma_cb, recomp_ai_dma_addr, recomp_ai_dma_len;   /* AI DMA (runtime/ai.c) */
/* frame timing (runtime/log.c): ticks spent per bucket, logged by VI */
enum { PROF_GX_VTX, PROF_GX_TEX, PROF_GX_END, PROF_AUDIO, PROF_GX_STATE,
       PROF_VX_UNIF, PROF_VX_DEC, PROF_VX_DL, PROF_LOG, PROF_PREDRAW, PROF_GX_SYNC, PROF_GPU, PROF_GPU_SPAN, PROF_DVD,
       PROF_GX_WAIT, PROF_GX_BUSY, PROF_GPU_DRAWS, PROF_GPU_IDX, PROF_N };   /* parts of PROF_GX_VTX: uniforms, decode loop, draws from display lists; PROF_LOG: recomp_log writes (inside the others); PROF_GX_SYNC: C3D_FrameBegin (vblank cap + GPU queue wait); PROF_GPU: GPU queue time from C3D_FrameEnd to queue done (C3D_GetDrawingTime; with mid-frame submits only the tail); PROF_GPU_SPAN: first mid-frame submit (else C3D_FrameEnd) to queue done; PROF_DVD: game thread waiting for the DVD reader thread (inside the others); with the GX thread (recomp_gx_thread) the PROF_GX_* and PROF_VX_* buckets are GX-thread time, PROF_GX_WAIT is the game thread waiting for it, PROF_GX_BUSY the GX thread handling records; PROF_GPU_DRAWS/IDX: counts, not ticks (C3D_DrawElements calls, indices) */
extern u64 recomp_prof[PROF_N];
extern volatile u32 recomp_logw_since;  /* low word of the tick the log writer's fwrite+fflush began, 0 = idle */
extern volatile u32 recomp_logw_n;      /* log writes finished */
struct Thread_tag *recomp_aux_thread(void (*f)(void *), size_t stack, int prio);   /* thread off the game's core (log.c) */
int  recomp_aux_core(void);             /* the core recomp_aux_thread uses: 2 (New 3DS), 1 or 0 */
void recomp_thread_crash_log(void);     /* crashes on the calling thread end the log too (log.c) */
extern u64 recomp_first_draw;   /* tick of the frame's first GX draw (0 = none yet); PROF_PREDRAW: frame time before it */
extern u32 recomp_run_frames;
u64 recomp_ticks(void);
void recomp_prof_frame(u32 retrace);
void recomp_prof_sampler_start(void);  /* cpu.lr histogram thread (log.c) */
void recomp_ax_close(void);             /* flushes the WAV dump, stops ndsp */
u32 recomp_retrace_count(void);
u8 recomp_aram_read8(u32 a);            /* ARAM byte (runtime/ar.c) */
void recomp_wait_point(void);           /* pending "interrupts": DVD, then ARQ */
u64  recomp_timebase(void);             /* Gekko timebase, 40.5 MHz */
void recomp_gx_init(void);             /* citro3d, render target, shader */
void recomp_gx_start(void);            /* flag files, then the GX thread on core 2 (after recomp_log_open) */
void recomp_gx_sync(void);             /* game thread: wait until the GX thread handled every GX call so far */
void recomp_gx_retrace(void);          /* VIWaitForRetrace: sync, then apply a requested GX thread on/off */
void recomp_gx_thread_toggle(void);    /* bottom-screen tap: GX thread on/off at the next retrace */
void recomp_gx_stop(void);             /* at exit: sync, then end the GX thread */
extern int recomp_gx_thread;           /* GX calls are handled on the GX thread now (else inline on the game thread) */
extern u32 recomp_gx_thread_flips;     /* GX thread switches so far (for the # time lines) */
void recomp_status(const char *s);     /* bottom screen's fixed last row (main.c; game thread) */
#define RECOMP_STATUS_ROWS 8            /* bottom screen's fixed rows: stats (log.c), then the toggle line (gx.c) */
void recomp_status_row(int row, const char *s);   /* fixed row 0..RECOMP_STATUS_ROWS-1, redrawn only when changed (main.c; game thread) */
extern volatile u32 recomp_gx_stat[4];  /* GX frames, draws, triangles (running totals), linear heap free (bytes); written by whoever runs GX */
extern u32 recomp_dvd_slow_n, recomp_dvd_late_n;   /* dvd.c */
void recomp_gx_kick_toggle(void);      /* ZL: mid-frame GPU submit on/off from the next frame */
extern int recomp_gx_kick_on;          /* mid-frame GPU submit mode now (for the # time lines) */
extern u32 recomp_gx_kick_flips;       /* ZL toggles so far */
extern u32 recomp_gx_dbg[3];           /* last draw: gpucmd offset at entry, prim, vertex count */
void recomp_gx_fifo_write(u32 v, int size); /* WGPIPE 0xCC008000 */
void recomp_log_open(void);
void recomp_log_close(void);
