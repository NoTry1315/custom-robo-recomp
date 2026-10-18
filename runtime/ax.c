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

/* Voice pool. AXInit is not run, so the SDK's own AXVPB pool and free list
 * (0x8041fa20) stay empty; voices are handed out from guest RAM above the
 * GameCube's 24 MB (0x81800000.., unused by the game). AXVPB fields set as
 * AXAcquireVoice does: +0 next, +4 prev, +0xc priority, +0x10 callback,
 * +0x14 user context, +0x18 index. No stealing, no mixing (phase 6). */
#define VPB_BASE 0x81800000u
#define VPB_SIZE 0x200u
#define VPB_COUNT 64
static u8 vpb_used[VPB_COUNT];

void sdk_AXAcquireVoice(void) {
    LOG();
    for (u32 i = 0; i < VPB_COUNT; i++) {
        if (vpb_used[i]) continue;
        u32 v = VPB_BASE + i * VPB_SIZE;
        vpb_used[i] = 1;
        for (u32 k = 0; k < VPB_SIZE; k += 4) wr32(v + k, 0);
        wr32(v + 0xc, R[3]); wr32(v + 0x10, R[4]); wr32(v + 0x14, R[5]); wr32(v + 0x18, i);
        R[3] = v;
        return;
    }
    R[3] = 0;
}

void sdk_AXFreeVoice(void) {
    LOG();
    u32 i = (R[3] - VPB_BASE) / VPB_SIZE;
    if (R[3] >= VPB_BASE && i < VPB_COUNT) vpb_used[i] = 0;
}
