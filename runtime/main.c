#include <3ds.h>
#include <stdio.h>
#include <setjmp.h>
#include "ppc.h"

void cpu_init(void);
void f_80003154(void); /* __start (DOL entry) */
extern jmp_buf recomp_exit_jmp;
u32 __stacksize__ = 1024 * 1024;  /* translated code nests one host frame per guest call */

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    cpu_init();
    recomp_gx_init();
    recomp_log_open();
    recomp_log("# custom-robo-recomp phase 6: audio\n");
    if (recomp_load_dol() == 0) {
        printf("main.dol loaded, running __start\n");
        if (!setjmp(recomp_exit_jmp)) {
            f_80003154();
            recomp_done("__start returned");
        }
    } else {
        recomp_log("# DONE no DOL\n");
    }
    printf("guest run ended, see sdmc:/cr_autotest.log\n");
    recomp_log_close();
    gfxExit();
    return 0;
}
