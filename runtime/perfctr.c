/* ARM11 hardware performance counters per frame (sdmc:/cr_perfctr.txt at boot, or the bottom-screen
 * button "Perf counters", live): the core's CP15 counters through svcControlPerformanceCounter.
 *
 * A core has one cycle counter (CCNT) and two event counters (PMN0, PMN1), so the five event pairs in
 * pairs[] rotate one per frame and each event is averaged over the frames its pair counted. The
 * counters are read and reset at every VIWaitForRetrace (recomp_prof_frame, game thread, core 0),
 * so a frame's counts are core 0 for the whole frame: the game thread plus whatever else runs on
 * core 0 (kernel, libctru service threads). While the game thread waits (gx wait, vblank) the core
 * mostly sleeps; "ccnt MHz" (cycles per wall-clock microsecond) shows whether CCNT runs then:
 * ~804 = it counts every cycle, lower = it stops in WFI and cycles ~ busy time.
 * Frames with a counter overflow are dropped. "# perf r<n>" every 60 retraces, values per frame;
 * the boot line "# perfctr:" has the SVC results (Citra and old kernels may not implement it). */
#include <3ds.h>
#include <stdio.h>
#include "ppc.h"

int recomp_perfctr_on;     /* ppc.h; sdmc:/cr_perfctr.txt, button "Perf counters" */
u32 recomp_perfctr_flips;

static const struct { PerfCounterEvent ev[2]; } pairs[] = {   /* PMN0, PMN1 */
    { { PERFCOUNTEREVT_CORE_INST_EXECUTED, PERFCOUNTEREVT_CORE_INST_CACHE_MISS } },
    { { PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS } },
    { { PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD } },
    { { PERFCOUNTEREVT_CORE_DATA_CACHE_READ, PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED } },
    { { PERFCOUNTEREVT_CORE_INST_MICRO_TLB_MISS, PERFCOUNTEREVT_CORE_MAIN_TLB_MISS } },
};
#define NP (int)(sizeof pairs / sizeof pairs[0])

static int state;            /* 0 off, 1 counting, -1 the kernel refused (button shows --) */
static int cur = -1;         /* pair counting since the last read, -1 none */
static u64 ev[NP][2], evf[NP], cyc, cycf, svc_ticks, win_t0;
static u32 dropped;
static u64 last_ovf;         /* overflow flags of the last dropped frame */

static Result pc(u64 *out, PerfCounterOperation op, u32 a, u64 b) {
    u64 o = 0;
    Result r = svcControlPerformanceCounter(&o, op, a, b);
    if (out) *out = o;
    return r;
}

static void arm(int p) {   /* events of pair p, all three values and overflow flags to 0 */
    pc(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, pairs[p].ev[0]);
    pc(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, pairs[p].ev[1]);
    pc(NULL, PERFCOUNTEROP_RESET, 7, 7);
    cur = p;
}

static void clear(void) {
    for (int p = 0; p < NP; p++) ev[p][0] = ev[p][1] = evf[p] = 0;
    cyc = cycf = svc_ticks = 0; dropped = 0;
    win_t0 = svcGetSystemTick();
}

static void start(void) {
    Result en = pc(NULL, PERFCOUNTEROP_ENABLE, 0, 0);
    Result cc = R_SUCCEEDED(en) ? pc(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_CYCLE_COUNTER, PERFCOUNTEREVT_CORE_CYCLE_COUNT) : -1;
    u64 e0 = 0, e1 = 0, ce = 0;
    if (R_SUCCEEDED(en) && R_SUCCEEDED(cc)) {
        arm(0);
        pc(&e0, PERFCOUNTEROP_GET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, 0);
        pc(&e1, PERFCOUNTEROP_GET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, 0);
        pc(&ce, PERFCOUNTEROP_GET_EVENT, PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0);
    }
    state = R_SUCCEEDED(en) && R_SUCCEEDED(cc) ? 1 : -1;
    recomp_log("# perfctr: enable %08lx, cycle event %08lx; events read back %lx %lx ccnt %lx; %s\n", (unsigned long)en,
               (unsigned long)cc, (unsigned long)e0, (unsigned long)e1, (unsigned long)ce, state > 0 ? "counting" : "not available");
    if (state < 0) { recomp_perfctr_on = 0; return; }
    clear();
}

