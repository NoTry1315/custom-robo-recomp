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
u64 recomp_first_draw;
u64 recomp_ticks(void) { return svcGetSystemTick(); }

/* Sampling profiler: a thread on another core (aux_thread) wakes every
 * 0.5 ms and counts cpu.lr, the return address of the last guest call. The
 * time is in that callee (a leaf, or an SDK function) or in its caller after
 * it returned; gen/ shows the callee at "cpu.lr = 0x<lr>u; f_...". Logged as
 * "# prof" lines every 300 retraces, then reset. */
#define SAMPLE_SLOTS 4096
static struct { u32 lr, n; } samples[SAMPLE_SLOTS];
#ifdef RECOMP_FNPROF
/* make FNPROF=1: gen/ stores the running guest function here (0 = SDK/runtime),
 * so "# prof" lines list functions instead of call sites */
volatile u32 recomp_cur_fn;
#endif
static volatile u32 sample_total;
static volatile int sampler_stop;

static void sampler(void *arg) {
    (void)arg;
    while (!sampler_stop) {
        svcSleepThread(500000);
#ifdef RECOMP_FNPROF
        u32 lr = recomp_cur_fn;
#else
        u32 lr = cpu.lr;
#endif
        u32 h = (lr >> 2) * 2654435761u >> 20;
        for (u32 i = 0; i < SAMPLE_SLOTS; i++) {
            u32 k = (h + i) & (SAMPLE_SLOTS - 1);
            if (samples[k].lr == lr || !samples[k].n) { samples[k].lr = lr; samples[k].n++; break; }
        }
        sample_total++;
    }
}

/* Helper threads (sampler, watchdog, log writer) run off the game's core 0:
 * core 2 (New 3DS) if threadCreate there works, else core 1 after
 * APT_SetAppCpuTimeLimit(30), else core 0. Probed once, like mGBA. */
static int aux_core = -3;
static void aux_probe_fn(void *arg) { (void)arg; }
static Thread aux_thread(ThreadFunc f, size_t stack, int prio) {
    if (aux_core == -3) {
        Thread t = threadCreate(aux_probe_fn, NULL, 0x1000, 0x30, 2, false);
        if (t) aux_core = 2;
        else if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)) && (t = threadCreate(aux_probe_fn, NULL, 0x1000, 0x30, 1, false))) aux_core = 1;
        else aux_core = 0;
        if (t) { threadJoin(t, U64_MAX); threadFree(t); }
    }
    return threadCreate(f, NULL, stack, prio, aux_core, true);
}
Thread recomp_aux_thread(ThreadFunc f, size_t stack, int prio) { return aux_thread(f, stack, prio); }
int recomp_aux_core(void) { return aux_core; }   /* probed by the first aux_thread (recomp_log_open) */

