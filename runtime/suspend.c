/* HOME / sleep transitions, traced (2026-10-29: on hardware, pressing HOME in scene 8.3 crashed the whole 3DS:
 * Luma dump = svcBreak(PANIC) in HOME Menu itself, pc 0x136f28, with the 3dsx app's title id on its stack; the
 * game log just stops). The game log is buffered and does not survive a system crash, so every step of the
 * handover is appended to sdmc:/cr_suspend.log and the file closed at once (FS commits it): libctru's own steps
 * through --wrap (GSPGPU_SaveVramSysArea / ReleaseRight / AcquireRight / RestoreVramSysArea, Makefile), the APT
 * and DSP hooks, and what our other threads (GX thread, AX mix worker, DVD reader / prefetcher) are doing. While
 * the game is suspended a monitor thread adds a line 0.1, 0.5, 1, 2 and 5 s after the suspend (few SD writes while HOME
 * Menu starts, so the trace barely changes the timing it records).
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
static int monitor_ok;
static LightEvent mon_ev;

void recomp_ax_suspend_state(char *s, size_t n);
void recomp_gx_suspend_state(char *s, size_t n);
void recomp_dvd_suspend_state(char *s, size_t n);

static void mem_state(char *s, size_t n) {
    snprintf(s, n, "free KB: system %lu app %lu base %lu linear %lu vram %lu", (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) >> 10),
             (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), (unsigned long)(osGetMemRegionFree(MEMREGION_BASE) >> 10),
             (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(vramSpaceFree() >> 10));
}

/* one line, file closed at once; the threads' state with it */
static void trace(const char *what, long res) {
    static LightLock lock = 1;   /* 1 = unlocked (LightLock_Init) */
    char ax[96], gx[96], dvd[96], mem[128];
    recomp_ax_suspend_state(ax, sizeof ax);
    recomp_gx_suspend_state(gx, sizeof gx);
    recomp_dvd_suspend_state(dvd, sizeof dvd);
    mem_state(mem, sizeof mem);
    u64 now = svcGetSystemTick();
    LightLock_Lock(&lock);
    FILE *f = fopen("sdmc:/cr_suspend.log", "a");
    if (f) {
        fprintf(f, "r%lu +%lu ms %s %08lx | %s | %s | %s | %s\n", (unsigned long)recomp_retrace_count(),
                t_suspend ? (unsigned long)((now - t_suspend) / CPU_TICKS_PER_MSEC) : 0ul, what, (unsigned long)res, ax, gx, dvd, mem);
        fclose(f);
    }
    LightLock_Unlock(&lock);
}

static void on_apt(APT_HookType hook, void *param) {
    (void)param;
    static const char *const names[] = { "apt ONSUSPEND", "apt ONRESTORE", "apt ONSLEEP", "apt ONWAKEUP", "apt ONEXIT" };
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        t_suspend = svcGetSystemTick();
        recomp_suspended = 1;
        if (monitor_ok) LightEvent_Signal(&mon_ev);
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

/* while suspended: 5 lines, so the last line before a system crash brackets when it was */
static void monitor(void *arg) {
    (void)arg;
    static const u16 at_ms[] = { 100, 500, 1000, 2000, 5000 };
    for (;;) {
        LightEvent_Wait(&mon_ev);
        u16 prev = 0;
        for (unsigned i = 0; i < sizeof at_ms / sizeof at_ms[0] && recomp_suspended; i++) {
            svcSleepThread((s64)(at_ms[i] - prev) * 1000000);
            prev = at_ms[i];
            if (recomp_suspended) trace("suspended", at_ms[i]);
        }
    }
}

void recomp_suspend_init(void) {
    aptHook(&apt_cookie, on_apt, NULL);
    dspHook(&dsp_cookie, on_dsp);
    LightEvent_Init(&mon_ev, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    monitor_ok = recomp_aux_thread(monitor, 0x2000, prio - 1) != NULL;
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
