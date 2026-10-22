/* VI module: retrace counting and retrace callbacks. Phase 3 has no video
 * output; a retrace happens each time the game waits for one. The run ends
 * after RUN_FRAMES retraces (or the pad script's run length) when an autotest
 * input file exists; interactive play (neither file) has no limit. */
#include <3ds.h>
#include <stdio.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define RUN_FRAMES 2400
u32 recomp_run_frames = RUN_FRAMES;   /* a pad script line "<n> 0 0" changes it (runtime/pad.c) */

static u32 retrace, pre_cb, post_cb;
/* gen/functable.c defines it when recomp.py ran with RECOMP_SITEPROF=1: "# site r k n" lines,
 * calls per call site / function entry / return since the last dump (recomp_unknowns.md) */
void recomp_site_dump(u32 retrace) __attribute__((weak));

/* sdmc:/cr_pad_script.txt or cr_pad_replay.txt = an autotest run (autotest.ps1
 * always writes one), which must end by itself: keep RUN_FRAMES or the script's
 * "<n> 0 0". Neither file = the user playing on hardware: no run length. */
static int file_exists(const char *path) { FILE *f = fopen(path, "r"); if (f) fclose(f); return f != 0; }
int recomp_autotest_run(void) {
    static int at = -1;
    if (at < 0) at = file_exists("sdmc:/cr_pad_script.txt") || file_exists("sdmc:/cr_pad_replay.txt");
    return at;
}
static void run_length_init(void) {
    if (!recomp_autotest_run()) {
        recomp_run_frames = 0xffffffff;
        recomp_log("# run length: unlimited (no pad script or replay)\n");
    }
}

void sdk_VISetPreRetraceCallback(void) { LOG(); u32 old = pre_cb; pre_cb = R[3]; R[3] = old; }
void sdk_VISetPostRetraceCallback(void) { LOG(); u32 old = post_cb; post_cb = R[3]; R[3] = old; }
u32 recomp_retrace_count(void) { return retrace; }
void sdk_VIGetRetraceCount(void) { LOG(); R[3] = retrace; }
void sdk_VIGetTvFormat(void) { LOG(); R[3] = 0; }     /* VI_NTSC, from boot info 0xCC */
void sdk_VIGetDTVStatus(void) { LOG(); R[3] = 0; }    /* no component cable */

/* the retrace interrupt: counter, then pre and post callbacks with the count */
void sdk_VIWaitForRetrace(void) {
    LOG();
    if (retrace == 0) run_length_init();
    retrace++;
    recomp_prof_frame(retrace);
    recomp_wait_point();
    recomp_ax_service();
    if (pre_cb) recomp_call_guest(pre_cb, retrace);
    if (post_cb) recomp_call_guest(post_cb, retrace);
    if (retrace % 30 == 0) recomp_log("# retrace %lu scene %lx sub %lx flags %lx fade %lx cnt %lx logo %x ld %lx\n", (unsigned long)retrace,
        (unsigned long)rd32(0x80321d88), (unsigned long)rd32(0x80321d98), (unsigned long)rd32(0x80321dc0), (unsigned long)rd32(0x80321dc8),
        (unsigned long)rd32(0x80321db4), rd8(0x8044f3d0), (unsigned long)rd32(0x8044f3d4));   /* scene struct 0x80321d88, logo index, loader flags (see recomp_unknowns.md) */
    if (recomp_site_dump && retrace % 300 == 0) recomp_site_dump(retrace);   /* RECOMP_SITEPROF=1 gen/ only */
    if (retrace >= recomp_run_frames) recomp_done("run length reached");
    /* HOME / power / sleep: APT suspends us inside aptMainLoop, false = close requested.
     * citro3d's APT hooks run in there: the GX thread must be idle (it normally is,
     * GXDrawDone waited for it). A bottom-screen tap's GX thread on/off is applied here too. */
    recomp_gx_retrace();
    if (!aptMainLoop()) recomp_done("APT exit");
}
