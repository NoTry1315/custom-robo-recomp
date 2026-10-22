#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "ppc.h"
#include "buildinfo.h"   /* RECOMP_BUILD, written by tools/buildinfo.sh every make */

void cpu_init(void);
void recomp_mtx_init(void);
void f_80003154(void); /* __start (DOL entry) */
extern jmp_buf recomp_exit_jmp;
u32 __stacksize__ = 1024 * 1024;  /* translated code nests one host frame per guest call */

/* bottom screen: rows 0-21 scroll (boot info, run end); rows 22-29 are fixed status rows
 * (stats from log.c, then gx.c's toggle line). Game thread only (consoleSelect is not
 * thread-safe). A row is redrawn only when its text changed: the console draws glyphs on the CPU. */
static PrintConsole *con_main, con_status;
static char shown[RECOMP_STATUS_ROWS][40];
void recomp_status_row(int row, const char *s) {
    char t[40];
    if (!con_main || row < 0 || row >= RECOMP_STATUS_ROWS) return;
    snprintf(t, sizeof t, "%-39.39s", s);   /* 39: a 40th character would wrap to the next row (and scroll at the last one) */
    if (!strcmp(t, shown[row])) return;
    strcpy(shown[row], t);
    consoleSelect(&con_status);
    con_status.cursorY = row;   /* window-relative; "\r" sets the column (writing cursorX directly lost each row's first character) */
    printf("\r%s", t);
    consoleSelect(con_main);
}
void recomp_status(const char *s) { recomp_status_row(RECOMP_STATUS_ROWS - 1, s); }

int main(void) {
    gfxInitDefault();
    con_main = consoleInit(GFX_BOTTOM, NULL);
    con_status = *con_main;
    consoleSetWindow(&con_status, 0, 30 - RECOMP_STATUS_ROWS, 40, RECOMP_STATUS_ROWS);
    consoleSetWindow(con_main, 0, 0, 40, 30 - RECOMP_STATUS_ROWS);
    cpu_init();
    recomp_gx_init();
    recomp_log_open();
    recomp_prof_sampler_start();
    recomp_gx_start();
    recomp_log("# custom-robo-recomp phase 6: audio\n");
    /* New 3DS: 804 MHz + L2 cache; without this it runs in old-3DS mode (268 MHz) */
    osSetSpeedupEnable(true);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    recomp_log("# system: %s, speedup requested\n", n3ds ? "New 3DS" : "old 3DS (or emulator reporting old)");
    if (recomp_load_dol() == 0) {
        recomp_mtx_init();
        printf("main.dol loaded, running __start\n");
        printf("build %s\n", RECOMP_BUILD);
        recomp_log("# build %s\n", RECOMP_BUILD);
        /* sdmc flag files present at boot, so a hardware run shows what it tested */
        static const char *const flags[] = { "cr_gxthread_off", "cr_gxthread_fliptest", "cr_batch_off", "cr_dlcache_off", "cr_dlclean_off", "cr_gpukick_off",
            "cr_mtx_off", "cr_vsync_on", "cr_dvdasync_off", "cr_dvd_stalltest", "cr_card_off", "cr_bench", "cr_fbdump", "cr_audio_dump", "cr_audio_rt_off", "cr_audio_rt_on", "cr_pad_script", "cr_pad_replay" };
        int nf = 0;
        printf("flags:");
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++) {
            char p[48]; snprintf(p, sizeof p, "sdmc:/%s.txt", flags[i]);
            FILE *f = fopen(p, "rb");
            if (f) { fclose(f); printf(" %s", flags[i]); nf++; }
        }
        printf(nf ? "\n" : " none\n");
        recomp_ax_pacing_init();   /* "audio real-time|per retrace" line */
        if (!setjmp(recomp_exit_jmp)) {
            f_80003154();
            recomp_done("__start returned");
        }
    } else {
        recomp_log("# DONE no DOL\n");
    }
    printf("guest run ended, see sdmc:/cr_autotest.log\n");
    recomp_gx_stop();   /* the GX thread is done with citro3d before gfxExit */
    recomp_log_close();
    gfxExit();
    return 0;
}