void recomp_prof_sampler_start(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    aux_thread(sampler, 0x1000, prio - 1);
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

static u64 log_worst;   /* longest single recomp_log write since the last frame */
static u32 stalls;      /* "# stall" lines so far */

/* bottom-screen stats: the numbers of the # time lines once per 60-retrace window, so a
 * hardware A/B run can be read on the device (game thread; rows above the toggle line) */
struct stats { double frame, worst, game, aud, wait, busy, vtx, tex, st, end, sync, gpu, dvd; };
static void stats_draw(u32 retrace, const struct stats *v) {
    static u32 gf, gd, gt;
    static u64 cost, cost_max;
    u64 t0 = svcGetSystemTick();
    u32 f = recomp_gx_stat[0]; __dmb();
    u32 d = recomp_gx_stat[1], t = recomp_gx_stat[2], nf = f - gf;
    char r[48];
    snprintf(r, sizeof r, "%5.1f fps  speed %3.0f%%  worst %5.1f ms", 1000.0 / v->frame, 100000.0 / 60 / v->frame, v->worst);
    recomp_status_row(0, r);
    snprintf(r, sizeof r, "frame %5.1f  game %5.1f  audio %4.1f", v->frame, v->game, v->aud);
    recomp_status_row(1, r);
    if (recomp_gx_thread) {
        snprintf(r, sizeof r, "gxthr wait %4.1f  busy %4.1f  sync %4.1f", v->wait, v->busy, v->sync);
        recomp_status_row(2, r);
        snprintf(r, sizeof r, "vtx %4.1f tex %4.1f state %4.1f gpu %4.1f", v->vtx, v->tex, v->st, v->gpu);
    } else {
        snprintf(r, sizeof r, "inline vtx %4.1f  tex %4.1f  state %4.1f", v->vtx, v->tex, v->st);
        recomp_status_row(2, r);
        snprintf(r, sizeof r, "end %4.1f  sync %4.1f  gpu %4.1f", v->end, v->sync, v->gpu);
    }
    recomp_status_row(3, r);
    snprintf(r, sizeof r, "scene %lx sub %lx   retrace %lu", (unsigned long)rd32(0x80321d88), (unsigned long)rd32(0x80321d98), (unsigned long)retrace);
    recomp_status_row(4, r);
    snprintf(r, sizeof r, "draws %5lu  tris %6lu  lin %5lu KB", nf ? (unsigned long)((d - gd) / nf) : 0ul, nf ? (unsigned long)((t - gt) / nf) : 0ul,
             (unsigned long)(recomp_gx_stat[3] >> 10));
    recomp_status_row(5, r);
    snprintf(r, sizeof r, "dvd wait %4.1f late %lu slow %lu stall %lu", v->dvd, (unsigned long)recomp_dvd_late_n, (unsigned long)recomp_dvd_slow_n, (unsigned long)stalls);
    recomp_status_row(6, r);
    gf = f; gd = d; gt = t;
    u64 c = svcGetSystemTick() - t0;   /* inside "game" of the next window */
    cost += c; if (c > cost_max) cost_max = c;
    if (retrace % 300 == 0) {
        recomp_log("# status r%lu: bottom-screen stats %.2f ms per update (worst %.2f), every 60 retraces\n", (unsigned long)retrace,
                   cost * 1000.0 / SYSCLOCK_ARM11 / 5, cost_max * 1000.0 / SYSCLOCK_ARM11);
        cost = cost_max = 0;
    }
}

/* the GX thread adds to its buckets while this runs, and a u64 is two stores: read until stable */
static u64 prof_get(int i) { volatile u64 *p = &recomp_prof[i]; u64 a, b; do { a = *p; b = *p; } while (a != b); return a; }

void recomp_prof_frame(u32 retrace) {
    static u64 last, start, worst;
    static u64 sum[PROF_N];
    u64 now = svcGetSystemTick();
    static u64 fsum[PROF_N];   /* per-frame snapshot, for the stall line */
    u64 cur[PROF_N];
    if (last) {
        u64 dt = now - last;
        recomp_prof[PROF_PREDRAW] += recomp_first_draw > last ? recomp_first_draw - last : (recomp_first_draw ? 0 : dt);
    }
    for (int i = 0; i < PROF_N; i++) cur[i] = prof_get(i);
    if (last) {
        u64 dt = now - last;
        if (dt > worst) worst = dt;
        if (dt > SYSCLOCK_ARM11 / 2) {   /* one frame over 0.5 s: where it went */
            stalls++;
            double m = 1000.0 / SYSCLOCK_ARM11;
            u64 d[PROF_N];
            for (int i = 0; i < PROF_N; i++) d[i] = cur[i] - fsum[i];
            u64 lw = log_worst;
            if (recomp_gx_thread)   /* the gx buckets ran on the GX thread */
                recomp_log("# stall r%lu: frame %.0f ms: gx wait %.0f, audio %.0f, log writes %.0f (longest %.0f), dvd wait %.0f, rest (game) %.0f; gx thread busy %.0f (sync %.0f)\n",
                    (unsigned long)retrace, dt * m, d[PROF_GX_WAIT] * m, d[PROF_AUDIO] * m, d[PROF_LOG] * m, lw * m, d[PROF_DVD] * m,
                    (double)(dt - d[PROF_GX_WAIT] - d[PROF_AUDIO]) * m, d[PROF_GX_BUSY] * m, d[PROF_GX_SYNC] * m);
            else
                recomp_log("# stall r%lu: frame %.0f ms: gx vertex %.0f, gx tex %.0f, gx state %.0f, gx end/wait %.0f, gx sync %.0f, audio %.0f, log writes %.0f (longest %.0f), dvd wait %.0f, rest (game) %.0f\n",
                    (unsigned long)retrace, dt * m, d[PROF_GX_VTX] * m, d[PROF_GX_TEX] * m, d[PROF_GX_STATE] * m, d[PROF_GX_END] * m, d[PROF_GX_SYNC] * m, d[PROF_AUDIO] * m,
                    d[PROF_LOG] * m, lw * m, d[PROF_DVD] * m, (double)(dt - d[PROF_GX_VTX] - d[PROF_GX_TEX] - d[PROF_GX_STATE] - d[PROF_GX_END] - d[PROF_GX_SYNC] - d[PROF_AUDIO]) * m);
        }
    } else start = now;
    last = now;
    recomp_first_draw = 0;
    for (int i = 0; i < PROF_N; i++) fsum[i] = cur[i];
    log_worst = 0;
    if (retrace % 60 || retrace == 0) return;
    double ms = 1000.0 / SYSCLOCK_ARM11, wall = (now - start) * ms;
    double vtx = (cur[PROF_GX_VTX] - sum[PROF_GX_VTX]) * ms / 60, tex = (cur[PROF_GX_TEX] - sum[PROF_GX_TEX]) * ms / 60;
    double end = (cur[PROF_GX_END] - sum[PROF_GX_END]) * ms / 60, aud = (cur[PROF_AUDIO] - sum[PROF_AUDIO]) * ms / 60;
    double st = (cur[PROF_GX_STATE] - sum[PROF_GX_STATE]) * ms / 60, sync = (cur[PROF_GX_SYNC] - sum[PROF_GX_SYNC]) * ms / 60;
    double frame = wall / 60;
    if (recomp_gx_thread) {
        double wait = (cur[PROF_GX_WAIT] - sum[PROF_GX_WAIT]) * ms / 60, busy = (cur[PROF_GX_BUSY] - sum[PROF_GX_BUSY]) * ms / 60;
        recomp_log("# time r%lu: %.1f fps, frame %.1f ms (worst %.1f): game %.1f, gx wait %.1f, audio %.1f (log writes %.1f, dvd wait %.1f, inside the others); gx thread busy %.1f: vertex %.1f, tex %.1f, state %.1f, end/wait %.1f, sync %.1f\n",
                   (unsigned long)retrace, 1000.0 / frame, frame, worst * ms, frame - wait - aud, wait, aud,
                   (cur[PROF_LOG] - sum[PROF_LOG]) * ms / 60, (cur[PROF_DVD] - sum[PROF_DVD]) * ms / 60, busy, vtx, tex, st, end, sync);
    } else
        recomp_log("# time r%lu: %.1f fps, frame %.1f ms (worst %.1f): game %.1f, gx vertex %.1f, gx tex %.1f, gx state %.1f, gx end/wait %.1f, gx sync %.1f, audio %.1f (log writes %.1f, dvd wait %.1f, inside the others)\n",
                   (unsigned long)retrace, 1000.0 / frame, frame, worst * ms, frame - vtx - tex - st - end - sync - aud, vtx, tex, st, end, sync, aud,
                   (cur[PROF_LOG] - sum[PROF_LOG]) * ms / 60, (cur[PROF_DVD] - sum[PROF_DVD]) * ms / 60);
    double pre = (cur[PROF_PREDRAW] - sum[PROF_PREDRAW]) * ms / 60, gpu = (cur[PROF_GPU] - sum[PROF_GPU]) * ms / 60;
    struct stats sv = { frame, worst * ms, frame - aud, aud, 0, 0, vtx, tex, st, end, sync, gpu, (cur[PROF_DVD] - sum[PROF_DVD]) * ms / 60 };
    if (recomp_gx_thread) { sv.wait = (cur[PROF_GX_WAIT] - sum[PROF_GX_WAIT]) * ms / 60; sv.busy = (cur[PROF_GX_BUSY] - sum[PROF_GX_BUSY]) * ms / 60; sv.game -= sv.wait; }
    else sv.game -= vtx + tex + st + end + sync;
    stats_draw(retrace, &sv);
    double span = (cur[PROF_GPU_SPAN] - sum[PROF_GPU_SPAN]) * ms / 60;
    static u32 flips, gflips;   /* a window with a ZL toggle (or a bottom-screen tap) in it mixes both modes */
    const char *kick = recomp_gx_kick_flips != flips ? "mixed" : recomp_gx_kick_on ? "on" : "off";
    const char *gxt = recomp_gx_thread_flips != gflips ? "mixed" : recomp_gx_thread ? "on" : "off";
    flips = recomp_gx_kick_flips; gflips = recomp_gx_thread_flips;
    recomp_log("# time r%lu split: before first draw %.1f, from first draw %.1f; gpu %.1f (tail: C3D_FrameEnd -> queue done), gpu span %.1f (first submit -> queue done, incl. idle gaps); gpukick %s, gx thread %s; %.0f draw calls, %.0f indices per frame\n",
               (unsigned long)retrace, pre, frame - pre, gpu, span, kick, gxt,
               (cur[PROF_GPU_DRAWS] - sum[PROF_GPU_DRAWS]) / 60.0, (cur[PROF_GPU_IDX] - sum[PROF_GPU_IDX]) / 60.0);
    if (retrace % 300 == 0) {
        double un = (cur[PROF_VX_UNIF] - sum[PROF_VX_UNIF]) * ms / 60, dec = (cur[PROF_VX_DEC] - sum[PROF_VX_DEC]) * ms / 60;
        double dl = (cur[PROF_VX_DL] - sum[PROF_VX_DL]) * ms / 60;
        recomp_log("# time r%lu gx vertex: uniforms %.1f, decode %.1f, rest %.1f; display lists %.1f, immediate %.1f\n",
                   (unsigned long)retrace, un, dec, vtx - un - dec, dl, vtx - dl);
    }
    for (int i = 0; i < PROF_N; i++) sum[i] = cur[i];
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

/* Log lines go to a memory ring; a lower-priority writer thread puts them on
 * the SD card. A single fwrite+fflush blocked the game for up to 4 s on
 * hardware (hw run 6, 2026-10-19: every '# stall' was log writes). Crashes and
 * the end of the run drain the ring synchronously (log_drain). */
#define LOGBUF (1u << 20)
static char logbuf[LOGBUF];
static volatile u32 log_head, log_tail;   /* free-running byte counts: written / on the card */
static LightLock wlock;                   /* fwrite and log_tail */
static LightEvent log_ev;
static volatile int log_closing;
/* hw runs 15-21: single DVD reads (and a failed fopen) took 2-4 s; these let
 * dvd.c say whether a log write was on the card at the same time */
volatile u32 recomp_logw_since, recomp_logw_n;

static void log_drain(void) {
    LightLock_Lock(&wlock);
    u32 t = log_tail, h = log_head;
    u64 t0 = svcGetSystemTick(), dt = 0;
    if (log_fp && t != h) {
        recomp_logw_since = (u32)t0 | 1;
        while (t != h) {
            u32 o = t & (LOGBUF - 1), n = h - t;
            if (n > LOGBUF - o) n = LOGBUF - o;
            fwrite(logbuf + o, 1, n, log_fp);
            t += n;
        }
        fflush(log_fp);
        dt = svcGetSystemTick() - t0;
        recomp_logw_since = 0;
        recomp_logw_n++;
    }
    u32 n = t - log_tail;
    log_tail = t;
    LightLock_Unlock(&wlock);
    if (dt > SYSCLOCK_ARM11 / 4)
        recomp_log("# log write slow r%lu: %lu bytes at %lu KB took %.0f ms\n", (unsigned long)recomp_retrace_count(),
                   (unsigned long)n, (unsigned long)((t - n) >> 10), dt * 1000.0 / SYSCLOCK_ARM11);
}

static void log_writer(void *arg) {
    (void)arg;
    while (!log_closing) {
        LightEvent_Wait(&log_ev);
        svcSleepThread(100000000LL);   /* gather 100 ms of lines per write */
        log_drain();
    }
}

void recomp_log(const char *fmt, ...) {
    va_list ap;
    char line[2048];
    if (!log_fp) return;
    va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    u64 t0 = svcGetSystemTick();
    LightLock_Lock(&lock);
    heartbeat++;
    while (log_head - log_tail + n > LOGBUF) {   /* ring full: wait for the writer */
        LightLock_Unlock(&lock);
        LightEvent_Signal(&log_ev);
        svcSleepThread(1000000LL);
        LightLock_Lock(&lock);
    }
    u32 o = log_head & (LOGBUF - 1), a = LOGBUF - o < (u32)n ? LOGBUF - o : (u32)n;
    memcpy(logbuf + o, line, a);
    memcpy(logbuf, line + a, n - a);
    log_head += n;
    u64 dt = svcGetSystemTick() - t0;
    recomp_prof[PROF_LOG] += dt;
    if (dt > log_worst) log_worst = dt;
    LightLock_Unlock(&lock);
    LightEvent_Signal(&log_ev);
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
            log_drain();
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
    log_drain();
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
/* libctru keeps the handler per thread (TLS): the game thread, and the GX thread */
void recomp_thread_crash_log(void) { threadOnException(on_exception, RUN_HANDLER_ON_FAULTING_STACK, WRITE_DATA_TO_HANDLER_STACK); }

void recomp_log_open(void) {
    LightLock_Init(&lock);
    LightLock_Init(&wlock);
    LightEvent_Init(&log_ev, RESET_ONESHOT);
    log_fp = fopen(LOG_PATH, "w");
    recomp_thread_crash_log();
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    aux_thread(watchdog, 0x2000, prio - 1);
    aux_thread(log_writer, 0x2000, prio + 1);
    recomp_log("# log: helper threads on core %d\n", aux_core);
}

void recomp_log_close(void) {
    finished = 1;
    log_closing = 1;
    log_drain();
    if (log_fp) fclose(log_fp);
    log_fp = NULL;
}
