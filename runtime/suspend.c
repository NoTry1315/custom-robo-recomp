/* HOME / sleep transitions, traced (2026-10-29: on hardware, pressing HOME in scene 8.3 crashed the whole 3DS:
 * Luma dump = svcBreak(PANIC) in HOME Menu itself, pc 0x136f28, with the 3dsx app's title id on its stack; the
 * game log just stops). The game log is buffered and does not survive a system crash, so every step of the
 * handover goes to sdmc:/cr_suspend.log, file closed after each batch (FS commits it): libctru's own steps
 * through --wrap (GSPGPU_SaveVramSysArea / ReleaseRight / AcquireRight / RestoreVramSysArea, Makefile), the APT
 * and DSP hooks, and what our other threads (GX thread, AX mix worker, DVD reader / prefetcher) are doing. While
 * the game is suspended the writer thread adds a line 0.1, 0.5, 1, 2 and 5 s after the suspend. The steps only copy
 * a line into memory; the writer thread (core 2) does the SD writes: one open/write/close took ~250 ms on hardware
 * (run 60), and writing on the game thread delayed the HOME handover by ~1.2 s.
 * Every 300 retraces "# mem" logs the free memory of each region: HOME Menu allocates from SYSTEM.
 * sdmc:/cr_home_jump.txt "<retrace>" = call aptJumpToHomeMenu at that retrace (the HOME button's path, for a
 * test with nobody at the console; the 3DS then stays in HOME Menu). */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "ppc.h"

volatile int recomp_suspended;   /* between APTHOOK_ONSUSPEND/ONSLEEP and ONRESTORE/ONWAKEUP (log.c's watchdog) */
static aptHookCookie apt_cookie;
static dspHookCookie dsp_cookie;
static u64 t_suspend;
static u32 home_jump_at;
static int writer_ok;
static LightEvent wr_ev;
#define TR_LINES 64
static char tr_buf[TR_LINES][512];   /* lines not yet written: tr_r..tr_w */
static volatile u32 tr_w, tr_r, tr_dropped;
static LightLock tr_lock = 1;        /* 1 = unlocked (LightLock_Init) */

void recomp_ax_suspend_state(char *s, size_t n);
void recomp_gx_suspend_state(char *s, size_t n);
void recomp_dvd_suspend_state(char *s, size_t n);

static void mem_state(char *s, size_t n) {
    snprintf(s, n, "free KB: system %lu app %lu base %lu linear %lu vram %lu", (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) >> 10),
             (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), (unsigned long)(osGetMemRegionFree(MEMREGION_BASE) >> 10),
             (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(vramSpaceFree() >> 10));
}

/* the writer thread (or the caller, without one): append the pending lines, close the file */
static void tr_flush(void) {
    u32 w = tr_w;
    if (tr_r == w) return;
    __dmb();   /* the lines up to w are complete */
    FILE *f = fopen("sdmc:/cr_suspend.log", "a");
    for (; tr_r != w; tr_r++) if (f) fputs(tr_buf[tr_r % TR_LINES], f);
    if (f) { if (tr_dropped) fprintf(f, "(%lu lines dropped: buffer full)\n", (unsigned long)tr_dropped); tr_dropped = 0; fclose(f); }
}

/* one line with the threads' state, queued for the writer thread */
static void trace(const char *what, long res) {
    char ax[96], gx[96], dvd[96], mem[128];
    recomp_ax_suspend_state(ax, sizeof ax);
    recomp_gx_suspend_state(gx, sizeof gx);
    recomp_dvd_suspend_state(dvd, sizeof dvd);
    mem_state(mem, sizeof mem);
    u64 now = svcGetSystemTick();
    LightLock_Lock(&tr_lock);
    if (tr_w - tr_r < TR_LINES) {
        snprintf(tr_buf[tr_w % TR_LINES], sizeof tr_buf[0], "r%lu +%lu ms %s %08lx | %s | %s | %s | %s\n", (unsigned long)recomp_retrace_count(),
                 t_suspend ? (unsigned long)((now - t_suspend) / CPU_TICKS_PER_MSEC) : 0ul, what, (unsigned long)res, ax, gx, dvd, mem);
        __dmb();   /* the line before its index */
        tr_w++;
    } else tr_dropped++;
    LightLock_Unlock(&tr_lock);
    if (writer_ok) LightEvent_Signal(&wr_ev);
    else tr_flush();
}

