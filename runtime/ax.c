/* AX: no mixing yet (audio is phase 6). What the game depends on now is
 * the AX frame callback, the sound driver's 5 ms tick: it runs from the
 * wait point at the DSP frame rate, 200 per second = 10 per 3 retraces
 * at 60 Hz. AXRegisterCallback is 0x80145fb8 (swaps the r13 callback
 * under OSDisableInterrupts, returns the old one). */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)

static u32 ax_cb;

void sdk_AXRegisterCallback(void) { LOG(); u32 old = ax_cb; ax_cb = R[3]; R[3] = old; }

void recomp_ax_service(void) {
    static u32 frames, done;
    if (!ax_cb) return;
    frames++;
    u32 due = frames * 10 / 3;
    for (; done < due; done++) recomp_call_guest(ax_cb, 0);
}
