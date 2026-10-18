/* AX: the voice mixer, done on the host the way Dolphin's HLE AX ucode does
 * it, 5 ms per frame (160 samples at 32 kHz), output through ndsp.
 *
 * AXInit is not run, so the SDK's own AXVPB pool and free list (0x8041fa20)
 * stay empty; voices are handed out from guest RAM above the GameCube's
 * 24 MB (0x81800000.., unused by the game). The voice setters are hand-
 * written and store into the AXVPB at the SDK's own offsets (checked against
 * the disassembly, see sdk_functions.md); the AXPB itself starts at +0x138
 * (Dolphin's AXPB field order). The mixer reads it there every frame and
 * writes back what the DSP would: state, current address, ADPCM history,
 * sample-rate fraction, envelope and mixer volumes.
 *
 * The AX frame callback (AXRegisterCallback 0x80145fb8) is the sound
 * driver's tick: 200 per second = 10 per 3 retraces at 60 Hz, from the
 * VIWaitForRetrace wait point. As in the SDK's AX interrupt, each frame first
 * consumes the voices' sync flags and renders them (__AXSyncPBs, DSP), then
 * calls the callback; what it changes is heard from the next frame. synth.c
 * asserts this order (AX_SYNC_USER_VE clear in its tick). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)

#define VPB_BASE 0x81800000u
#define VPB_SIZE 0x200u
#define VPB_COUNT 64
#define FRAME 160                  /* samples per 5 ms frame at 32 kHz */

/* AXVPB offsets */
#define V_PRIO 0x0c
#define V_SYNC 0x1c
#define V_DEPOP 0x20
#define PB 0x138
#define P_SRCSEL (PB + 0x08)
#define P_MIXCTRL (PB + 0x0c)
#define P_STATE (PB + 0x0e)
#define P_TYPE (PB + 0x10)
#define P_MIX (PB + 0x12)          /* vL, dL, vR, dR, auxA L/R, auxB L/R, S.. */
#define P_ITD (PB + 0x36)
#define P_VE (PB + 0x64)           /* volume, delta */
#define P_ADDR (PB + 0x6e)         /* loop flag, format, loop, end, current */
#define P_ADPCM (PB + 0x7e)        /* coefs[16], gain, pred_scale, yn1, yn2 */
#define P_SRC (PB + 0xa6)          /* ratio hi/lo, cur frac, last samples[4] */
#define P_ADPCMLOOP (PB + 0xb4)    /* pred_scale, yn1, yn2 */

static u32 ax_cb;
static u8 vpb_used[VPB_COUNT];

void sdk_AXRegisterCallback(void) { LOG(); u32 old = ax_cb; ax_cb = R[3]; R[3] = old; }

/* ---- voices ---- */

/* one log line per voice event, capped */
static void vlog(const char *what, u32 v, u32 val) {
    static u32 n;
    if (++n > 400) return;
    recomp_log("# AX: r%lu v%d %s %lx cur %lx loop %lx end %lx fmt %x lp %x\n", (unsigned long)recomp_retrace_count(),
               (int)((v - VPB_BASE) / VPB_SIZE), what, (unsigned long)val, (unsigned long)rd32(v + P_ADDR + 12),
               (unsigned long)rd32(v + P_ADDR + 4), (unsigned long)rd32(v + P_ADDR + 8), rd16(v + P_ADDR + 2), rd16(v + P_ADDR));
}

void sdk_AXAcquireVoice(void) {
    LOG();
    for (u32 i = 0; i < VPB_COUNT; i++) {
        if (vpb_used[i]) continue;
        u32 v = VPB_BASE + i * VPB_SIZE;
        vpb_used[i] = 1;
        for (u32 k = 0; k < VPB_SIZE; k += 4) wr32(v + k, 0);
        wr32(v + V_PRIO, R[3]); wr32(v + 0x10, R[4]); wr32(v + 0x14, R[5]); wr32(v + 0x18, i);
        R[3] = v;
        return;
    }
    recomp_log("# AX: out of voices\n");
    R[3] = 0;
}

