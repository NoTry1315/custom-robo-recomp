/* CARD module: no memory card in either slot (saving is a later phase).
 * CARDProbeEx (0x8014ed5c, named from its body: chan >= 2 -> CARD_RESULT_FATAL_ERROR,
 * EXIProbeEx -1 -> CARD_RESULT_NOCARD) answers NOCARD, as the SDK does with an empty slot. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define CARD_RESULT_NOCARD (-3)
#define CARD_RESULT_FATAL_ERROR (-128)

void sdk_CARDProbeEx(void) { LOG(); R[3] = (u32)(R[3] < 2 ? CARD_RESULT_NOCARD : CARD_RESULT_FATAL_ERROR); }
