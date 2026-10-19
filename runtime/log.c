/* Run log (sdmc:/cr_autotest.log): every SDK call, recomp_bad, hardware
 * access; a summary and the closing "# DONE" line that autotest.ps1 waits for.
 * A watchdog thread ends the log if nothing new is logged for HANG_MS: the
 * guest is stuck, either silent or polling the same SDK calls. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include "ppc.h"

/* Frame timing: ticks (SYSCLOCK_ARM11, 268 MHz) per bucket, summed over 60
 * retraces. "game" is the rest of the frame: translated code + SDK layer. */
u64 recomp_prof[PROF_N];
u64 recomp_ticks(void) { return svcGetSystemTick(); }

/* Sampling profiler: a higher-priority thread on the guest's core wakes every
 * 0.5 ms and counts cpu.lr, the return address of the last guest call. The
 * time is in that callee (a leaf, or an SDK function) or in its caller after
 * it returned; gen/ shows the callee at "cpu.lr = 0x<lr>u; f_...". Logged as
 * "# prof" lines every 300 retraces, then reset. */
#define SAMPLE_SLOTS 4096
static struct { u32 lr, n; } samples[SAMPLE_SLOTS];
static volatile u32 sample_total;
static volatile int sampler_stop;

static void sampler(void *arg) {
    (void)arg;
    while (!sampler_stop) {
        svcSleepThread(500000);
        u32 lr = cpu.lr, h = (lr >> 2) * 2654435761u >> 20;
        for (u32 i = 0; i < SAMPLE_SLOTS; i++) {
            u32 k = (h + i) & (SAMPLE_SLOTS - 1);
            if (samples[k].lr == lr || !samples[k].n) { samples[k].lr = lr; samples[k].n++; break; }
        }
        sample_total++;
    }
}

void recomp_prof_sampler_start(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    threadCreate(sampler, NULL, 0x1000, prio - 1, -2, true);
}

static void sampler_dump(u32 retrace) {
    u32 total = sample_total;
    if (!total) return;
    recomp_log("# prof r%lu: %lu samples, top call sites (lr: share)\n", (unsigned long)retrace, (unsigned long)total);
    for (int t = 0; t < 25; t++) {
        int best = -1;
        for (int i = 0; i < SAMPLE_SLOTS; i++) if (samples[i].n && (best < 0 || samples[i].n > samples[best].n)) best = i;
        if (best < 0) break;
        recomp_log("# prof  %08lx %5.1f%%\n", (unsigned long)samples[best].lr, samples[best].n * 100.0 / total);
        samples[best].n = 0;
    }
    memset(samples, 0, sizeof samples);
    sample_total = 0;
}

void recomp_prof_frame(u32 retrace) {
    static u64 last, start, worst;
    static u64 sum[PROF_N];
    u64 now = svcGetSystemTick();
    if (last) {
        u64 dt = now - last;
        if (dt > worst) worst = dt;
    } else start = now;
    last = now;
    if (retrace % 60 || retrace == 0) return;
    double ms = 1000.0 / SYSCLOCK_ARM11, wall = (now - start) * ms;
    double vtx = (recomp_prof[PROF_GX_VTX] - sum[PROF_GX_VTX]) * ms / 60, tex = (recomp_prof[PROF_GX_TEX] - sum[PROF_GX_TEX]) * ms / 60;
    double end = (recomp_prof[PROF_GX_END] - sum[PROF_GX_END]) * ms / 60, aud = (recomp_prof[PROF_AUDIO] - sum[PROF_AUDIO]) * ms / 60;
    double st = (recomp_prof[PROF_GX_STATE] - sum[PROF_GX_STATE]) * ms / 60;
    double frame = wall / 60;
    recomp_log("# time r%lu: %.1f fps, frame %.1f ms (worst %.1f): game %.1f, gx vertex %.1f, gx tex %.1f, gx state %.1f, gx end/wait %.1f, audio %.1f\n",
               (unsigned long)retrace, 1000.0 / frame, frame, worst * ms, frame - vtx - tex - st - end - aud, vtx, tex, st, end, aud);
    for (int i = 0; i < PROF_N; i++) sum[i] = recomp_prof[i];
    start = now; worst = 0;
    if (retrace % 300 == 0) sampler_dump(retrace);
}

#define LOG_PATH "sdmc:/cr_autotest.log"
#define PER_NAME 20         /* calls logged in full per SDK function */
#define HANG_MS 20000      /* a 30-retrace log gap on the title's 3D background in real-time Citra exceeds 4 s */

static FILE *log_fp;
static LightLock lock;
static volatile u32 heartbeat;
static const char *volatile ring[32];
static volatile u32 ring_pos;
jmp_buf recomp_exit_jmp;

static struct { const char *name; u32 n; } counts[2048];

static u32 *count_of(const char *name) {
    u32 h = (u32)(uintptr_t)name * 2654435761u >> 21;
    for (;; h = (h + 1) & 2047) {
        if (!counts[h].name) counts[h].name = name;
        if (counts[h].name == name) return &counts[h].n;
    }
}

void recomp_log(const char *fmt, ...) {
    va_list ap;
    if (!log_fp) return;
    LightLock_Lock(&lock);
    heartbeat++;
    va_start(ap, fmt); vfprintf(log_fp, fmt, ap); va_end(ap);
    fflush(log_fp);
    LightLock_Unlock(&lock);
}

void recomp_sdk_log(const char *name) {
    u32 *n = count_of(name);
    ring[ring_pos++ & 31] = name;
    if (++*n <= PER_NAME)
        recomp_log("sdk %s r3=%08lx r4=%08lx r5=%08lx r6=%08lx lr=%08lx\n", name, (unsigned long)R[3],
                   (unsigned long)R[4], (unsigned long)R[5], (unsigned long)R[6], (unsigned long)cpu.lr);
}

