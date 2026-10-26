/* PAD module: controller 1 is the 3DS (buttons, circle pad, C-stick);
 * while SELECT is held the 3DS drives controller 2 instead (P1 vs P2 tests).
 * Port 2 is always connected (idle when not driven), ports 3-4 are empty. PADStatus[4], 12 bytes each: button u16, stick x/y,
 * substick x/y (s8), trigger L/R, analog A/B (u8), err (s8). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include "ppc.h"

#define LOG() RECOMP_LOG()
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

/* Replay: sdmc:/cr_pad_replay.txt holds "# pad r<n> buttons ..." lines copied
 * from a played log (tools/citra/replay.ps1). Each state holds from retrace n
 * until the next line; live 3DS input is added on top (buttons OR'd, a live
 * stick wins when moved). */
typedef struct { u32 r; u16 b; s8 port, sx, sy, cx, cy; } ReplayEv;
static ReplayEv *rp;
static int rp_n = -1, rp_i;
static void replay_load(void) {
    rp_n = 0;
    FILE *f = fopen("sdmc:/cr_pad_replay.txt", "r");
    if (!f) return;
    int cap = 0;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        unsigned long r, b; int p = 1, sx = 0, sy = 0, cx = 0, cy = 0;
        int k = sscanf(line, "# pad r%lu buttons %lx p%d stick %d %d cstick %d %d", &r, &b, &p, &sx, &sy, &cx, &cy);
        if (k < 2) continue;
        if (k == 2 && strstr(line, " p2")) p = 2;   /* older log format */
        if (rp_n == cap) { cap = cap ? cap * 2 : 1024; rp = realloc(rp, cap * sizeof *rp); }
        rp[rp_n++] = (ReplayEv){ r, b, p - 1, sx, sy, cx, cy };
    }
    fclose(f);
    recomp_log("# pad replay: %d events, last at r%lu\n", rp_n, rp_n ? (unsigned long)rp[rp_n - 1].r : 0ul);
}
static void replay_apply(u16 *b, int *port, s8 *sx, s8 *sy, s8 *cx, s8 *cy) {
    if (rp_n < 0) replay_load();
    if (!rp_n) return;
    u32 t = recomp_retrace_count();
    while (rp_i + 1 < rp_n && rp[rp_i + 1].r <= t) rp_i++;
    const ReplayEv *e = &rp[rp_i];
    if (e->r > t) return;
    *b |= e->b;
    if (!*b || e->b) *port = e->port;
    if (!*sx && !*sy) { *sx = e->sx; *sy = e->sy; }
    if (!*cx && !*cy) { *cx = e->cx; *cy = e->cy; }
}

void sdk_PADRead(void) {
    LOG();
    u32 st = R[3];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 12; k += 4) wr32(st + 12 * i + k, 0);
        if (i > 1) wr8(st + 12 * i + 10, PAD_ERR_NO_CONTROLLER);
    }
    static int irrst;
    if (!irrst) irrst = irrstInit() == 0 ? 1 : -1;   /* C-stick: New 3DS */
    hidScanInput();
    u32 held = hidKeysHeld();
    if (hidKeysDown() & KEY_TOUCH) {   /* not a game input: the bottom screen's toggle buttons (runtime/menu.c) */
        touchPosition t; hidTouchRead(&t);
        recomp_menu_touch(t.px, t.py);
    }
    u16 b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (held & map[i].key) b |= map[i].pad;
    b |= script_buttons();
    circlePosition cp, cs;
    hidCircleRead(&cp);
    cs.dx = cs.dy = 0;
    if (irrst > 0) { irrstScanInput(); irrstCstickRead(&cs); }
    int port = held & KEY_SELECT ? 1 : 0;   /* SELECT: this input goes to controller 2 */
    s8 sx = axis(cp.dx), sy = axis(cp.dy), cx = axis(cs.dx), cy = axis(cs.dy);
    replay_apply(&b, &port, &sx, &sy, &cx, &cy);
    st += 12 * port;
    wr16(st, b);
    /* every input change is logged, so a played log replays (cr_pad_replay.txt);
     * sticks in steps of 8 to keep the log small */
    static u16 last_b; static int last_port; static s8 last_q[4];
    s8 q[4] = { sx & ~7, sy & ~7, cx & ~7, cy & ~7 };
    if (b != last_b || port != last_port || memcmp(q, last_q, 4)) {
        recomp_log("# pad r%lu buttons %04x p%d stick %d %d cstick %d %d\n", (unsigned long)recomp_retrace_count(), b, port + 1,
                   sx, sy, cx, cy);
        last_b = b; last_port = port; memcpy(last_q, q, 4);
    }
    wr8(st + 2, (u8)sx); wr8(st + 3, (u8)sy);
    wr8(st + 4, (u8)cx); wr8(st + 5, (u8)cy);
    wr8(st + 6, b & PAD_L ? 0xff : 0); wr8(st + 7, b & PAD_R ? 0xff : 0);
    wr8(st + 8, b & PAD_A ? 0xff : 0); wr8(st + 9, b & PAD_B ? 0xff : 0);
    R[3] = 3u << 30;   /* PAD_CHAN0_BIT | PAD_CHAN1_BIT: mask of connected ports (see recomp_unknowns.md) */
}

/* SI-level port control: nothing to do for a host controller; all return TRUE */
void sdk_PADReset(void) { LOG(); R[3] = 1; }
void sdk_PADRecalibrate(void) { LOG(); R[3] = 1; }
void sdk_PADControlMotor(void) { LOG(); }
