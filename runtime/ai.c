/* AI: the DSP sample rate, the disc audio stream (DTK) and the AI DMA. The stream is not
 * used on the title (AISetStreamPlayState is never called); its registers are
 * kept so the getters return what was set instead of a stale r3.
 *
 * AI DMA: on the GameCube the AI plays 0x280-byte buffers (5 ms) from RAM and interrupts
 * when one is done; AX's callback there (__AXOutAiCallback 0x80145a00, registered by
 * AXInit) queues the next AX output buffer. AXInit is not run here, so its callback is
 * the starting value; the host AX mixer plays its own mix directly while it is still
 * registered. The THP movie player (seq_movie) puts its own callback in front, which
 * calls AX's and mixes the movie audio into a buffer of its own: then ax.c runs the
 * chain every 5 ms frame and plays the buffer left in the DMA registers (runtime/ax.c). */
#include "ppc.h"

#define LOG() RECOMP_LOG()

static u32 dsp_rate = 1, stream_rate, stream_state, vol_l, vol_r;   /* DSP 32 kHz */
u32 recomp_ai_dma_cb = AX_AI_CALLBACK, recomp_ai_dma_addr, recomp_ai_dma_len;

void sdk_AIRegisterDMACallback(void) { LOG(); u32 old = recomp_ai_dma_cb; recomp_ai_dma_cb = R[3]; R[3] = old; }
/* AIInitDMA(addr r3, len r4): the registers keep addr & 0x03ffffe0 and len / 32 */
void sdk_AIInitDMA(void) { LOG(); recomp_ai_dma_addr = R[3] & 0x03ffffe0u; recomp_ai_dma_len = R[4] & ~31u; }
void sdk_AIStartDMA(void) { LOG(); }      /* AX keeps the DMA running: never stopped */
void sdk_AIGetDMAStartAddr(void) { LOG(); R[3] = recomp_ai_dma_addr; }   /* physical */

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