void recomp_sdk_call(const char *name) { recomp_sdk_log(name); }

void recomp_bad(u32 pc, const char *why) {
    u32 *n = count_of(why);
    if (++*n <= PER_NAME) recomp_log("recomp_bad %08lx: %s (lr=%08lx)\n", (unsigned long)pc, why, (unsigned long)cpu.lr);
}

static int by_count(const void *a, const void *b) {
    const u32 *x = a, *y = b;
    return (int)y[1] - (int)x[1];
}

static void summary(void) {
    static u32 rows[2048][2];
    int k = 0;
    for (int i = 0; i < 2048; i++) if (counts[i].name) { rows[k][0] = i; rows[k][1] = counts[i].n; k++; }
    qsort(rows, k, sizeof rows[0], by_count);
    recomp_log("# summary: %d distinct names (SDK calls and recomp_bad reasons)\n", k);
    for (int i = 0; i < k; i++) recomp_log("#  %8lu %s\n", (unsigned long)rows[i][1], counts[rows[i][0]].name);
    recomp_log("# last calls:");
    for (int i = 0; i < 32; i++) { const char *s = ring[(ring_pos + i) & 31]; if (s) recomp_log(" %s", s); }
    recomp_log("\n# lr=%08lx r1=%08lx\n", (unsigned long)cpu.lr, (unsigned long)R[1]);
}

static volatile int finished;

void recomp_done(const char *why) {
    if (!finished) {
        finished = 1;
        summary();
        recomp_ax_close();
        recomp_log("# DONE %s\n", why);
    }
    longjmp(recomp_exit_jmp, 1);
}

static void watchdog(void *arg) {
    (void)arg;
    u32 last = heartbeat - 1;
    while (!finished) {
        svcSleepThread((s64)HANG_MS * 1000000);
        if (finished) break;
        if (heartbeat == last) {
            finished = 1;
            recomp_log("# nothing new logged for %d ms: guest hang\n", HANG_MS);
            summary();
            recomp_log("# DONE hang\n");
            break;
        }
        last = heartbeat;
    }
}

/* Crashes end the log too: a failed assert(), abort() (malloc/citro3d
 * failures end there) or an ARM exception logs why, the host pc/lr, the
 * guest lr and the last SDK calls, then "# DONE crash". */
Result __real_svcBreak(UserBreakType reason);
static __attribute__((noreturn)) void crash(const char *why, u32 pc, u32 lr) {
    if (!finished) {
        finished = 1;
        recomp_log("# CRASH %s host pc=%08lx lr=%08lx retrace %lu\n", why, (unsigned long)pc, (unsigned long)lr,
                   (unsigned long)recomp_retrace_count());
        summary();
        recomp_log("# DONE crash\n");
    }
    __real_svcBreak(USERBREAK_PANIC);
    for (;;) svcSleepThread(1000000000LL);
}

/* libctru/citro3d panic with svcBreak (GPU command buffer or GX queue full,
 * ndsp, heap setup): linked with --wrap=svcBreak so the caller is logged;
 * look the lr up in custom-robo-recomp.map / objdump */
Result __wrap_svcBreak(UserBreakType reason) {
    char buf[200];
    u32 *a, size, off;
    GPUCMD_GetBuffer(&a, &size, &off);
    snprintf(buf, sizeof buf, "svcBreak(%d) gpucmd %lu/%lu words, last draw entered at %lu (prim %lx n %lu)", (int)reason,
             (unsigned long)off, (unsigned long)size, (unsigned long)recomp_gx_dbg[0], (unsigned long)recomp_gx_dbg[1],
             (unsigned long)recomp_gx_dbg[2]);
    /* heuristic backtrace: stack words that point into .text (addr2line them) */
    extern char __etext[] __attribute__((weak));
    u32 lo = 0x100000, hi = __etext ? (u32)__etext : 0x1000000;
    u32 *sp = __builtin_frame_address(0);
    recomp_log("# CRASH stack:");
    for (int i = 0, k = 0; i < 1024 && k < 24; i++) {
        u32 v = sp[i];
        if (v > lo && v < hi && (v & 1) == 0 && (v & 3) == 0) { recomp_log(" %08lx", (unsigned long)v); k++; }
    }
    recomp_log("\n");
    crash(buf, 0, (u32)__builtin_return_address(0));
}

void __assert_func(const char *file, int line, const char *func, const char *expr) {
    char buf[200];
    snprintf(buf, sizeof buf, "assert %s:%d %s: %s", file, line, func ? func : "", expr);
    crash(buf, 0, (u32)__builtin_return_address(0));
}

void abort(void) { crash("abort", 0, (u32)__builtin_return_address(0)); }

static void on_exception(ERRF_ExceptionInfo *e, CpuRegisters *r) {
    static const char *const names[] = { "prefetch abort", "data abort", "undefined insn", "vfp" };
    char buf[96];
    snprintf(buf, sizeof buf, "%s far=%08lx sp=%08lx", e->type < 4 ? names[e->type] : "exception",
             (unsigned long)e->far, (unsigned long)r->sp);
    crash(buf, r->pc, r->lr);
}

void recomp_log_open(void) {
    LightLock_Init(&lock);
    log_fp = fopen(LOG_PATH, "w");
    threadOnException(on_exception, RUN_HANDLER_ON_FAULTING_STACK, WRITE_DATA_TO_HANDLER_STACK);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    threadCreate(watchdog, NULL, 0x2000, prio - 1, -2, true);
}

void recomp_log_close(void) {
    finished = 1;
    if (log_fp) fclose(log_fp);
    log_fp = NULL;
}
