/* PAD module, phase 3: controller 1 connected with nothing pressed, ports
 * 2-4 empty. Real input is phase 4. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define PAD_ERR_NO_CONTROLLER 0xff

/* PADStatus[4], 12 bytes each: button, stick x/y, substick x/y, triggers,
 * analog A/B, err */
void sdk_PADRead(void) {
    LOG();
    u32 st = R[3];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 12; k += 4) wr32(st + 12 * i + k, 0);
        if (i) wr8(st + 12 * i + 10, PAD_ERR_NO_CONTROLLER);
    }
    R[3] = 1u << 31;   /* PAD_CHAN0_BIT: mask of connected ports (see recomp_unknowns.md) */
}
