/* PAD module: controller 1 is the 3DS (buttons, circle pad, C-stick),
 * ports 2-4 are empty. PADStatus[4], 12 bytes each: button u16, stick x/y,
 * substick x/y (s8), trigger L/R, analog A/B (u8), err (s8). */
#include <3ds.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define PAD_ERR_NO_CONTROLLER 0xff

enum { PAD_LEFT = 0x1, PAD_RIGHT = 0x2, PAD_DOWN = 0x4, PAD_UP = 0x8, PAD_Z = 0x10, PAD_R = 0x20,
       PAD_L = 0x40, PAD_A = 0x100, PAD_B = 0x200, PAD_X = 0x400, PAD_Y = 0x800, PAD_START = 0x1000 };

static const struct { u32 key; u16 pad; } map[] = {
    { KEY_DLEFT, PAD_LEFT }, { KEY_DRIGHT, PAD_RIGHT }, { KEY_DDOWN, PAD_DOWN }, { KEY_DUP, PAD_UP },
    { KEY_ZR, PAD_Z }, { KEY_R, PAD_R }, { KEY_L, PAD_L }, { KEY_A, PAD_A }, { KEY_B, PAD_B },
    { KEY_X, PAD_X }, { KEY_Y, PAD_Y }, { KEY_START, PAD_START },
};

/* 3DS circle pad reads about +-155; a GC stick about +-100 from centre */
static s8 axis(int v) { v = v * 100 / 155; return v > 127 ? 127 : v < -128 ? -128 : v; }

void sdk_PADRead(void) {
    LOG();
    u32 st = R[3];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 12; k += 4) wr32(st + 12 * i + k, 0);
        if (i) wr8(st + 12 * i + 10, PAD_ERR_NO_CONTROLLER);
    }
    static int irrst;
    if (!irrst) irrst = irrstInit() == 0 ? 1 : -1;   /* C-stick: New 3DS */
    hidScanInput();
    u32 held = hidKeysHeld();
    u16 b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (held & map[i].key) b |= map[i].pad;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    cs.dx = cs.dy = 0;
    if (irrst > 0) { irrstScanInput(); irrstCstickRead(&cs); }
    wr16(st, b);
    wr8(st + 2, (u8)axis(cp.dx)); wr8(st + 3, (u8)axis(cp.dy));
    wr8(st + 4, (u8)axis(cs.dx)); wr8(st + 5, (u8)axis(cs.dy));
    wr8(st + 6, b & PAD_L ? 0xff : 0); wr8(st + 7, b & PAD_R ? 0xff : 0);
    wr8(st + 8, b & PAD_A ? 0xff : 0); wr8(st + 9, b & PAD_B ? 0xff : 0);
    R[3] = 1u << 31;   /* PAD_CHAN0_BIT: mask of connected ports (see recomp_unknowns.md) */
}

/* SI-level port control: nothing to do for a host controller; all return TRUE */
void sdk_PADReset(void) { LOG(); R[3] = 1; }
void sdk_PADRecalibrate(void) { LOG(); R[3] = 1; }
void sdk_PADControlMotor(void) { LOG(); }