static void on_apt(APT_HookType hook, void *param) {
    (void)param;
    static const char *const names[] = { "apt ONSUSPEND", "apt ONRESTORE", "apt ONSLEEP", "apt ONWAKEUP", "apt ONEXIT" };
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        t_suspend = svcGetSystemTick();
        recomp_suspended = 1;
    }
    trace(hook < 5 ? names[hook] : "apt ?", hook);
    recomp_log("# suspend r%lu: %s\n", (unsigned long)recomp_retrace_count(), hook < 5 ? names[hook] : "apt ?");
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) { recomp_suspended = 0; t_suspend = 0; }
}

static void on_dsp(DSP_HookType hook) {
    static const char *const names[] = { "dsp ONSLEEP", "dsp ONWAKEUP", "dsp ONCANCEL" };
    trace(hook < 3 ? names[hook] : "dsp ?", hook);
}

Result __real_GSPGPU_SaveVramSysArea(void);
Result __real_GSPGPU_RestoreVramSysArea(void);
Result __real_GSPGPU_ReleaseRight(void);
Result __real_GSPGPU_AcquireRight(u8 flags);
Result __wrap_GSPGPU_SaveVramSysArea(void) {
    trace("gsp SaveVramSysArea ...", 0);
    Result r = __real_GSPGPU_SaveVramSysArea();
    trace("gsp SaveVramSysArea", r);
    return r;
}
Result __wrap_GSPGPU_RestoreVramSysArea(void) {
    Result r = __real_GSPGPU_RestoreVramSysArea();
    trace("gsp RestoreVramSysArea", r);
    return r;
}
Result __wrap_GSPGPU_ReleaseRight(void) {
    trace("gsp ReleaseRight ...", 0);
    Result r = __real_GSPGPU_ReleaseRight();
    trace("gsp ReleaseRight", r);
    return r;
}
Result __wrap_GSPGPU_AcquireRight(u8 flags) {
    Result r = __real_GSPGPU_AcquireRight(flags);
    if (t_suspend) trace("gsp AcquireRight", r);   /* gfxInit's first acquire is not a resume */
    return r;
}

/* writes the queued lines; while suspended also adds 5 lines, so the last line before a system crash brackets when
 * it was */
static void writer(void *arg) {
    (void)arg;
    static const u16 at_ms[] = { 100, 500, 1000, 2000, 5000 };
    u64 seen = 0;
    unsigned next = 0;
    for (;;) {
        LightEvent_WaitTimeout(&wr_ev, recomp_suspended && next < 5 ? 20000000LL : 1000000000LL);
        u64 ts = t_suspend;
        if (ts != seen) { seen = ts; next = 0; }
        if (recomp_suspended && ts && next < 5 && (svcGetSystemTick() - ts) / CPU_TICKS_PER_MSEC >= at_ms[next]) trace("suspended", at_ms[next++]);
        tr_flush();
    }
}

void recomp_suspend_init(void) {
    aptHook(&apt_cookie, on_apt, NULL);
    dspHook(&dsp_cookie, on_dsp);
    LightEvent_Init(&wr_ev, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    writer_ok = recomp_aux_thread(writer, 0x2000, prio - 1) != NULL;
    FILE *f = fopen("sdmc:/cr_home_jump.txt", "rb");
    if (f) {
        unsigned long v = 0;
        if (fscanf(f, "%lu", &v) == 1) home_jump_at = v;
        fclose(f);
        rename("sdmc:/cr_home_jump.txt", "sdmc:/cr_home_jump.txt.done");   /* once: the console cannot be reached after a jump */
    }
    char mem[128];
    mem_state(mem, sizeof mem);
    recomp_log("# suspend: trace to sdmc:/cr_suspend.log on HOME / sleep%s; %s\n", home_jump_at ? " (cr_home_jump: HOME at a set retrace)" : "", mem);
    if (home_jump_at) recomp_log("# suspend: cr_home_jump at retrace %lu\n", (unsigned long)home_jump_at);
}

/* vi.c, every retrace before aptMainLoop */
void recomp_suspend_retrace(u32 retrace) {
    if (retrace % 300 == 0) {
        char mem[128];
        mem_state(mem, sizeof mem);
        recomp_log("# mem r%lu: %s\n", (unsigned long)retrace, mem);
    }
    if (home_jump_at && retrace == home_jump_at) {
        recomp_log("# suspend: cr_home_jump: aptJumpToHomeMenu at retrace %lu\n", (unsigned long)retrace);
        trace("cr_home_jump: aptJumpToHomeMenu", 0);
        aptJumpToHomeMenu();
        trace("cr_home_jump: back from HOME Menu", 0);
    }
}
