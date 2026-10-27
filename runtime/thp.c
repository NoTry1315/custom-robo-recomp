/* THP movie library. THPVideoDecode (0x801aeb40) is native: libjpeg-turbo decodes the frame
 * to planar YCbCr and the planes are written as GX I8 tiles, as the translated decoder does
 * (sdmc:/cr_thpnative_off.txt or the bottom-screen button "Native THP" = the translated body,
 * still in gen/ through recomp.py THP_NATIVE). Its helpers and THPAudioDecode (0x801b60a8) are
 * translated from main.dol (recomp.py TRANSLATE_SDK_RANGES). THPInit is native: it points the
 * decoders' work buffers at the locked cache, which here is RAM at LC_BASE instead of
 * 0xE0000000 (ppc.h). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include "ppc.h"

#define LOG() RECOMP_LOG()

void f_801aeb40(void);

/* 0x801b6008 THPInit() -> TRUE, as the guest minus OSRegisterVersion: six locked-cache
 * pointers at 0x8044c380 (LC + 0, 0x2000, 0x2800, then 0, 0x2800, 0x3200), GQR2-5 =
 * load/store types 4-7 (u8, u16, s8, s16) unscaled, initialised flag at -0x422c(r13). */
void sdk_THPInit(void) {
    LOG();
    u32 t = 0x8044c280u;
    wr32(t + 0x100, LC_BASE);
    wr32(t + 0x104, LC_BASE + 0x2000);
    wr32(t + 0x108, LC_BASE + 0x2800);
    wr32(t + 0x10c, LC_BASE);
    wr32(t + 0x110, LC_BASE + 0x2800);
    wr32(t + 0x114, LC_BASE + 0x3200);
    for (int i = 2; i <= 5; i++) cpu.gqr[i] = (u32)(i + 2) << 16 | (u32)(i + 2);
    wr32(0x804543c0u - 0x422c, 1);
    R[3] = 1;
}

int recomp_thpnative_off;   /* sdmc:/cr_thpnative_off.txt, button "Native THP" */
static int thp_verify, thp_fast;
static tjhandle tj;
static u8 *jbuf, *pl[3], *ref[3];
static u32 jcap, plw, plh, jlen, jfile;
static u32 n_dec, n_win, n_fail;
static u64 t_win, t_prep, t_tj;   /* window totals: whole call, restuff + header, libjpeg */

static u8 *G(u32 a) { return ram + (a & RAM_MASK); }

/* Video component size: the caller passes frame + 8 + 4 * ncomp (+ earlier components), and
 * the frame header holds the component sizes right before it. The first k whose size ends the
 * data with EOI (FF D9, then only zero padding) wins (scan data is unstuffed, so FF D9 alone
 * also occurs inside a frame, and the audio size came first); else a 512 KB bound (libjpeg stops after
 * the last MCU anyway). */
static u32 video_size(u32 file) {
    for (u32 k = 1; k <= 16; k++) {
        u32 sz = rd32_ram(file - 4 * k);
        if (sz < 64 || sz > 0x100000) continue;
        const u8 *p = G(file);
        u32 i = sz;
        while (i > sz - 32 && p[i - 1] == 0) i--;   /* zero padding, then EOI */
        if (p[i - 2] == 0xff && p[i - 1] == 0xd9) return i;
    }
    return 0x80000;
}

/* THP scan data has no 0xFF00 byte stuffing (FFmpeg mjpegdec special-cases AV_CODEC_ID_THP):
 * copy the markers, stuff the scan, end with EOI. -> length, 0 if no SOS or DRI used. */
