/* VI module: retrace counting and retrace callbacks. Phase 3 has no video
 * output; a retrace happens each time the game waits for one. The run ends
 * after RUN_FRAMES retraces. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define RUN_FRAMES 120

static u32 retrace, pre_cb, post_cb;

void sdk_VISetPreRetraceCallback(void) { LOG(); u32 old = pre_cb; pre_cb = R[3]; R[3] = old; }
void sdk_VISetPostRetraceCallback(void) { LOG(); u32 old = post_cb; post_cb = R[3]; R[3] = old; }
void sdk_VIGetRetraceCount(void) { LOG(); R[3] = retrace; }
void sdk_VIGetTvFormat(void) { LOG(); R[3] = 0; }     /* VI_NTSC, from boot info 0xCC */
void sdk_VIGetDTVStatus(void) { LOG(); R[3] = 0; }    /* no component cable */

/* the retrace interrupt: counter, then pre and post callbacks with the count */
void sdk_VIWaitForRetrace(void) {
    LOG();
    retrace++;
    if (pre_cb) recomp_call_guest(pre_cb, retrace);
    if (post_cb) recomp_call_guest(post_cb, retrace);
    if (retrace % 30 == 0) recomp_log("# retrace %lu\n", (unsigned long)retrace);
    if (retrace >= RUN_FRAMES) recomp_done("120 retraces");
}
