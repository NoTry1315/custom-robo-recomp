/* GX module, phase 3: no GPU. The draw-done token completes immediately, so
 * the PE draw-done interrupt (callback) is delivered at the next point the
 * CPU would wait for it. Graphics are phase 5. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)

static u32 draw_done_cb;
static int draw_done_pending;

static void deliver_draw_done(void) {
    if (!draw_done_pending) return;
    draw_done_pending = 0;
    if (draw_done_cb) recomp_call_guest(draw_done_cb, 0);
}

void sdk_GXSetDrawDoneCallback(void) { LOG(); u32 old = draw_done_cb; draw_done_cb = R[3]; R[3] = old; }
void sdk_GXWaitDrawDone(void) { LOG(); deliver_draw_done(); }
void sdk_GXDrawDone(void) { LOG(); draw_done_pending = 1; deliver_draw_done(); }