static int voice_index(u32 v) {
    u32 i = (v - VPB_BASE) / VPB_SIZE;
    return v >= VPB_BASE && i < VPB_COUNT ? (int)i : -1;
}

void sdk_AXFreeVoice(void) {
    LOG();
    int i = voice_index(R[3]);
    if (i < 0) return;
    vlog("free", R[3], 0);
    vpb_used[i] = 0;
    wr16(R[3] + P_STATE, 0);
    wr32(R[3] + V_PRIO, 0);
}

void sdk_AXSetVoicePriority(void) { LOG(); wr32(R[3] + V_PRIO, R[4]); }

static void sync(u32 v, u32 bits) { wr32(v + V_SYNC, rd32(v + V_SYNC) | bits); }
static void copy16(u32 dst, u32 src, u32 n) { for (u32 k = 0; k < n; k += 2) wr16(dst + k, rd16(src + k)); }

static u32 voices_started;

/* type: 0 polyphase 8 tap, 1 polyphase 4 tap, 2 linear, 3/4 none (coef sel) */
void sdk_AXSetVoiceSrcType(void) {
    LOG();
    static const u16 sel[5][2] = { {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2} };
    u32 t = R[4] < 5 ? R[4] : 0;
    wr16(R[3] + P_SRCSEL, sel[t][0]);
    if (t >= 2) wr16(R[3] + P_SRCSEL + 2, sel[t][1]);
    sync(R[3], 0x1);
}

void sdk_AXSetVoiceState(void) {
    LOG();
    vlog("state", R[3], R[4]);
    if (R[4] == 1 && rd16(R[3] + P_STATE) != 1) voices_started++;
    wr16(R[3] + P_STATE, R[4]);
    if (R[4] == 0) wr32(R[3] + V_DEPOP, 1);
    sync(R[3], 0x4);
}

void sdk_AXSetVoiceMix(void) {
    LOG();
    static const u16 bit[18] = { 0x1, 0x9, 0x2, 0xa, 0x10, 0x50, 0x20, 0x60, 0x200, 0xa00, 0x400, 0xc00,
                                 0x1000, 0x3000, 0x4, 0xc, 0x80, 0x180 };
    u32 v = R[3], m = R[4], ctrl = 0;
    for (u32 k = 0; k < 18; k++) {
        u16 x = rd16(m + k * 2);
        wr16(v + P_MIX + k * 2, x);
        if (x) ctrl |= bit[k];
    }
    wr16(v + P_MIXCTRL, ctrl);
    sync(v, 0x12);
}

void sdk_AXSetVoiceItdOn(void) {
    LOG();
    wr16(R[3] + P_ITD, 1);
    for (u32 k = 6; k < 14; k += 2) wr16(R[3] + P_ITD + k, 0);
    wr32(R[3] + V_SYNC, (rd32(R[3] + V_SYNC) & ~0x40u) | 0x20);
}
void sdk_AXSetVoiceItdTarget(void) { LOG(); wr16(R[3] + P_ITD + 10, R[4]); wr16(R[3] + P_ITD + 12, R[5]); sync(R[3], 0x40); }
void sdk_AXSetVoiceVe(void) { LOG(); copy16(R[3] + P_VE, R[4], 4); sync(R[3], 0x200); }
void sdk_AXSetVoiceVeDelta(void) { LOG(); wr16(R[3] + P_VE + 2, R[4]); sync(R[3], 0x400); }