static void stop(void) {
    if (state > 0) pc(NULL, PERFCOUNTEROP_DISABLE, 0, 0);
    if (state > 0) state = 0;
    cur = -1;
}

static void changed(void) {   /* menu.c, game thread, after the button flipped recomp_perfctr_on */
    recomp_perfctr_flips++;
    if (state < 0) { recomp_perfctr_on = 0; return; }
    if (recomp_perfctr_on) start(); else stop();
}

void recomp_perfctr_init(void) {
    FILE *f = fopen("sdmc:/cr_perfctr.txt", "rb");
    if (f) { fclose(f); recomp_perfctr_on = 1; }
    recomp_menu_bind("Perf counters", &recomp_perfctr_on, changed);
    if (recomp_perfctr_on) start();
    else recomp_log("# perfctr: off (sdmc:/cr_perfctr.txt or the button \"Perf counters\" turn it on)\n");
}

void recomp_perfctr_exit(void) { stop(); }

/* recomp_prof_frame, every retrace (game thread) */
void recomp_perfctr_frame(u32 retrace) {
    if (state <= 0) return;
    u64 t0 = svcGetSystemTick();
    if (cur >= 0) {
        u64 a = 0, b = 0, c = 0, ovf = 0;
        pc(&a, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_0, 0);
        pc(&b, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_1, 0);
        pc(&c, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0);
        pc(&ovf, PERFCOUNTEROP_GET_OVERFLOW_FLAGS, 0, 0);
        if (ovf & 7) { dropped++; last_ovf = ovf; }
        else { ev[cur][0] += a; ev[cur][1] += b; evf[cur]++; cyc += c; cycf++; }
    }
    arm(cur < 0 ? 0 : (cur + 1) % NP);
    svc_ticks += svcGetSystemTick() - t0;
    if (retrace % 60) return;
    if (!cycf) {   /* Citra: the SVC is a stub that returns success and leaves junk in the outputs */
        recomp_log("# perf r%lu: no frame counted (%lu dropped, overflow flags %llx)\n", (unsigned long)retrace,
                   (unsigned long)dropped, (unsigned long long)last_ovf);
        clear();
        return;
    }
    double n = (double)cycf, wall_us = (svcGetSystemTick() - win_t0) * 1e6 / SYSCLOCK_ARM11 / (n + dropped);
    double cy = cyc / n, v[NP][2];
    for (int p = 0; p < NP; p++)
        for (int k = 0; k < 2; k++) v[p][k] = evf[p] ? ev[p][k] / (double)evf[p] : 0;
    double inst = v[0][0];
    /* per frame: cycles and events; per 1000 instructions for the misses; stalls as a share of cycles */
    recomp_log("# perf r%lu: core 0 per frame (%lu frames, %lu dropped): cycles %.0f (ccnt MHz %.0f, wall %.2f ms), inst %.0f (ipc %.2f), "
               "icache miss %.0f (%.2f/k inst), dcache reads %.0f, read miss %.0f (%.2f/k inst), write miss %.0f (%.2f/k inst), "
               "stall inst %.0f (%.1f%%), stall data %.0f (%.1f%%), branch mispredict %.0f (%.2f/k inst), "
               "inst micro-TLB miss %.0f (%.2f/k inst), main TLB miss %.0f (%.2f/k inst); svc %.0f us\n",
               (unsigned long)retrace, (unsigned long)cycf, (unsigned long)dropped, cy, cy / wall_us, wall_us / 1000, inst, cy ? inst / cy : 0,
               v[0][1], inst ? v[0][1] * 1000 / inst : 0, v[3][0], v[1][0], inst ? v[1][0] * 1000 / inst : 0, v[1][1], inst ? v[1][1] * 1000 / inst : 0,
               v[2][0], cy ? v[2][0] * 100 / cy : 0, v[2][1], cy ? v[2][1] * 100 / cy : 0, v[3][1], inst ? v[3][1] * 1000 / inst : 0,
               v[4][0], inst ? v[4][0] * 1000 / inst : 0, v[4][1], inst ? v[4][1] * 1000 / inst : 0,
               svc_ticks * 1e6 / SYSCLOCK_ARM11 / (n + dropped));
    clear();
}
