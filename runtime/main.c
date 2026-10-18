#include <3ds.h>
#include <stdio.h>
#include "ppc.h"

void cpu_init(void);
void f_80003154(void); /* __start (DOL entry) */

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    cpu_init();
    printf("custom-robo-recomp: phase 2 build (no DOL loader yet)\n");
    if (hidKeysHeld() & KEY_SELECT) f_80003154(); /* kept linked; boot is phase 3 */
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
