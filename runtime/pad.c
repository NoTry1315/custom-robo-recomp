/* PAD module: controller 1 is the 3DS (buttons, circle pad, C-stick),
 * ports 2-4 are empty. PADStatus[4], 12 bytes each: button u16, stick x/y,
 * substick x/y (s8), trigger L/R, analog A/B (u8), err (s8). */
#include <3ds.h>
#include <stdio.h>
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

/* Autotest input: sdmc:/cr_pad_script.txt, lines "<from> <to> <buttons hex>" in
 * retraces; the buttons are held on controller 1 for from <= retrace < to.
 * A line "<n> 0 0" sets the run length to n retraces instead (runtime/vi.c).
 * tools/citra/autotest.ps1 -Pad writes it. */
static struct { u32 from, to; u16 b; } script[128];
static int script_n = -1;
static u16 script_buttons(void) {
    if (script_n < 0) {
        script_n = 0;
        FILE *f = fopen("sdmc:/cr_pad_script.txt", "r");
        if (f) {
            unsigned long a, z, b;
            while (script_n < 128 && fscanf(f, "%lu %lu %lx", &a, &z, &b) == 3) {
                if (z == 0) { recomp_run_frames = a; continue; }   /* "<n> 0 0": run length in retraces */
                script[script_n].from = a; script[script_n].to = z; script[script_n].b = b; script_n++; }
            fclose(f);
            recomp_log("# pad script: %d entries\n", script_n);
        }
    }
    u32 t = recomp_retrace_count();
    u16 b = 0;
    for (int i = 0; i < script_n; i++) if (t >= script[i].from && t < script[i].to) b |= script[i].b;
    return b;
}

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
    b |= script_buttons();
    circlePosition cp, cs;
    hidCircleRead(&cp);
    cs.dx = cs.dy = 0;
    if (irrst > 0) { irrstScanInput(); irrstCstickRead(&cs); }
    wr16(st, b);
    static u16 last_b;   /* hardware runs: shows what was pressed when */
    if (b != last_b) { recomp_log("# pad r%lu buttons %04x\n", (unsigned long)recomp_retrace_count(), b); last_b = b; }
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