static u32 restuff(u32 file, u32 n) {
    if (jcap < 2 * n + 16) { free(jbuf); jcap = 2 * n + 16; jbuf = malloc(jcap); }
    const u8 *s = G(file);
    u32 i = 0, o = 0;
    if (s[0] != 0xff || s[1] != 0xd8) return 0;
    jbuf[o++] = 0xff; jbuf[o++] = 0xd8; i = 2;
    for (;;) {
        if (i + 4 > n || s[i] != 0xff) return 0;
        u8 m = s[i + 1];
        u32 len = s[i + 2] << 8 | s[i + 3];
        if (m == 0xdd) return 0;   /* restart markers would be ambiguous in unstuffed data */
        memcpy(jbuf + o, s + i, len + 2); o += len + 2; i += len + 2;
        if (m == 0xda) break;
    }
    u32 end = n >= 2 && s[n - 2] == 0xff && s[n - 1] == 0xd9 ? n - 2 : n;
    for (; i < end; i++) {
        u8 b = s[i];
        jbuf[o++] = b;
        if (b == 0xff) jbuf[o++] = 0;
    }
    jbuf[o++] = 0xff; jbuf[o++] = 0xd9;
    return o;
}

/* GX I8: 8x4 texel tiles, row-major */
static void tile_i8(u8 *dst, const u8 *src, u32 w, u32 h) {
    for (u32 ty = 0; ty < h; ty += 4)
        for (u32 tx = 0; tx < w; tx += 8)
            for (u32 y = 0; y < 4; y++) {
                memcpy(dst, src + (ty + y) * w + tx, 8);
                dst += 8;
            }
}

static int native_decode(u32 file, const u32 out[3]) {
    u64 t0 = svcGetSystemTick();
    u32 n = video_size(file), len = restuff(file, n);
    jlen = len; jfile = file;
    if (!len) return -1;
    int w, h, ss, cs;
    if (tjDecompressHeader3(tj, jbuf, len, &w, &h, &ss, &cs) || ss != TJSAMP_420 || (w & 15) || (h & 15))
        return -2;
    if ((u32)w != plw || (u32)h != plh) {
        for (int i = 0; i < 3; i++) { free(pl[i]); free(ref[i]); }
        pl[0] = malloc(w * h); pl[1] = malloc(w * h / 4); pl[2] = malloc(w * h / 4);
        ref[0] = malloc(w * h); ref[1] = malloc(w * h / 4); ref[2] = malloc(w * h / 4);
        plw = w; plh = h;
    }
    int strides[3] = { w, w / 2, w / 2 };
    u64 t1 = svcGetSystemTick();
    t_prep += t1 - t0;
    if (tjDecompressToYUVPlanes(tj, jbuf, len, pl, w, strides, h, thp_fast ? TJFLAG_FASTDCT : 0) &&
        tjGetErrorCode(tj) != TJERR_WARNING)
        return -3;
    t_tj += svcGetSystemTick() - t1;
    for (int i = 0; i < 3; i++) {
        u32 pw = i ? w / 2 : w, ph = i ? h / 2 : h;
        tile_i8(G(out[i]), pl[i], pw, ph);
        recomp_mark_ram(out[i], pw * ph);
    }
    return 0;
}

/* verify: decode translated first, then native over it; per plane mean abs diff, max diff and
 * correlation against the translated output (log), and frame 100 of each to sdmc */
