#include <3ds.h>
#include <stdio.h>
#include <setjmp.h>
#include "ppc.h"
#include "buildinfo.h"   /* RECOMP_BUILD, written by tools/buildinfo.sh every make */

void cpu_init(void);
void recomp_mtx_init(void);
void f_80003154(void); /* __start (DOL entry) */
extern jmp_buf recomp_exit_jmp;
u32 __stacksize__ = 1024 * 1024;  /* translated code nests one host frame per guest call */

/* bottom screen: rows 0-28 scroll (boot info, run end); row 29 is a fixed status line.
 * Game thread only (consoleSelect is not thread-safe). */
static PrintConsole *con_main, con_status;
void recomp_status(const char *s) {
    if (!con_main) return;
    consoleSelect(&con_status);
    printf("\r%-39.39s", s);   /* 39: a 40th character would wrap and scroll the one-row window */
    consoleSelect(con_main);
}

int main(void) {
    gfxInitDefault();
    con_main = consoleInit(GFX_BOTTOM, NULL);
    con_status = *con_main;
    consoleSetWindow(&con_status, 0, 29, 40, 1);
    consoleSetWindow(con_main, 0, 0, 40, 29);
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
            "cr_mtx_off", "cr_vsync_on", "cr_dvdasync_off", "cr_dvd_stalltest", "cr_card_off", "cr_bench", "cr_fbdump", "cr_audio_dump", "cr_pad_script", "cr_pad_replay" };
        int nf = 0;
        printf("flags:");
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++) {
            char p[48]; snprintf(p, sizeof p, "sdmc:/%s.txt", flags[i]);
            FILE *f = fopen(p, "rb");
            if (f) { fclose(f); printf(" %s", flags[i]); nf++; }
        }
        printf(nf ? "\n" : " none\n");
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
