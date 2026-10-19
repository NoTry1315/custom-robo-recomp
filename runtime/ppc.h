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

#define R cpu.gpr
#define F cpu.fpr

/* ---- memory (big-endian guest) ---- */
u32  hw_read(u32 a, int size);
void hw_write(u32 a, u32 v, int size);
#define IS_HW(a) (((a) & 0xFE000000u) == 0xCC000000u)

static inline u8  rd8 (u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,1); return ram[a & RAM_MASK]; }
static inline u16 rd16(u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,2); u16 v; memcpy(&v, ram + (a & RAM_MASK), 2); return __builtin_bswap16(v); }
static inline u32 rd32(u32 a){ if (__builtin_expect(IS_HW(a),0)) return hw_read(a,4); u32 v; memcpy(&v, ram + (a & RAM_MASK), 4); return __builtin_bswap32(v); }
static inline u64 rd64(u32 a){ return ((u64)rd32(a) << 32) | rd32(a + 4); }
static inline void wr8 (u32 a, u8 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,1); return; } ram[a & RAM_MASK] = v; }
static inline void wr16(u32 a, u16 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,2); return; } v = __builtin_bswap16(v); memcpy(ram + (a & RAM_MASK), &v, 2); }
static inline void wr32(u32 a, u32 v){ if (__builtin_expect(IS_HW(a),0)) { hw_write(a,v,4); return; } v = __builtin_bswap32(v); memcpy(ram + (a & RAM_MASK), &v, 4); }
static inline void wr64(u32 a, u64 v){ wr32(a, (u32)(v >> 32)); wr32(a + 4, (u32)v); }

/* r1-relative (stack) accesses never reach hardware registers: no IS_HW check */
static inline u8  rd8_ram (u32 a){ return ram[a & RAM_MASK]; }
static inline u16 rd16_ram(u32 a){ u16 v; memcpy(&v, ram + (a & RAM_MASK), 2); return __builtin_bswap16(v); }
static inline u32 rd32_ram(u32 a){ u32 v; memcpy(&v, ram + (a & RAM_MASK), 4); return __builtin_bswap32(v); }
static inline u64 rd64_ram(u32 a){ return ((u64)rd32_ram(a) << 32) | rd32_ram(a + 4); }
static inline void wr8_ram (u32 a, u8 v){ ram[a & RAM_MASK] = v; }
static inline void wr16_ram(u32 a, u16 v){ v = __builtin_bswap16(v); memcpy(ram + (a & RAM_MASK), &v, 2); }
static inline void wr32_ram(u32 a, u32 v){ v = __builtin_bswap32(v); memcpy(ram + (a & RAM_MASK), &v, 4); }
static inline void wr64_ram(u32 a, u64 v){ wr32_ram(a, (u32)(v >> 32)); wr32_ram(a + 4, (u32)v); }

static inline float  u2f(u32 v){ float f; memcpy(&f, &v, 4); return f; }
static inline u32    f2u(float f){ u32 v; memcpy(&v, &f, 4); return v; }
static inline double u2d(u64 v){ double d; memcpy(&d, &v, 8); return d; }
static inline u64    d2u(double d){ u64 v; memcpy(&v, &d, 8); return v; }

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
static inline u64 fctiw_bits(double b, int tz){
    s32 r; double t = tz ? trunc(b) : rint(b);
    if (isnan(b) || t <= -2147483648.0) r = (s32)0x80000000; else if (t >= 2147483647.0) r = 0x7FFFFFFF; else r = (s32)t;
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
/* frame timing (runtime/log.c): ticks spent per bucket, logged by VI */
enum { PROF_GX_VTX, PROF_GX_TEX, PROF_GX_END, PROF_AUDIO, PROF_GX_STATE,
       PROF_VX_UNIF, PROF_VX_DEC, PROF_VX_DL, PROF_LOG, PROF_PREDRAW, PROF_N };   /* parts of PROF_GX_VTX: uniforms, decode loop, draws from display lists; PROF_LOG: recomp_log writes (inside the others) */
extern u64 recomp_prof[PROF_N];
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
extern u32 recomp_gx_dbg[3];           /* last draw: gpucmd offset at entry, prim, vertex count */
void recomp_gx_fifo_write(u32 v, int size); /* WGPIPE 0xCC008000 */
void recomp_log_open(void);
void recomp_log_close(void);
