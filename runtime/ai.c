/* AI: the DSP sample rate and the disc audio stream (DTK). The stream is not
 * used on the title (AISetStreamPlayState is never called); its registers are
 * kept so the getters return what was set instead of a stale r3. */
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)

static u32 dsp_rate = 1, stream_rate, stream_state, vol_l, vol_r;   /* DSP 32 kHz */

void sdk_AISetDSPSampleRate(void) { LOG(); dsp_rate = R[3]; }
void sdk_AIGetDSPSampleRate(void) { LOG(); R[3] = dsp_rate; }
void sdk_AISetStreamSampleRate(void) { LOG(); stream_rate = R[3]; }
void sdk_AIGetStreamSampleRate(void) { LOG(); R[3] = stream_rate; }
void sdk_AISetStreamPlayState(void) { LOG(); if (R[3]) recomp_log("# AI: disc stream started, not played\n"); stream_state = R[3]; }
void sdk_AIGetStreamPlayState(void) { LOG(); R[3] = stream_state; }
void sdk_AISetStreamVolLeft(void) { LOG(); vol_l = R[3] & 0xff; }
void sdk_AISetStreamVolRight(void) { LOG(); vol_r = R[3] & 0xff; }
void sdk_AIGetStreamVolLeft(void) { LOG(); R[3] = vol_l; }
void sdk_AIGetStreamVolRight(void) { LOG(); R[3] = vol_r; }
