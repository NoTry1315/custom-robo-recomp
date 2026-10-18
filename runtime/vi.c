/* VI module: retrace counting and retrace callbacks. Phase 3 has no video
 * output; a retrace happens each time the game waits for one. The run ends
 * after RUN_FRAMES retraces. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define RUN_FRAMES 2400

static u32 retrace, pre_cb, post_cb;

void sdk_VISetPreRetraceCallback(void) { LOG(); u32 old = pre_cb; pre_cb = R[3]; R[3] = old; }
void sdk_VISetPostRetraceCallback(void) { LOG(); u32 old = post_cb; post_cb = R[3]; R[3] = old; }
u32 recomp_retrace_count(void) { return retrace; }
void sdk_VIGetRetraceCount(void) { LOG(); R[3] = retrace; }
void sdk_VIGetTvFormat(void) { LOG(); R[3] = 0; }     /* VI_NTSC, from boot info 0xCC */
void sdk_VIGetDTVStatus(void) { LOG(); R[3] = 0; }    /* no component cable */

/* the retrace interrupt: counter, then pre and post callbacks with the count */
void sdk_VIWaitForRetrace(void) {
    LOG();
    retrace++;
    recomp_prof_frame(retrace);
    recomp_wait_point();
    recomp_ax_service();
    if (pre_cb) recomp_call_guest(pre_cb, retrace);
    if (post_cb) recomp_call_guest(post_cb, retrace);
    if (retrace % 30 == 0) recomp_log("# retrace %lu scene %lx sub %lx flags %lx fade %lx cnt %lx logo %x ld %lx\n", (unsigned long)retrace,
        (unsigned long)rd32(0x80321d88), (unsigned long)rd32(0x80321d98), (unsigned long)rd32(0x80321dc0), (unsigned long)rd32(0x80321dc8),
        (unsigned long)rd32(0x80321db4), rd8(0x8044f3d0), (unsigned long)rd32(0x8044f3d4));   /* scene struct 0x80321d88, logo index, loader flags (see recomp_unknowns.md) */
    if (retrace >= RUN_FRAMES) recomp_done("2400 retraces");
}