static void verify_cmp(const u32 out[3]) {
    char line[256];
    int bad = 0;
    int o = snprintf(line, sizeof line, "# thp verify r%lu dec %lu", (unsigned long)recomp_retrace_count(), (unsigned long)n_dec);
    for (int i = 0; i < 3; i++) {
        u32 sz = i ? plw * plh / 4 : plw * plh;
        const u8 *a = ref[i], *b = G(out[i]);
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, sad = 0;
        int mx = 0;
        for (u32 j = 0; j < sz; j++) {
            int d = a[j] - b[j];
            if (d < 0) d = -d;
            if (d > mx) mx = d;
            sad += d; sa += a[j]; sb += b[j]; saa += a[j] * a[j]; sbb += b[j] * b[j]; sab += a[j] * b[j];
        }
        double va = saa - sa * sa / sz, vb = sbb - sb * sb / sz, c = sab - sa * sb / sz;
        if (i == 0) bad = sad / sz > 10;
        double r = va > 0 && vb > 0 ? c / __builtin_sqrt(va * vb) : 1;
        o += snprintf(line + o, sizeof line - o, " %c mad %.3f max %d corr %.5f", "YUV"[i], sad / sz, mx, r);
    }
    recomp_log("%s\n", line);
    if (bad) {
        char nm[48];
        snprintf(nm, sizeof nm, "sdmc:/cr_thp_bad_%lu.jpg", (unsigned long)n_dec);
        FILE *f = fopen(nm, "wb");
        if (f) { fwrite(jbuf, 1, jlen, f); fclose(f); }
        snprintf(nm, sizeof nm, "sdmc:/cr_thp_bad_%lu.raw", (unsigned long)n_dec);
        f = fopen(nm, "wb");
        if (f) { fwrite(G(jfile - 0x40), 1, 0x40 + 0x10000, f); fclose(f); }
        recomp_log("# thp verify: decode %lu differs, file %08lx, jpeg %lu bytes dumped\n", (unsigned long)n_dec,
                   (unsigned long)jfile, (unsigned long)jlen);
    }
    if (n_dec == 100 || bad) {
        for (int k = 0; k < 2; k++) {
            char nm[48];
            snprintf(nm, sizeof nm, "sdmc:/cr_thp_%s_%lu.yuv", k ? "native" : "translated", (unsigned long)n_dec);
            FILE *f = fopen(nm, "wb");
            if (!f) continue;
            for (int i = 0; i < 3; i++) fwrite(k ? G(out[i]) : ref[i], 1, i ? plw * plh / 4 : plw * plh, f);
            fclose(f);
        }
        recomp_log("# thp verify: frame 100 dumped (I8 tiles, Y U V, %lux%lu)\n", (unsigned long)plw, (unsigned long)plh);
    }
}

/* 0x801aeb40 THPVideoDecode(file, Y, U, V, work) -> 0 */
void sdk_THPVideoDecode(void) {
    u64 t0 = svcGetSystemTick();
    u32 file = R[3], out[3] = { R[4], R[5], R[6] };
    if (recomp_thpnative_off || !file || !out[0] || !out[1] || !out[2] || n_fail >= 8) {
        f_801aeb40();
    } else {
        if (thp_verify) {
            f_801aeb40();   /* args already read; the native writes over its output */
            if (plw)
                for (int i = 0; i < 3; i++) memcpy(ref[i], G(out[i]), i ? plw * plh / 4 : plw * plh);
        }
        int e = native_decode(file, out);
        if (e) {
            recomp_log("# thp: native decode failed (%d) at %08lx, translated\n", e, (unsigned long)file);
            n_fail++;
            f_801aeb40();
        } else {
            if (thp_verify && n_dec > 0) verify_cmp(out);
            R[3] = 0;
        }
    }
    n_dec++;
    t_win += svcGetSystemTick() - t0;
    if (++n_win == 30) {
        double k = 1000.0 / SYSCLOCK_ARM11 / n_win;
        recomp_log("# thp r%lu: %lu decodes, %s, %.2f ms per decode (restuff %.2f, libjpeg %.2f, tiles + rest %.2f)\n",
                   (unsigned long)recomp_retrace_count(), (unsigned long)n_dec, recomp_thpnative_off ? "translated" : "native",
                   t_win * k, t_prep * k, t_tj * k, (t_win - t_prep - t_tj) * k);
        n_win = 0; t_win = t_prep = t_tj = 0;
    }
}

void recomp_thp_init(void) {
    FILE *f = fopen("sdmc:/cr_thpnative_off.txt", "rb");
    if (f) fclose(f);
    recomp_thpnative_off = f != NULL;
    f = fopen("sdmc:/cr_thpverify.txt", "rb");
    if (f) fclose(f);
    thp_verify = f != NULL;
    f = fopen("sdmc:/cr_thp_islow.txt", "rb");
    if (f) fclose(f);
    thp_fast = f == NULL;   /* FASTDCT by default (hw run 56: 23.6 -> 20.4 ms per decode) */
    tj = tjInitDecompress();
    recomp_log("# thp: THPVideoDecode %s%s%s (sdmc:/cr_thpnative_off.txt)\n", recomp_thpnative_off ? "translated" : "native",
               thp_fast ? ", FASTDCT" : ", ISLOW (cr_thp_islow.txt)", thp_verify ? ", verify" : "");
    recomp_menu_bind("Native THP", &recomp_thpnative_off, 0);
}