void sdk_AXSetVoiceAddr(void) {
    LOG();
    u32 v = R[3];
    copy16(v + P_ADDR, R[4], 16);
    vlog("addr", v, 0);
    u16 fmt = rd16(v + P_ADDR + 2);
    if (fmt == 0x0a || fmt == 0x19) {         /* PCM16 / PCM8: clear ADPCM block, gain */
        for (u32 k = 0; k < 0x28; k += 2) wr16(v + P_ADPCM + k, 0);
        wr16(v + P_ADPCM + 0x20, fmt == 0x0a ? 0x0800 : 0x0100);
    }
    wr32(v + V_SYNC, (rd32(v + V_SYNC) & ~0x1e000u) | 0x21000);
}
void sdk_AXSetVoiceLoop(void) { LOG(); vlog("loopflag", R[3], R[4]); wr16(R[3] + P_ADDR, R[4]); sync(R[3], 0x2000); }
void sdk_AXSetVoiceLoopAddr(void) { LOG(); vlog("loopaddr", R[3], R[4]); wr16(R[3] + P_ADDR + 4, R[4] >> 16); wr16(R[3] + P_ADDR + 6, R[4]); sync(R[3], 0x4000); }
void sdk_AXSetVoiceEndAddr(void) { LOG(); vlog("endaddr", R[3], R[4]); wr16(R[3] + P_ADDR + 8, R[4] >> 16); wr16(R[3] + P_ADDR + 10, R[4]); sync(R[3], 0x8000); }
void sdk_AXSetVoiceCurrentAddr(void) { LOG(); vlog("curaddr", R[3], R[4]); wr16(R[3] + P_ADDR + 12, R[4] >> 16); wr16(R[3] + P_ADDR + 14, R[4]); sync(R[3], 0x10000); }
void sdk_AXSetVoiceAdpcm(void) { LOG(); copy16(R[3] + P_ADPCM, R[4], 0x28); sync(R[3], 0x20000); }
void sdk_AXSetVoiceSrc(void) { LOG(); copy16(R[3] + P_SRC, R[4], 14); wr32(R[3] + V_SYNC, (rd32(R[3] + V_SYNC) & ~0x80000u) | 0x40000); }
void sdk_AXSetVoiceSrcRatio(void) {
    LOG();
    float f = (float)F[1].ps0 * 65536.0f;
    u32 r = f <= 0 ? 0 : f >= (float)0x40000 ? 0x40000 : (u32)f;
    wr16(R[3] + P_SRC, r >> 16); wr16(R[3] + P_SRC + 2, r);
    sync(R[3], 0x80000);
}
void sdk_AXSetVoiceAdpcmLoop(void) { LOG(); copy16(R[3] + P_ADPCMLOOP, R[4], 6); sync(R[3], 0x100000); }

/* aux effect callbacks: no aux bus mixing (reverb/chorus), see recomp_unknowns.md */
void sdk_AXRegisterAuxACallback(void) { LOG(); }
void sdk_AXRegisterAuxBCallback(void) { LOG(); }

/* ---- mixer ---- */

static s32 mixL[FRAME], mixR[FRAME];
static struct { u32 frames, peak, active_max, ended, looped, queued, dropped; u64 sumsq; } st;

static inline s16 clamp16(s32 x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }

/* one source sample at the current address; advances the address, handles
 * end/loop; returns 0 and stops the voice past a non-looping end */
typedef struct { u32 cur, end, loop; u16 fmt, looping, ps; s16 yn1, yn2; s16 coef[16]; int stopped, is_stream; u32 v; } Src;

