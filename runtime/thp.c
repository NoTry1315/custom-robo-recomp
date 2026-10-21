/* THP movie library. The decoders (THPVideoDecode 0x801aeb40, THPAudioDecode 0x801b60a8 and
 * their helpers) are pure CPU code and are translated from main.dol (recomp.py
 * TRANSLATE_SDK_RANGES). Only THPInit is native: it points the decoders' work buffers at
 * the locked cache, which here is RAM at LC_BASE instead of 0xE0000000 (ppc.h). */
#include <3ds.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)

/* 0x801b6008 THPInit() -> TRUE, as the guest minus OSRegisterVersion: six locked-cache
 * pointers at 0x8044c380 (LC + 0, 0x2000, 0x2800, then 0, 0x2800, 0x3200), GQR2-5 =
 * load/store types 4-7 (u8, u16, s8, s16) unscaled, initialised flag at -0x422c(r13). */
void sdk_THPInit(void) {
    LOG();
    u32 t = 0x8044c280u;
    wr32(t + 0x100, LC_BASE);
    wr32(t + 0x104, LC_BASE + 0x2000);
    wr32(t + 0x108, LC_BASE + 0x2800);
    wr32(t + 0x10c, LC_BASE);
    wr32(t + 0x110, LC_BASE + 0x2800);
    wr32(t + 0x114, LC_BASE + 0x3200);
    for (int i = 2; i <= 5; i++) cpu.gqr[i] = (u32)(i + 2) << 16 | (u32)(i + 2);
    wr32(0x804543c0u - 0x422c, 1);
    R[3] = 1;
}