static s16 fetch(Src *s) {
    if (s->stopped) return 0;
    s16 out;
    if (s->fmt == 0) {                        /* DSP-ADPCM, nibble addresses */
        if ((s->cur & 15) == 0) { s->ps = recomp_aram_read8(s->cur >> 1); s->cur += 2; }
        u8 b = recomp_aram_read8(s->cur >> 1);
        s32 nib = (s->cur & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        s32 c1 = s->coef[(s->ps >> 4) * 2 & 15], c2 = s->coef[((s->ps >> 4) * 2 + 1) & 15];
        s32 val = ((nib << (s->ps & 15)) << 11) + c1 * s->yn1 + c2 * s->yn2;
        out = clamp16((val + 1024) >> 11);
        s->yn2 = s->yn1; s->yn1 = out;
    } else if (s->fmt == 0x0a) {              /* PCM16, word addresses */
        out = (s16)((recomp_aram_read8(s->cur * 2) << 8) | recomp_aram_read8(s->cur * 2 + 1));
    } else {                                  /* PCM8 */
        out = (s16)((s8)recomp_aram_read8(s->cur) << 8);
    }
    if (s->cur == s->end) {
        if (s->looping) {
            s->cur = s->loop;
            if (s->fmt == 0) {
                s->ps = rd16(s->v + P_ADPCMLOOP);
                if (!s->is_stream) { s->yn1 = rd16(s->v + P_ADPCMLOOP + 2); s->yn2 = rd16(s->v + P_ADPCMLOOP + 4); }
            }
            st.looped++;
        } else {
            s->stopped = 1;
            st.ended++;
            vlog("ended", s->v, s->cur);
        }
    } else s->cur++;
    return out;
}

static void mix_voice(u32 v) {
    Src s;
    s.v = v;
    s.looping = rd16(v + P_ADDR);
    s.fmt = rd16(v + P_ADDR + 2);
    s.loop = rd32(v + P_ADDR + 4);
    s.end = rd32(v + P_ADDR + 8);
    s.cur = rd32(v + P_ADDR + 12);
    for (int k = 0; k < 16; k++) s.coef[k] = rd16(v + P_ADPCM + k * 2);
    s.ps = rd16(v + P_ADPCM + 0x22);
    s.yn1 = rd16(v + P_ADPCM + 0x24);
    s.yn2 = rd16(v + P_ADPCM + 0x26);
    s.is_stream = rd16(v + P_TYPE) != 0;
    s.stopped = 0;

    u32 ratio = rd32(v + P_SRC);
    u32 frac = rd16(v + P_SRC + 4);
    s16 last[4];
    for (int k = 0; k < 4; k++) last[k] = rd16(v + P_SRC + 6 + k * 2);
    s32 ve = rd16(v + P_VE);
    s16 ved = rd16(v + P_VE + 2);
    s32 vl = rd16(v + P_MIX), vr = rd16(v + P_MIX + 4);
    s16 dl = rd16(v + P_MIX + 2), dr = rd16(v + P_MIX + 6);

    for (int i = 0; i < FRAME; i++) {
        frac += ratio;
        while (frac >= 0x10000) {             /* linear interpolation between last[2] and last[3] */
            frac -= 0x10000;
            last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
            last[3] = fetch(&s);
        }
        s32 x = last[2] + (((s32)(last[3] - last[2]) * (s32)(frac >> 1)) >> 15);
        x = (x * ve) >> 15;
        mixL[i] += (x * vl) >> 15;
        mixR[i] += (x * vr) >> 15;
        ve += ved; ve = ve < 0 ? 0 : ve > 0xffff ? 0xffff : ve;
        vl += dl; vr += dr;
        vl = vl < 0 ? 0 : vl > 0xffff ? 0xffff : vl;
        vr = vr < 0 ? 0 : vr > 0xffff ? 0xffff : vr;
    }

    if (s.stopped) wr16(v + P_STATE, 0);
    wr16(v + P_ADDR + 12, s.cur >> 16); wr16(v + P_ADDR + 14, s.cur);
    wr16(v + P_ADPCM + 0x22, s.ps); wr16(v + P_ADPCM + 0x24, s.yn1); wr16(v + P_ADPCM + 0x26, s.yn2);
    wr16(v + P_SRC + 4, frac);
    for (int k = 0; k < 4; k++) wr16(v + P_SRC + 6 + k * 2, last[k]);
    wr16(v + P_VE, ve);
    wr16(v + P_MIX, vl); wr16(v + P_MIX + 4, vr);
}

/* ---- output: ndsp + a WAV dump of the first WAV_SECONDS for checking ---- */

#define NBUF 8
#define BUF_FRAMES 4               /* 20 ms per ndsp buffer */
#define WAV_SECONDS 60
static int ndsp_ok = -1;
static ndspWaveBuf wbuf[NBUF];
static s16 *wdata;
static int wfill, wcur;
static FILE *wav;
static u32 wav_samples;

static void out_init(void) {
    Result r = ndspInit();
    ndsp_ok = R_SUCCEEDED(r);
    recomp_log("# AX: ndspInit %08lx%s\n", (unsigned long)r, ndsp_ok ? "" : " (no sdmc:/3ds/dspfirm.cdc? mixing to the WAV only)");
    if (ndsp_ok) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, 32000.0f);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = { 1.0f, 1.0f };
        ndspChnSetMix(0, mix);
        wdata = linearAlloc(NBUF * BUF_FRAMES * FRAME * 4);
        memset(wbuf, 0, sizeof wbuf);
        for (int i = 0; i < NBUF; i++) {
            wbuf[i].data_vaddr = wdata + i * BUF_FRAMES * FRAME * 2;
            wbuf[i].nsamples = BUF_FRAMES * FRAME;
            wbuf[i].status = NDSP_WBUF_DONE;
        }
    }
    wav = fopen("sdmc:/cr_audio.wav", "wb");
    if (wav) { static u8 hdr[44]; fwrite(hdr, 1, 44, wav); }
}

static void wav_close(void) {
    if (!wav) return;
    u32 bytes = wav_samples * 4;
    u8 h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0\0}\0\0\0\xf4\x01\0\x04\0\x10\0data....";
    u32 riff = bytes + 36;
    memcpy(h + 4, &riff, 4); memcpy(h + 40, &bytes, 4);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav); wav = NULL;
}

static void out_frame(void) {
    s16 pcm[FRAME * 2];
    for (int i = 0; i < FRAME; i++) {
        s16 l = clamp16(mixL[i]), r = clamp16(mixR[i]);
        pcm[i * 2] = l; pcm[i * 2 + 1] = r;
        u32 a = l < 0 ? -l : l; if (a > st.peak) st.peak = a;
        st.sumsq += (s32)l * l;
    }
    if (wav) {
        fwrite(pcm, 2, FRAME * 2, wav);     /* little-endian host = WAV order */
        if ((wav_samples += FRAME) >= 32000u * WAV_SECONDS) wav_close();
    }
    if (ndsp_ok != 1) return;
    ndspWaveBuf *b = &wbuf[wcur];
    if (b->status != NDSP_WBUF_DONE && b->status != NDSP_WBUF_FREE) { st.dropped++; return; }   /* output behind: drop */
    memcpy((s16 *)b->data_vaddr + wfill * FRAME * 2, pcm, sizeof pcm);
    if (++wfill == BUF_FRAMES) {
        DSP_FlushDataCache(b->data_vaddr, BUF_FRAMES * FRAME * 4);
        ndspChnWaveBufAdd(0, b); st.queued++;
        wfill = 0;
        wcur = (wcur + 1) % NBUF;
    }
}

static void ax_frame(void) {
    memset(mixL, 0, sizeof mixL); memset(mixR, 0, sizeof mixR);
    u32 active = 0;
    for (u32 i = 0; i < VPB_COUNT; i++) {
        u32 v = VPB_BASE + i * VPB_SIZE;
        if (!vpb_used[i]) continue;
        wr32(v + V_SYNC, 0);                /* user changes consumed (__AXSyncPBs) */
        if (rd16(v + P_STATE) != 1) continue;
        active++;
        mix_voice(v);
    }
    if (active > st.active_max) st.active_max = active;
    out_frame();
    if (++st.frames % 1000 == 0) {          /* every 5 s of audio = 300 retraces */
        recomp_log("# AX: frame %lu voices started %lu max active %lu ended %lu loops %lu peak %lu ndsp bufs %lu dropped frames %lu rms %lu\n",
                   (unsigned long)st.frames, (unsigned long)voices_started, (unsigned long)st.active_max,
                   (unsigned long)st.ended, (unsigned long)st.looped, (unsigned long)st.peak, (unsigned long)st.queued, (unsigned long)st.dropped,
                   (unsigned long)__builtin_sqrt((double)st.sumsq / (FRAME * 1000)));
        st.active_max = st.peak = st.ended = st.looped = st.queued = st.dropped = 0; st.sumsq = 0;
    }
}

void recomp_ax_service(void) {
    static u32 frames, done;
    if (ndsp_ok < 0) out_init();
    frames++;
    u32 due = frames * 10 / 3;
    for (; done < due; done++) {
        ax_frame();
        if (ax_cb) recomp_call_guest(ax_cb, 0);
    }
}

void recomp_ax_close(void) {
    wav_close();
    if (ndsp_ok == 1) { ndspExit(); ndsp_ok = 0; }
}
