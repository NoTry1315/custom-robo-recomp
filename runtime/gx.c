/* GX module -> PICA200 (citro3d).
 *
 * GX state is kept host-side, set by the hand-written GX calls below. Vertex
 * data reaches us as a byte stream: GXBegin pushes the draw command (opcode,
 * vertex count) and the game then writes the vertices to the write-gather
 * pipe 0xCC008000 itself (inline GXPosition* etc.), see gx_fifo_write. A
 * primitive is decoded and drawn as soon as its last byte arrives.
 *
 * Vertices are decoded on the CPU (Dolphin's vertex loader semantics) into
 * untransformed PICA vertices; the GX transform, COLOR0/ALPHA0 lighting and
 * texgen run in the PICA vertex shader (vshader.v.pica) from per-draw
 * uniforms, which also map the EFB to the 400x240 top screen. TEV stages are translated to PICA texenv stages; what the
 * PICA cannot express is logged once (docs/recomp_unknowns.md, "GX"). */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include "ppc.h"
#include "vshader_shbin.h"

#define LOG() recomp_sdk_log(__func__ + 4)
/* unemulated GX features: the first hit is logged with its frame, and every
 * site's hit count goes into the "# gx gaps" line with the frame stats */
#define GAP_SITES 64
static struct { const char *fmt; u32 n; } gap[GAP_SITES];
static int gap_n;
static u32 frames;
u32 recomp_gx_dbg[3];
#define ONCE(...) do { static int id_ = -1; \
    if (id_ < 0) { id_ = gap_n < GAP_SITES ? gap_n++ : GAP_SITES - 1; gap[id_].fmt = #__VA_ARGS__; \
        recomp_log("# gx: frame %lu: ", (unsigned long)frames); recomp_log(__VA_ARGS__); } \
    gap[id_].n++; } while (0)
static void gap_dump(void) {
    if (!gap_n) return;
    recomp_log("# gx gaps (hits since start):");
    for (int i = 0; i < gap_n; i++) recomp_log(" [%lu] %.40s;", (unsigned long)gap[i].n, gap[i].fmt + 1);
    recomp_log("\n");
}
#define GUEST(a) (((a) & RAM_MASK) | 0x80000000u)

/* ---------------------------------------------------------------- state */
enum { A_PNMTX = 0, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_MAX = 26 };

typedef struct { u8 cnt, type, frac; } VtxFmt;
typedef struct { u8 a, b, c, d, op, bias, scale, clamp, out; } TevSide;
typedef struct { TevSide c, a; u8 coord, map, chan, ksel_c, ksel_a, ras_swap, tex_swap; } TevStage;
typedef struct { u8 enable, amb_src, mat_src, attn_fn, diff_fn; u8 light_mask; } Chan;
typedef struct { float pos[3], dir[3], a[3], k[3]; u8 color[4]; } Light;
typedef struct { u8 func, src, normalize; u32 mtx; } TexGen;
typedef struct { u32 img; u16 w, h; u8 fmt, wrap_s, wrap_t, min_f, mag_f, ci; u32 tlut; } TexMap;
typedef struct { u32 addr; u8 fmt; u16 n; } Tlut;

static struct {
    u8 desc[A_MAX];
    VtxFmt vat[8][A_MAX];
    u32 array_base[12], array_stride[12];
    float xf[0x600];           /* XF matrix memory: pos/tex 0x000, normal 0x400 */
    u32 cur_mtx, tex_mtx_idx[8];
    float proj[6]; u32 proj_ortho;
    float vp[6];
    s32 sc[4], sc_off[2];
    u32 n_texgens, n_chans, n_stages;
    TexGen texgen[8];
    TevStage tev[16];
    u8 tev_reg[4][4], kcolor[4][4];  /* PREV, REG0..2 / K0..3 (RGBA, 0..255) */
    Chan chan[4];              /* COLOR0, COLOR1, ALPHA0, ALPHA1 */
    u8 mat[2][4], amb[2][4];
    Light light[8];
    TexMap map[8];
    Tlut tlut[20];
    u8 blend_type, blend_src, blend_dst, logic_op;
    u8 z_enable, z_func, z_update, color_update, alpha_update;
    u8 cull, alpha_cmp[5];
    u8 clear_color[4]; u32 clear_z;
    u16 efb_w, efb_h;
} gx;

/* ---------------------------------------------------------------- PICA side */
#define VB_VERTS 0x6000      /* per frame half: immediate draws and display-list draws the vertex cache does not take; u16 indices are relative to vb_base (rebased) */
#define IB_INDICES 0x48000   /* per frame half */
#define VC_VERTS 0x14000     /* vertex cache arena (5.4 MB) */
typedef struct { float pos[4]; u8 mat[4]; float nrm[3], tc[3][3]; } PVtx;   /* vshader.v.pica inputs */

static C3D_RenderTarget *target;
static DVLB_s *vsh_dvlb;
static shaderProgram_s vsh;
static PVtx *vbuf[2];
static u16 *ibuf[2];
static u32 vb_used, ib_used, vb_half;
/* display-list vertex cache, see vc_draw */
static PVtx *vc_arena;
static u32 vc_top, vc_gen = 1, vc_full, vc_dlo = 0xffffffffu, vc_dhi, vc_hits, vc_misses;
static u8 mat_vtx[4], mat_reg[4];   /* material colour per component: vertex CLR0 or register */
static float ushadow[96][4];        /* last value written per float uniform */
static int u_clip, u_pos, u_nrm, u_tex, u_amb, u_lcfg, u_lights, u_blit, u_iloop;
static int frame_open, clear_pending = 1;

/* Draw batching: a draw's indices are appended to the pending C3D_DrawElements
 * while no PICA state changed since the previous draw (same vertex buffer,
 * indices contiguous in ibuf). Every citro3d state call is preceded by
 * pend_flush(), since citro3d applies state when the draw is issued.
 * sdmc:/cr_batch_off.txt = one C3D_DrawElements per draw (draws are still
 * counted as mergeable). */
static u16 *pend_ix;
static u16 *last_end;   /* end of the previous draw's indices; NULL after a state change */
static u32 pend_n, st_merged, batch_off = 2;
static void pend_issue(void) {
    if (!pend_n) return;
    C3D_DrawElements(GPU_TRIANGLES, pend_n, C3D_UNSIGNED_SHORT, pend_ix);
    pend_n = 0;
}
static void pend_flush(void) { last_end = NULL; pend_issue(); }

void recomp_gx_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8);   /* 2 MB: battle frames need over 1 MB (~112 words per draw) */
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    vsh_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&vsh);
    shaderProgramSetVsh(&vsh, &vsh_dvlb->DVLE[0]);
    C3D_BindProgram(&vsh);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 5, GPU_FLOAT, 3);
    vbuf[0] = linearAlloc(sizeof(PVtx) * VB_VERTS * 2);
    vc_arena = linearAlloc(sizeof(PVtx) * VC_VERTS);   /* NULL: no vertex cache */
    if (!vc_arena) recomp_log("# gx: no linear memory for the vertex cache\n");
    vbuf[1] = vbuf[0] + VB_VERTS;
    ibuf[0] = linearAlloc(2 * IB_INDICES * 2);
    if (!vbuf[0] || !ibuf[0]) { recomp_log("# gx: linearAlloc of vertex/index buffers failed (linear free %lu)\n",(unsigned long)linearSpaceFree()); abort(); }
    ibuf[1] = ibuf[0] + IB_INDICES;
    shaderInstance_s *si = vsh.vertexShader;
    u_clip = shaderInstanceGetUniformLocation(si, "clipm"); u_pos = shaderInstanceGetUniformLocation(si, "posm");
    u_nrm = shaderInstanceGetUniformLocation(si, "nrmm"); u_tex = shaderInstanceGetUniformLocation(si, "texm");
    u_amb = shaderInstanceGetUniformLocation(si, "amb"); u_lcfg = shaderInstanceGetUniformLocation(si, "lcfg");
    u_lights = shaderInstanceGetUniformLocation(si, "lights"); u_blit = shaderInstanceGetUniformLocation(si, "blit");
    u_iloop = shaderInstanceGetUniformLocation(si, "iloop");
    memset(ushadow, 0xff, sizeof ushadow);
    gx.efb_w = 640; gx.efb_h = 480;
    gx.color_update = gx.alpha_update = 1;
    gx.z_enable = 1; gx.z_func = 3; gx.z_update = 1;
    gx.clear_z = 0xffffff;
    gx.n_chans = 1; gx.n_stages = 1;
    for (int i = 0; i < 4; i++) gx.mat[i & 1][i >> 1] = 255;
    for (int i = 0; i < 16; i++) { gx.tev[i].coord = gx.tev[i].map = 0xff; gx.tev[i].chan = 4; }
    gx.alpha_cmp[0] = gx.alpha_cmp[3] = 7;
    for (int i = 0; i < 3; i++) gx.xf[i * 5] = 1;   /* PNMTX0 = identity */
}

/* EFB -> screen: the EFB copy source scaled to fit 400x240, centered */
static float efb_scale(void) {
    float sx = 400.0f / gx.efb_w, sy = 240.0f / gx.efb_h;
    return sx < sy ? sx : sy;
}
static float efb_x0(void) { return (400 - gx.efb_w * efb_scale()) * 0.5f; }
static float efb_y0(void) { return (240 - gx.efb_h * efb_scale()) * 0.5f; }

/* u16 indices reach 0x10000 vertices: past that, later draws index from a new base */
static u32 vb_base;
static const PVtx *cur_buf;   /* PICA attribute buffer: vbuf + vb_base or a vertex cache window */
static void use_buf(const PVtx *p) {
    if (p == cur_buf) return;
    pend_flush();
    cur_buf = p;
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, p, sizeof(PVtx), 6, 0x543210);
}
static void vc_frame_begin(void);

static void frame_begin(void) {
    if (frame_open) return;
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) recomp_log("# gx: C3D_FrameBegin failed at frame %lu\n", (unsigned long)frames);
    if (!C3D_FrameDrawOn(target)) recomp_log("# gx: C3D_FrameDrawOn failed at frame %lu\n", (unsigned long)frames);
    frame_open = 1;
    vb_half = frames & 1; vb_used = ib_used = 0;
    vb_base = 0; cur_buf = NULL;
    use_buf(vbuf[vb_half]);
    vc_frame_begin();
    if (clear_pending) {
        u32 c = gx.clear_color[0] << 24 | gx.clear_color[1] << 16 | gx.clear_color[2] << 8 | gx.clear_color[3];
        pend_flush();
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, c, gx.clear_z);
        clear_pending = 0;
    }
}

static u32 st_draws, st_tris, st_tex, st_evict, st_upload, st_upbytes, st_split;

static int in_draw;
static void frame_end(void) {
    if (in_draw) recomp_log("# gx: frame ended inside a draw at frame %lu (guest lr %08lx)\n", (unsigned long)frames, (unsigned long)cpu.lr);
    frame_begin();
    if (frames % 300 == 0) {   /* draw stats every 300 copies */
        recomp_log("# gx frame %lu: draws %lu tris %lu textured %lu evicted %lu uploads %lu (%lu KB) efb %dx%d clear %02x%02x%02x%02x cmdbuf-full skips %lu, cmdbuf %d%%\n", (unsigned long)frames,
            (unsigned long)st_draws, (unsigned long)st_tris, (unsigned long)st_tex, (unsigned long)st_evict, (unsigned long)st_upload, (unsigned long)(st_upbytes >> 10), gx.efb_w, gx.efb_h,
            gx.clear_color[0], gx.clear_color[1], gx.clear_color[2], gx.clear_color[3],
            (unsigned long)st_split, (int)(C3D_GetCmdBufUsage() * 100));
        recomp_log("# gx batch frame %lu: draws %lu mergeable %lu (%lu%%) -> %lu DrawElements%s\n", (unsigned long)frames, (unsigned long)st_draws,
            (unsigned long)st_merged, st_draws ? (unsigned long)(st_merged * 100 / st_draws) : 0ul,
            (unsigned long)(batch_off ? st_draws : st_draws - st_merged), batch_off ? " (batching off)" : "");
    }
    if (frames % 300 == 0) {
        gap_dump();
        recomp_log("# gx vcache frame %lu: hits %lu misses %lu (draws), arena %lu KB, linear free %lu KB\n", (unsigned long)frames,
                   (unsigned long)vc_hits, (unsigned long)vc_misses, (unsigned long)(vc_top * sizeof(PVtx) >> 10), (unsigned long)(linearSpaceFree() >> 10));
    }
    vc_hits = vc_misses = 0;
    st_draws = st_tris = st_tex = st_evict = st_upload = st_upbytes = st_split = st_merged = 0;
    /* a frame with no draws still shows the clear color (frame_begin above) */
    pend_flush();
    last_end = NULL;
    u64 t0 = recomp_ticks();
    /* one flush per frame: the GPU reads the buffers only at C3D_FrameEnd */
    if (vb_used) GSPGPU_FlushDataCache(vbuf[vb_half], vb_used * sizeof(PVtx));
    if (ib_used) GSPGPU_FlushDataCache(ibuf[vb_half], ib_used * 2);
    if (vc_dhi > vc_dlo) GSPGPU_FlushDataCache(vc_arena + vc_dlo, (vc_dhi - vc_dlo) * sizeof(PVtx));
    vc_dlo = 0xffffffffu; vc_dhi = 0;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);   /* buffers + textures flushed above; skips the whole-linear-heap flush */
    recomp_prof[PROF_GX_END] += recomp_ticks() - t0;
    frame_open = 0;
    frames++;
}

/* ---------------------------------------------------------------- textures */
static const u8 tile_w[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
static const u8 tile_h[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
static const u8 tex_bpp[16] = {4, 8, 8, 16, 16, 16, 32, 0, 4, 8, 16, 0, 0, 0, 4, 0};

static u32 rgb565(u16 v) {
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xffu << 24;
}
static u32 rgb5a3(u16 v) {
    if (v & 0x8000) {
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return (r * 255 / 31) | (g * 255 / 31) << 8 | (b * 255 / 31) << 16 | 0xffu << 24;
    }
    return ((v >> 8) & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | (v & 15) * 17 << 16 | ((v >> 12) & 7) * 255 / 7 << 24;
}
static u32 ia8(u16 v) { u32 i = v & 0xff; return i | i << 8 | i << 16 | (u32)(v >> 8) << 24; }
static u32 pal_color(const Tlut *t, u32 i) {
    if (i >= t->n) i = t->n ? t->n - 1 : 0;
    u16 v = rd16(t->addr + i * 2);
    return t->fmt == 0 ? ia8(v) : t->fmt == 1 ? rgb565(v) : rgb5a3(v);
}

/* decode a GX texture into w*h RGBA (byte order R,G,B,A in a little-endian u32) */
static void tex_decode(u32 *out, u32 src, int w, int h, int fmt, const Tlut *tl) {
    int tw = tile_w[fmt], th = tile_h[fmt];
    int bw = (w + tw - 1) / tw, bh = (h + th - 1) / th;
    const u8 *p = ram + (src & RAM_MASK);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        if (fmt == 14) {   /* CMPR: 2x2 DXT1 sub-blocks per 8x8 tile */
            for (int s = 0; s < 4; s++, p += 8) {
                u16 c0 = p[0] << 8 | p[1], c1 = p[2] << 8 | p[3];
                u32 pal[4]; pal[0] = rgb565(c0); pal[1] = rgb565(c1);
                for (int k = 0; k < 3; k++) {
                    u32 a = (pal[0] >> (8 * k)) & 255, b = (pal[1] >> (8 * k)) & 255;
                    u32 m2 = c0 > c1 ? (2 * a + b) / 3 : (a + b) / 2, m3 = c0 > c1 ? (a + 2 * b) / 3 : 0;
                    if (!k) { pal[2] = 0; pal[3] = 0; }
                    pal[2] |= m2 << (8 * k); pal[3] |= m3 << (8 * k);
                }
                pal[2] |= 0xffu << 24; if (c0 > c1) pal[3] |= 0xffu << 24;
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                    int px = bx * 8 + (s & 1) * 4 + x, py = by * 8 + (s >> 1) * 4 + y;
                    if (px < w && py < h) out[py * w + px] = pal[(p[4 + y] >> (6 - 2 * x)) & 3];
                }
            }
            continue;
        }
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
            int px = bx * tw + x, py = by * th + y, i = y * tw + x;
            u32 c;
            switch (fmt) {
            case 0: { u32 v = (p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15; v *= 17; c = v | v << 8 | v << 16 | v << 24; break; }
            case 1: { u32 v = p[i]; c = v | v << 8 | v << 16 | v << 24; break; }
            case 2: { u32 v = (p[i] & 15) * 17, a = (p[i] >> 4) * 17; c = v | v << 8 | v << 16 | a << 24; break; }
            case 3: c = ia8(p[2 * i] << 8 | p[2 * i + 1]); break;
            case 4: c = rgb565(p[2 * i] << 8 | p[2 * i + 1]); break;
            case 5: c = rgb5a3(p[2 * i] << 8 | p[2 * i + 1]); break;
            case 6: c = p[2 * i + 1] | p[32 + 2 * i] << 8 | p[32 + 2 * i + 1] << 16 | (u32)p[2 * i] << 24; break;
            case 8: c = pal_color(tl, (p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15); break;
            case 9: c = pal_color(tl, p[i]); break;
            case 10: c = pal_color(tl, (p[2 * i] << 8 | p[2 * i + 1]) & 0x3fff); break;
            default: c = 0xffff00ff; break;
            }
            if (px < w && py < h) out[py * w + px] = c;
        }
        p += tw * th * tex_bpp[fmt] / 8;   /* RGBA8: 64-byte tile, AR block then GB block */
    }
}

static u32 pow2(u32 v) { u32 p = 8; while (p < v && p < 1024) p <<= 1; return p; }

typedef struct { u32 img, tlut_addr, hash, last, pkey; u16 w, h; u8 fmt, tlut_fmt, used; C3D_Tex tex; } TexEntry;
#define TEX_CACHE 192
static TexEntry tcache[TEX_CACHE];
static u32 *decode_buf;

static u32 hash_bytes(u32 addr, u32 n) {
    const u8 *p = ram + (addr & RAM_MASK);
    u32 h = 2166136261u;
    for (u32 i = 0; i + 4 <= n; i += 4) { u32 v; memcpy(&v, p + i, 4); h = (h ^ v) * 16777619u; }
    return h;
}

static u32 tex_bytes(int w, int h, int fmt) {
    int tw = tile_w[fmt], th = tile_h[fmt];
    if (!tw) return 0;
    return ((w + tw - 1) / tw * tw) * ((h + th - 1) / th * th) * tex_bpp[fmt] / 8;
}

static C3D_Tex *tex_bind_params(TexEntry *e, const TexMap *m, float *su, float *sv);

/* PICA format holding a GX format exactly: I4/IA4 -> LA4, I8/IA8 -> LA8 (GX
 * intensity also feeds alpha), RGB565 -> RGB565; the rest RGBA8 */
static GPU_TEXCOLOR pica_fmt(int fmt) {
    switch (fmt) {
    case 0: case 2: return GPU_LA4;
    case 1: case 3: return GPU_LA8;
    case 4: return GPU_RGB565;
    default: return GPU_RGBA8;
    }
}

static const u8 morton_x[8] = {0, 1, 4, 5, 16, 17, 20, 21}, morton_y[8] = {0, 2, 8, 10, 32, 34, 40, 42};
static u32 col_src[1024], col_dst[1024];

/* GX texture -> W x H PICA tiled texture; GX row y is PICA memory row y,
 * sampled at v = 1 - y/H (see set_texgen); padding repeats the edge. The
 * compact formats are converted straight from the GX tiles. */
static void tex_upload(TexEntry *e, const TexMap *m, u32 W, u32 H, GPU_TEXCOLOR pf, const Tlut *tl) {
    int w = m->w > 1024 ? 1024 : m->w, h = m->h > 1024 ? 1024 : m->h, fmt = m->fmt;
    if (pf != GPU_RGBA8) {
        int tw = tile_w[fmt], th = tile_h[fmt], tn = tw * th, bw = (w + tw - 1) / tw;
        const u8 *src = ram + (m->img & RAM_MASK);
        for (u32 x = 0; x < W; x++) {   /* texel index of column x inside its GX tile row */
            u32 sx = x < (u32)w ? x : w - 1;
            col_src[x] = sx / tw * tn + sx % tw;
            col_dst[x] = (x >> 3) * 64 + morton_x[x & 7];
        }
        for (u32 y = 0; y < H; y++) {
            u32 sy = y < (u32)h ? y : h - 1;
            u32 rs = sy / th * bw * tn + sy % th * tw, rd = (y >> 3) * (W >> 3) * 64 + morton_y[y & 7];
            switch (fmt) {
            case 0: { u8 *d = e->tex.data; for (u32 x = 0; x < W; x++) { u32 i = rs + col_src[x], v = (src[i >> 1] >> ((i & 1) ? 0 : 4)) & 15; d[rd + col_dst[x]] = (u8)(v << 4 | v); } break; }
            case 2: { u8 *d = e->tex.data; for (u32 x = 0; x < W; x++) { u8 v = src[rs + col_src[x]]; d[rd + col_dst[x]] = (u8)(v << 4 | v >> 4); } break; }
            case 1: { u16 *d = e->tex.data; for (u32 x = 0; x < W; x++) { u16 v = src[rs + col_src[x]]; d[rd + col_dst[x]] = v << 8 | v; } break; }
            case 3: { u16 *d = e->tex.data; for (u32 x = 0; x < W; x++) { const u8 *p = src + 2 * (rs + col_src[x]); d[rd + col_dst[x]] = (u16)(p[1] << 8 | p[0]); } break; }
            default: { u16 *d = e->tex.data; for (u32 x = 0; x < W; x++) { const u8 *p = src + 2 * (rs + col_src[x]); d[rd + col_dst[x]] = (u16)(p[0] << 8 | p[1]); } break; }
            }
        }
    } else {
        if (!decode_buf) decode_buf = malloc(1024 * 1024 * 4);
        tex_decode(decode_buf, m->img, w, h, fmt, tl);
        u32 *d = e->tex.data;
        for (u32 x = 0; x < W; x++) col_dst[x] = (x >> 3) * 64 + morton_x[x & 7];
        for (u32 y = 0; y < H; y++) {
            const u32 *row = decode_buf + (y < (u32)h ? y : h - 1) * w;
            u32 rd = (y >> 3) * (W >> 3) * 64 + morton_y[y & 7];
            for (u32 x = 0; x < W; x++) {
                u32 c = row[x < (u32)w ? x : w - 1];
                d[rd + col_dst[x]] = __builtin_bswap32(c);   /* R,G,B,A bytes -> PICA A,B,G,R */
            }
        }
    }
    GSPGPU_FlushDataCache(e->tex.data, e->tex.size);
    st_upload++; st_upbytes += e->tex.size;
}

/* GXCopyTex destinations: EFB -> texture copies are not emulated, so RAM there
 * holds stale data; such textures are not drawn (were 600x440 per-frame uploads
 * of garbage in the campaign) */
static u32 copy_dst[8];

/* a texmap that draws without its texture (TEXC then reads white): each
 * distinct image is logged once, up to 48 */
static void tex_miss(const char *why, const TexMap *m) {
    static u32 seen[48];
    static int n;
    u32 key = GUEST(m->img) ^ (u32)m->fmt << 28 ^ (u32)m->w << 16 ^ m->h;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n == 48) return;
    seen[n++] = key;
    recomp_log("# gx tex miss: %s img %08lx fmt %d %dx%d ci %d frame %lu\n", why, (unsigned long)GUEST(m->img),
               m->fmt, m->w, m->h, m->ci, (unsigned long)frames);
}

static C3D_Tex *tex_get(const TexMap *m, float *su, float *sv) {
    if (!m->img) { tex_miss("no image", m); return NULL; }
    if (!tile_w[m->fmt & 15]) { tex_miss("format not decoded", m); return NULL; }
    if (!m->w || !m->h) { tex_miss("zero size", m); return NULL; }
    for (int i = 0; i < 8; i++) if (copy_dst[i] && copy_dst[i] == GUEST(m->img)) { ONCE("EFB copy texture not drawn\n"); tex_miss("EFB copy destination", m); return NULL; }
    const Tlut *tl = m->ci ? &gx.tlut[m->tlut < 20 ? m->tlut : 0] : NULL;
    /* hashed once per frame: the title draws one big texture as many strips.
     * Entries used this frame are found through a direct-mapped index (they are
     * not evicted or reused within the frame) */
    static TexEntry *fidx[256];
    u32 tla = tl ? tl->addr : 0;
    u32 fk = (m->img * 0x9e3779b1u ^ (u32)m->w << 20 ^ (u32)m->h << 8 ^ m->fmt ^ tla * 0x85ebca6bu) >> 24;
    TexEntry *t = fidx[fk];
    if (t && t->used && t->last == frames && t->img == m->img && t->w == m->w && t->h == m->h && t->fmt == m->fmt &&
        t->tlut_addr == tla) return tex_bind_params(t, m, su, sv);
    for (int i = 0; i < TEX_CACHE; i++) {
        t = &tcache[i];
        if (t->used && t->last == frames && t->img == m->img && t->w == m->w && t->h == m->h && t->fmt == m->fmt &&
            t->tlut_addr == tla) { fidx[fk] = t; return tex_bind_params(t, m, su, sv); }
    }
    u32 n = tex_bytes(m->w, m->h, m->fmt), hash = hash_bytes(m->img, n);
    if (tl) hash ^= hash_bytes(tl->addr, tl->n * 2) * 31 + tl->fmt;
    TexEntry *e = NULL, *victim = NULL, *same = NULL;
    for (int i = 0; i < TEX_CACHE; i++) {
        TexEntry *t = &tcache[i];
        if (t->used && t->img == m->img && t->w == m->w && t->h == m->h && t->fmt == m->fmt) {
            if (t->hash == hash) { e = t; break; }
            if (t->last < frames) same = t;   /* changed contents: reuse its memory (the GPU is done with it) */
        }
        if (!t->used) { if (!victim || victim->used) victim = t; }
        else if ((!victim || (victim->used && t->last < victim->last)) && t->last + 2 < frames) victim = t;
    }
    if (!e) pend_flush();   /* the entry may be bound to a pending draw */
    if (!e && same) {
        e = same;
        tex_upload(e, m, e->tex.width, e->tex.height, pica_fmt(m->fmt), tl);
        e->hash = hash;
    } else if (!e) {
        if (!victim) { ONCE("texture cache full\n"); tex_miss("cache full", m); return NULL; }
        e = victim;
        if (e->used) C3D_TexDelete(&e->tex);
        e->used = 0;
        u32 W = pow2(m->w), H = pow2(m->h);
        GPU_TEXCOLOR pf = pica_fmt(m->fmt);
        while (!C3D_TexInit(&e->tex, W, H, pf)) {   /* out of linear memory: free the least recently used */
            TexEntry *old = NULL;
            for (int i = 0; i < TEX_CACHE; i++)
                if (tcache[i].used && tcache[i].last < frames && (!old || tcache[i].last < old->last)) old = &tcache[i];
            if (!old) { ONCE("C3D_TexInit failed with nothing left to evict\n"); tex_miss("C3D_TexInit failed", m); return NULL; }
            C3D_TexDelete(&old->tex); old->used = 0;
            st_evict++;
        }
        tex_upload(e, m, W, H, pf, tl);
        e->img = m->img; e->w = m->w; e->h = m->h; e->fmt = m->fmt; e->hash = hash; e->used = 1;
        e->pkey = 0;
    }
    e->last = frames;
    e->tlut_addr = tla;
    fidx[fk] = e;
    return tex_bind_params(e, m, su, sv);
}

static C3D_Tex *tex_bind_params(TexEntry *e, const TexMap *m, float *su, float *sv) {
    static const GPU_TEXTURE_WRAP_PARAM wrap[4] = {GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT};
    *su = (float)m->w / e->tex.width; *sv = (float)m->h / e->tex.height;
    u32 pk = (u32)(m->wrap_s & 3) | (u32)(m->wrap_t & 3) << 2 | (u32)m->mag_f << 4 | (u32)m->min_f << 8;
    if (e->pkey == pk + 1) return &e->tex;   /* wrap/filter already set */
    e->pkey = pk + 1;
    u32 param = e->tex.param;
    C3D_TexSetWrap(&e->tex, wrap[m->wrap_s & 3], wrap[m->wrap_t & 3]);
    C3D_TexSetFilter(&e->tex, m->mag_f ? GPU_LINEAR : GPU_NEAREST, (m->min_f == 0 || m->min_f == 2 || m->min_f == 3) ? GPU_NEAREST : GPU_LINEAR);
    if (e->tex.param != param && pend_n) {   /* a pending draw may use the old parameters */
        u32 nw = e->tex.param; e->tex.param = param; pend_flush(); e->tex.param = nw;
    }
    return &e->tex;
}

/* ---------------------------------------------------------------- TEV -> texenv */
static int unit_of_map[8];

/* cst: a per-draw constant (rgb in v[0..2] for the colour side, v[3] for alpha),
 * placed in the PICA stage constant or the combiner buffer's initial colour */
typedef struct { GPU_TEVSRC src; int opnd; int cst; u8 v[4]; } Src;   /* opnd: 0 color, 1 alpha, 2 one-minus color, 3-5 R/G/B */

/* GX TEV registers on PICA, per side (0 rgb, 1 alpha): a register is a per-draw
 * constant until a stage writes it, then lives in the PICA previous output; when
 * another register is written while it is still read later, it moves to the
 * combiner buffer (update bit on its last stage, readable two stages later) */
enum { L_CONST, L_PREV, L_BUF, L_LOST };
static struct { int loc[4], prev_reg, buf_reg, buf_ready, buf_mask, bufc_used; u8 bufc[4]; } tr[2];

static int konst_value(int sel, int alpha, u8 rgba[4]) {
    if (sel < 8) { u8 v = (u8)((8 - sel) * 255 / 8); rgba[0] = rgba[1] = rgba[2] = rgba[3] = v; return 1; }
    if (!alpha && sel >= 0x0c && sel <= 0x0f) { memcpy(rgba, gx.kcolor[sel - 0x0c], 4); return 1; }
    if (sel >= 0x10) { u8 v = gx.kcolor[sel & 3][(sel - 0x10) >> 2]; rgba[0] = rgba[1] = rgba[2] = rgba[3] = v; return 1; }
    return 0;
}

/* COLOR1/ALPHA1 are not computed by the vertex shader: their unlit part
 * (ambient * material when lit, material otherwise) */
static void chan1_value(u8 v[4]) {
    for (int i = 0; i < 4; i++) {
        const Chan *c = &gx.chan[i < 3 ? 1 : 3];
        int m = gx.mat[1][i], a = gx.amb[1][i];
        if (gx.n_chans < 2) { v[i] = 0; continue; }
        if (c->mat_src || (c->enable && c->amb_src)) { ONCE("COLOR1 vertex colour taken as white\n"); if (c->mat_src) m = 255; if (c->amb_src) a = 255; }
        if (c->enable && c->light_mask) ONCE("COLOR1 lighting not emulated (ambient only)\n");
        v[i] = (u8)(c->enable ? a * m / 255 : m);
    }
}

static Src konst_src(const u8 v[4], int opnd, int alpha) {
    Src s = {GPU_CONSTANT, alpha, 1, {0, 0, 0, 0}};
    if (alpha) s.v[3] = v[opnd ? 3 : 0];
    else if (opnd == 1) s.v[0] = s.v[1] = s.v[2] = v[3];
    else memcpy(s.v, v, 3);
    return s;
}

/* GX colour/alpha arg -> PICA source */
static Src color_src(const TevStage *t, int arg, int alpha) {
    Src s = {GPU_PREVIOUS, alpha, 0, {0, 0, 0, 0}};
    u8 v[4] = {0, 0, 0, 0};
    int map_unit = t->map < 8 ? unit_of_map[t->map] : -1;
    static const u8 alpha_arg[8] = {1, 3, 5, 7, 9, 11, 14, 15};   /* GX_CA_* -> GX_CC_* */
    if (alpha) arg = alpha_arg[arg & 7];
    switch (arg) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: {
        int r = arg >> 1, opnd = alpha ? 1 : (arg & 1), side = alpha || (arg & 1);
        switch (tr[side].loc[r]) {
        case L_PREV: s.src = GPU_PREVIOUS; s.opnd = opnd; return s;
        case L_BUF: s.src = GPU_PREVIOUS_BUFFER; s.opnd = opnd; s.cst = -1 - side; return s;   /* cst < 0: buffer read, checked when placed */
        case L_LOST: ONCE("TEV register read after it was overwritten in PICA\n"); break;
        }
        return konst_src(gx.tev_reg[r], opnd, alpha);
    }
    case 8: case 9:
        if (map_unit < 0) { ONCE("TEXC without texture\n"); v[0] = v[1] = v[2] = v[3] = 255; return konst_src(v, 0, alpha); }
        s.src = (GPU_TEVSRC)(GPU_TEXTURE0 + map_unit); s.opnd = alpha ? 1 : (arg & 1);
        if (!alpha && !(arg & 1) && t->tex_swap) s.opnd = 2 + t->tex_swap;   /* default swap tables 1-3: RRR, GGG, BBB */
        if (t->tex_swap) ONCE("TEV texture swap %d (alpha kept)\n", t->tex_swap);
        return s;
    case 10: case 11:
        if (t->chan == 1 || t->chan == 3 || t->chan == 5) { chan1_value(v); return konst_src(v, alpha ? 1 : (arg & 1), alpha); }
        if (t->chan == 6) return konst_src(v, 0, alpha);
        s.src = GPU_PRIMARY_COLOR; s.opnd = alpha ? 1 : (arg & 1); return s;
    case 12: v[0] = v[1] = v[2] = v[3] = 255; return konst_src(v, 0, alpha);
    case 13: v[0] = v[1] = v[2] = v[3] = 128; return konst_src(v, 0, alpha);
    case 14: konst_value(alpha ? t->ksel_a : t->ksel_c, alpha, v); return konst_src(v, alpha, alpha);
    default: return konst_src(v, 0, alpha);
    }
}

/* one GX side (color or alpha) -> up to 3 PICA ops */
typedef struct { GPU_COMBINEFUNC f; Src s[3]; int scale; } POp;

static int cst_is(Src s, int alpha, int val) {
    if (s.cst <= 0) return 0;
    return alpha ? s.v[3] == val : s.v[0] == val && s.v[1] == val && s.v[2] == val;
}

/* the combiner buffer's initial colour is free to hold a second constant */
static int bufc_free(int alpha) { return !tr[alpha].buf_mask && !tr[alpha].bufc_used; }

static int side_ops(const TevStage *t, int alpha, POp *ops) {
    const TevSide *g = alpha ? &t->a : &t->c;
    Src A = color_src(t, g->a, alpha), B = color_src(t, g->b, alpha);
    Src C = color_src(t, g->c, alpha), D = color_src(t, g->d, alpha);
    int z_a = cst_is(A, alpha, 0), z_b = cst_is(B, alpha, 0), z_c = cst_is(C, alpha, 0), z_d = cst_is(D, alpha, 0);
    int one_c = cst_is(C, alpha, 255);
    int n = 0, scale = g->scale == 1 ? 1 : g->scale == 2 ? 2 : 0;
    Src prev = {GPU_PREVIOUS, alpha, 0, {0, 0, 0, 0}};
    if (g->scale == 3) ONCE("TEV scale 1/2 not supported\n");
    if (g->op > 1) ONCE("TEV compare op %d not supported\n", g->op);
    if (g->bias) ONCE("TEV bias %d not supported\n", g->bias);
    /* lerp part: a*(1-c) + b*c */
    int lerp_zero = (z_a && z_b) || (z_a && z_c) || (z_b && one_c);
    if (lerp_zero) {   /* d +- 0 */
        ops[n++] = (POp){GPU_REPLACE, {D, D, D}, scale};
        return n;
    }
    POp l;
    if (z_a) l = one_c ? (POp){GPU_REPLACE, {B, B, B}} : (POp){GPU_MODULATE, {B, C, C}};
    else if (one_c) l = (POp){GPU_REPLACE, {B, B, B}};
    else if (z_c) l = (POp){GPU_REPLACE, {A, A, A}};
    else if (A.cst > 0 && C.cst > 0 && B.cst <= 0 && !bufc_free(alpha)) {
        /* two constants and no free buffer colour: b*c, then + a*(1-c) folded on the CPU */
        Src K = A;
        for (int i = 0; i < 4; i++) K.v[i] = (u8)(A.v[i] * (255 - C.v[i]) / 255);
        ops[n++] = (POp){GPU_MODULATE, {B, C, C}, 0};
        l = (POp){GPU_ADD, {prev, K, K}};
    } else l = (POp){GPU_INTERPOLATE, {B, A, C}};
    if (z_d) { l.scale = scale; if (g->op == 1) ONCE("TEV 0 - x\n"); ops[n++] = l; return n; }
    if (n == 0 && g->op == 0 && l.f == GPU_MODULATE) { ops[n++] = (POp){GPU_MULTIPLY_ADD, {B, C, D}, scale}; return n; }
    if (n == 0 && l.f == GPU_REPLACE) {
        ops[n++] = (POp){g->op ? GPU_SUBTRACT : GPU_ADD, {g->op ? D : l.s[0], g->op ? l.s[0] : D, D}, scale};
        return n;
    }
    ops[n++] = l;
    if (D.src == GPU_PREVIOUS) ONCE("TEV lerp + CPREV needs the combiner buffer\n");
    ops[n++] = (POp){g->op ? GPU_SUBTRACT : GPU_ADD, {g->op ? D : prev, g->op ? prev : D, D}, scale};
    return n;
}

static int opnd_rgb(int o) {
    static const int m[6] = {GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B};
    return m[o < 6 ? o : 0];
}

/* is side x of register r read by a stage after si before being rewritten? (PREV is the output) */
static int tev_live(int r, u32 si, int x) {
    for (u32 s = si + 1; s < gx.n_stages && s < 16; s++) {
        const TevStage *t = &gx.tev[s];
        const u8 *ca = &t->c.a, *aa = &t->a.a;
        for (int i = 0; i < 4; i++) {
            if (!x && ca[i] == 2 * r) return 1;
            if (x && (ca[i] == 2 * r + 1 || aa[i] == r)) return 1;
        }
        if ((x ? t->a.out : t->c.out) == r) return 0;
    }
    return r == 0;
}

/* places the constants of one PICA stage: stage constant first, then the buffer colour */
static void place_srcs(Src *s, int alpha, int pstage, u8 sc[4], int *scu) {
    for (int i = 0; i < 3; i++) {
        if (s[i].cst < 0) {
            if (tr[-1 - s[i].cst].buf_ready > pstage) ONCE("TEV register read from the combiner buffer too early\n");
            continue;
        }
        if (!s[i].cst) continue;
        const u8 *v = s[i].v + (alpha ? 3 : 0);
        u8 *d = sc + (alpha ? 3 : 0), *b = tr[alpha].bufc + (alpha ? 3 : 0);
        int n = alpha ? 1 : 3;
        if (!*scu || !memcmp(d, v, n)) { memcpy(d, v, n); *scu = 1; s[i].src = GPU_CONSTANT; }
        else if (!tr[alpha].buf_mask && (!tr[alpha].bufc_used || !memcmp(b, v, n))) {
            memcpy(b, v, n); tr[alpha].bufc_used = 1; s[i].src = GPU_PREVIOUS_BUFFER;
        } else { ONCE("three different constants in one PICA stage\n"); s[i].src = GPU_CONSTANT; }
    }
}

/* everything setup_tev reads: unchanged since the last draw -> the texenv is too */
static int tev_changed(void) {
    static TevStage tev[16];
    static u8 reg[16], kc[16], mat[4], amb[4];
    static int uom[8];
    static Chan chan[4];
    static u32 n, nch;
    static int valid;
    u32 ns = gx.n_stages < 16 ? gx.n_stages : 16;
    if (valid && ns == n && gx.n_chans == nch && !memcmp(tev, gx.tev, ns * sizeof(TevStage)) &&
        !memcmp(reg, gx.tev_reg, 16) && !memcmp(kc, gx.kcolor, 16) && !memcmp(uom, unit_of_map, sizeof uom) &&
        !memcmp(chan, gx.chan, sizeof chan) && !memcmp(mat, gx.mat[1], 4) && !memcmp(amb, gx.amb[1], 4)) return 0;
    n = ns; nch = gx.n_chans;
    memcpy(tev, gx.tev, ns * sizeof(TevStage)); memcpy(reg, gx.tev_reg, 16); memcpy(kc, gx.kcolor, 16);
    memcpy(uom, unit_of_map, sizeof uom); memcpy(chan, gx.chan, sizeof chan); memcpy(mat, gx.mat[1], 4); memcpy(amb, gx.amb[1], 4);
    valid = 1;
    return 1;
}

static void setup_tev(void) {
    int pstage = 0;
    if (!tev_changed()) return;
    pend_flush();
    memset(tr, 0, sizeof tr);
    for (int x = 0; x < 2; x++) { tr[x].prev_reg = tr[x].buf_reg = -1; tr[x].buf_ready = 99; }
    for (u32 si = 0; si < gx.n_stages && si < 16; si++) {
        const TevStage *t = &gx.tev[si];
        POp co[3], ao[3];
        int nc = side_ops(t, 0, co), na = side_ops(t, 1, ao);
        int n = nc > na ? nc : na;
        for (int x = 0; x < 2; x++) {   /* the register in PICA previous is about to be replaced: keep it if still read */
            int w = x ? t->a.out : t->c.out, h = tr[x].prev_reg;
            if (h < 0 || h == w) continue;
            tr[x].loc[h] = L_LOST;
            if (!tev_live(h, si, x)) continue;
            if (tr[x].buf_reg >= 0 && tev_live(tr[x].buf_reg, si, x)) ONCE("two TEV registers live besides the one being written\n");
            else if (pstage - 1 > 3) ONCE("TEV register kept after PICA stage 3\n");
            else { tr[x].buf_mask |= 1 << (pstage - 1); tr[x].loc[h] = L_BUF; tr[x].buf_reg = h; tr[x].buf_ready = pstage + 1; }
        }
        for (int k = 0; k < n; k++) {
            if (pstage >= 6) { ONCE("more than 6 PICA texenv stages needed\n"); break; }
            C3D_TexEnv *env = C3D_GetTexEnv(pstage);
            C3D_TexEnvInit(env);
            POp c = k < nc ? co[k] : (POp){GPU_REPLACE, {{GPU_PREVIOUS, 0, 0, {0}}}}, a = k < na ? ao[k] : (POp){GPU_REPLACE, {{GPU_PREVIOUS, 1, 0, {0}}}};
            u8 sc[4] = {0, 0, 0, 0}; int scc = 0, sca = 0;
            place_srcs(c.s, 0, pstage, sc, &scc);
            place_srcs(a.s, 1, pstage, sc, &sca);
            C3D_TexEnvSrc(env, C3D_RGB, c.s[0].src, c.s[1].src, c.s[2].src);
            C3D_TexEnvOpRgb(env, opnd_rgb(c.s[0].opnd), opnd_rgb(c.s[1].opnd), opnd_rgb(c.s[2].opnd));
            C3D_TexEnvFunc(env, C3D_RGB, c.f);
            C3D_TexEnvScale(env, C3D_RGB, (GPU_TEVSCALE)(k + 1 == nc ? c.scale : 0));
            C3D_TexEnvSrc(env, C3D_Alpha, a.s[0].src, a.s[1].src, a.s[2].src);
            C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(env, C3D_Alpha, a.f);
            C3D_TexEnvScale(env, C3D_Alpha, (GPU_TEVSCALE)(k + 1 == na ? a.scale : 0));
            C3D_TexEnvColor(env, sc[0] | sc[1] << 8 | sc[2] << 16 | (u32)sc[3] << 24);
            pstage++;
        }
        for (int x = 0; x < 2; x++) {
            int w = x ? t->a.out : t->c.out;
            if (tr[x].buf_reg == w) tr[x].buf_reg = -1;
            tr[x].loc[w] = L_PREV; tr[x].prev_reg = w;
        }
    }
    C3D_TexEnvBufUpdate(C3D_RGB, tr[0].buf_mask);
    C3D_TexEnvBufUpdate(C3D_Alpha, tr[1].buf_mask);
    C3D_TexEnvBufColor(tr[0].bufc[0] | tr[0].bufc[1] << 8 | tr[0].bufc[2] << 16 | (u32)tr[1].bufc[3] << 24);
    if (!pstage) {   /* no stages: pass the rasterized color */
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        pstage = 1;
    }
    for (int i = pstage; i < 6; i++) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
}

static GPU_TESTFUNC test_func(int f) {
    static const GPU_TESTFUNC m[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    return m[f & 7];
}
static GPU_BLENDFACTOR blend_factor(int f, int dst) {
    switch (f) {
    case 0: return GPU_ZERO; case 1: return GPU_ONE;
    case 2: return dst ? GPU_SRC_COLOR : GPU_DST_COLOR;
    case 3: return dst ? GPU_ONE_MINUS_SRC_COLOR : GPU_ONE_MINUS_DST_COLOR;
    case 4: return GPU_SRC_ALPHA; case 5: return GPU_ONE_MINUS_SRC_ALPHA;
    case 6: return GPU_DST_ALPHA; default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}

/* PICA state set only when its arguments changed (citro3d re-sends every state
 * that was set, changed or not, at the next draw) */
#define SET_IF_CHANGED(call, ...) do { static u32 last_[16]; static int valid_; \
    const u32 cur_[] = {__VA_ARGS__}; \
    if (!valid_ || memcmp(last_, cur_, sizeof cur_)) { memcpy(last_, cur_, sizeof cur_); valid_ = 1; pend_flush(); call; } } while (0)
static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void setup_pixel(void) {
    u32 mask = (gx.color_update ? GPU_WRITE_COLOR : 0) | (gx.alpha_update ? GPU_WRITE_ALPHA : 0);
    int zt = gx.z_enable, zf = zt ? test_func(gx.z_func) : GPU_ALWAYS;
    if (zt && gx.z_update) mask |= GPU_WRITE_DEPTH;
    SET_IF_CHANGED(C3D_DepthTest(zt, (GPU_TESTFUNC)zf, (GPU_WRITEMASK)mask), zt, zf, mask);
    /* GX window depth = far + (far - near) * z/w, clamped to [0, 1] by the depth buffer */
    float zs = gx.vp[5] - gx.vp[4], zo = gx.vp[5];
    SET_IF_CHANGED(C3D_DepthMap(true, zs, zo), fbits(zs), fbits(zo));
    GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
    GPU_BLENDFACTOR bs = GPU_ONE, bd = GPU_ZERO;
    if (gx.blend_type == 1) { bs = blend_factor(gx.blend_src, 0); bd = blend_factor(gx.blend_dst, 1); }
    else if (gx.blend_type == 3) { eq = GPU_BLEND_REVERSE_SUBTRACT; bs = bd = GPU_ONE; }
    else if (gx.blend_type == 2) ONCE("logic op blend %d\n", gx.logic_op);
    SET_IF_CHANGED(C3D_AlphaBlend(eq, eq, bs, bd, bs, bd), eq, bs, bd);
    /* alpha compare: PICA has one test; take whichever GX comparison is not ALWAYS */
    u8 *ac = gx.alpha_cmp;
    if (ac[0] != 7 && ac[3] != 7 && !(ac[2] == 0 && (ac[0] == 7 || ac[3] == 7))) ONCE("two-sided alpha compare %d %d op %d\n", ac[0], ac[3], ac[2]);
    int at = 1, af = GPU_ALWAYS, ar = 0;
    if (ac[0] != 7) { af = test_func(ac[0]); ar = ac[1]; }
    else if (ac[3] != 7) { af = test_func(ac[3]); ar = ac[4]; }
    else at = 0;
    SET_IF_CHANGED(C3D_AlphaTest(at, (GPU_TESTFUNC)af, ar), at, af, ar);
    SET_IF_CHANGED(C3D_CullFace(GPU_CULL_NONE), 0);
    if (gx.cull) ONCE("cull mode %d drawn without culling\n", gx.cull);
    /* scissor: GX EFB rect -> screen -> framebuffer (240 wide, 400 tall, rotated) */
    static s32 sk[8]; static int sk_valid;
    const s32 cur[8] = {(s32)gx.sc[0], (s32)gx.sc[1], (s32)gx.sc[2], (s32)gx.sc[3], (s32)gx.sc_off[0], (s32)gx.sc_off[1], (s32)gx.efb_w, (s32)gx.efb_h};
    if (sk_valid && !memcmp(sk, cur, sizeof cur)) return;
    memcpy(sk, cur, sizeof cur); sk_valid = 1;
    float s = efb_scale(), x0 = efb_x0(), y0 = efb_y0();
    float l = x0 + (gx.sc[0] - gx.sc_off[0]) * s, t = y0 + (gx.sc[1] - gx.sc_off[1]) * s;
    float r = l + gx.sc[2] * s, b = t + gx.sc[3] * s;
    l = l < x0 ? x0 : l;
    t = t < y0 ? y0 : t;
    r = r > 400 - x0 ? 400 - x0 : r < l ? l : r;
    b = b > 240 - y0 ? 240 - y0 : b < t ? t : b;
    u32 s0 = (u32)(240 - b), s1 = (u32)(400 - r), s2 = (u32)(240 - t), s3 = (u32)(400 - l);
    SET_IF_CHANGED(C3D_SetScissor(GPU_SCISSOR_NORMAL, s0, s1, s2, s3), s0, s1, s2, s3);
}

/* ---------------------------------------------------------------- vertices */

static const u8 comp_size[5] = {1, 1, 2, 2, 4};
static const u8 clr_size[6] = {2, 3, 4, 2, 3, 4};

static int attr_size(int fmt, int a) {
    int d = gx.desc[a];
    if (!d) return 0;
    if (a <= 8) return 1;
    if (d >= 2) return d == 2 ? 1 : 2;
    const VtxFmt *f = &gx.vat[fmt][a];
    switch (a) {
    case A_POS: return comp_size[f->type > 4 ? 4 : f->type] * (f->cnt ? 3 : 2);
    case A_NRM: return comp_size[f->type > 4 ? 4 : f->type] * (f->cnt ? 9 : 3);
    case A_CLR0: case A_CLR1: return clr_size[f->type > 5 ? 5 : f->type];
    default: return comp_size[f->type > 4 ? 4 : f->type] * (f->cnt ? 2 : 1);
    }
}
static int vtx_size(int fmt) { int n = 0; for (int a = 0; a <= 20; a++) n += attr_size(fmt, a); return n; }

static void read_color(const u8 *p, int type, u8 *c) {
    switch (type) {
    case 0: { u32 v = rgb565(p[0] << 8 | p[1]); c[0] = v; c[1] = v >> 8; c[2] = v >> 16; c[3] = 255; break; }
    case 1: c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; c[3] = 255; break;
    case 2: c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; c[3] = 255; break;
    case 3: c[0] = (p[0] >> 4) * 17; c[1] = (p[0] & 15) * 17; c[2] = (p[1] >> 4) * 17; c[3] = (p[1] & 15) * 17; break;
    case 4: { u32 v = p[0] << 16 | p[1] << 8 | p[2];
        for (int i = 0; i < 4; i++) { u32 x = (v >> (18 - 6 * i)) & 63; c[i] = x << 2 | x >> 4; } break; }
    default: c[0] = p[0]; c[1] = p[1]; c[2] = p[2]; c[3] = p[3]; break;
    }
}

/* One plan per draw: where each present attribute's bytes come from. rd reads
 * n components (big-endian, fixed point scaled) into out. */
typedef struct APlan APlan;
struct APlan { s8 attr, d, type, cs, n; u8 off, size, dst, kind, dpos; u16 imax; float scale; const u8 *base; u32 stride; void (*rd)(const u8 *, const APlan *, float *); };

static inline u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void rd_u8(const u8 *p, const APlan *a, float *o) { for (int i = 0; i < a->n; i++) o[i] = p[i] * a->scale; }
static void rd_s8(const u8 *p, const APlan *a, float *o) { for (int i = 0; i < a->n; i++) o[i] = (s8)p[i] * a->scale; }
static void rd_u16(const u8 *p, const APlan *a, float *o) { for (int i = 0; i < a->n; i++) o[i] = be16(p + 2 * i) * a->scale; }
static void rd_s16(const u8 *p, const APlan *a, float *o) { for (int i = 0; i < a->n; i++) o[i] = (s16)be16(p + 2 * i) * a->scale; }
static void rd_f32(const u8 *p, const APlan *a, float *o) {
    for (int i = 0; i < a->n; i++) { u32 v = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3]; o[i] = u2f(v); }
}
static void rd_s16x3(const u8 *p, const APlan *a, float *o) {
    float s = a->scale; o[0] = (s16)be16(p) * s; o[1] = (s16)be16(p + 2) * s; o[2] = (s16)be16(p + 4) * s;
}
static void rd_s16x2(const u8 *p, const APlan *a, float *o) { float s = a->scale; o[0] = (s16)be16(p) * s; o[1] = (s16)be16(p + 2) * s; }
static void rd_s8x3(const u8 *p, const APlan *a, float *o) { float s = a->scale; o[0] = (s8)p[0] * s; o[1] = (s8)p[1] * s; o[2] = (s8)p[2] * s; }
static void rd_f32x3(const u8 *p, const APlan *a, float *o) {
    (void)a; for (int i = 0; i < 3; i++, p += 4) o[i] = u2f((u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}
static void rd_f32x2(const u8 *p, const APlan *a, float *o) {
    (void)a; for (int i = 0; i < 2; i++, p += 4) o[i] = u2f((u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

static int make_plan(int fmt, APlan *pl, int *vsize) {
    int np = 0, off = 0;
    for (int a = 0; a <= 20; a++) {
        int d = gx.desc[a];
        if (!d) continue;
        APlan *q = &pl[np++];
        memset(q, 0, sizeof *q);
        q->attr = a; q->d = d; q->off = off;
        if (a <= 8) { off += 1; continue; }
        const VtxFmt *f = &gx.vat[fmt][a];
        gx.desc[a] = 1; q->size = attr_size(fmt, a); gx.desc[a] = d;
        off += d == 1 ? q->size : d == 2 ? 1 : 2;
        if (d >= 2) {
            q->base = ram + (GUEST(gx.array_base[a - A_POS]) & RAM_MASK);
            q->stride = gx.array_stride[a - A_POS];
        }
        q->type = f->type > 4 ? 4 : f->type;
        q->cs = comp_size[(int)q->type];
        int frac = f->frac;
        if (a == A_POS) q->n = f->cnt ? 3 : 2;
        else if (a == A_NRM) { q->n = 3; frac = f->type == 1 ? 6 : f->type == 3 ? 14 : 0; }
        else if (a >= A_TEX0) q->n = f->cnt ? 2 : 1;
        if (a == A_CLR0 || a == A_CLR1) q->type = f->type > 5 ? 5 : f->type;
        q->scale = 1.0f / (float)(1 << frac);
        if (a != A_CLR0 && a != A_CLR1) {
            static void (*const gen[5])(const u8 *, const APlan *, float *) = {rd_u8, rd_s8, rd_u16, rd_s16, rd_f32};
            q->rd = gen[(int)q->type];
            if (q->type == 3 && q->n == 3) q->rd = rd_s16x3;
            else if (q->type == 3 && q->n == 2) q->rd = rd_s16x2;
            else if (q->type == 1 && q->n == 3) q->rd = rd_s8x3;
            else if (q->type == 4 && q->n == 3) q->rd = rd_f32x3;
            else if (q->type == 4 && q->n == 2) q->rd = rd_f32x2;
        }
    }
    *vsize = off;
    return np;
}

/* Keeps only what the PICA vertex needs: PNMTXIDX, POS, NRM, CLR0 when a
 * material component comes from it, and texcoords a unit reads. dst = PVtx tc
 * unit mask for TEXn, and POS/NRM also feeding texgen. */
static u8 tc_fed;   /* PICA tc units written by some attribute (trim_plan) */

static int trim_plan(APlan *pl, int np, const s8 tsrc[3]) {
    int k = 0, clr = mat_vtx[0] || mat_vtx[3];
    tc_fed = 0;
    for (int i = 0; i < np; i++) {
        APlan q = pl[i];
        q.dst = 0;
        for (int u = 0; u < 3; u++)
            if ((q.attr == A_POS && tsrc[u] == 1) || (q.attr == A_NRM && tsrc[u] == 2) || (q.attr >= A_TEX0 && tsrc[u] == 3 + q.attr - A_TEX0)) q.dst |= 1 << u;
        tc_fed |= q.dst;
        if (q.attr == A_PNMTX || q.attr == A_POS || q.attr == A_NRM || (q.attr == A_CLR0 && clr) || (q.attr >= A_TEX0 && q.dst)) pl[k++] = q;
    }
    return k;
}

/* PICA vertex: see runtime/vshader.v.pica */
/* Per draw: tm = the vertex with every field an attribute does not write
 * (pos/nrm 0, texcoords (0, 0, 1), register material, current matrix). Per
 * attribute the destination is fixed (dpos = float offset in PVtx), so the
 * vertex loop has no per-attribute decisions beyond its kind. Indexed
 * attributes record the largest index they read (imax, for the cache). */
enum { K_PNMTX, K_FLOAT, K_CLR };
static void plan_finish(APlan *pl, int np, PVtx *tm) {
    memset(tm, 0, sizeof *tm);
    for (int u = 0; u < 3; u++) tm->tc[u][2] = 1;
    u32 pn = gx.cur_mtx;
    if (pn % 3 || pn >= 30) { ONCE("position matrix row %lu not a PNMTXn\n", (unsigned long)pn); pn = 0; }
    tm->pos[3] = (float)pn;
    for (int i = 0; i < 4; i++) tm->mat[i] = mat_vtx[i] ? 0 : mat_reg[i];
    for (int i = 0; i < np; i++) {
        APlan *a = &pl[i];
        a->imax = 0;
        if (a->attr == A_PNMTX) { a->kind = K_PNMTX; continue; }
        if (a->attr == A_CLR0) { a->kind = K_CLR; continue; }
        a->kind = K_FLOAT;
        int u0 = a->dst ? __builtin_ctz(a->dst) : -1;
        if (a->attr == A_POS) a->dpos = offsetof(PVtx, pos) / 4;
        else if (a->attr == A_NRM) a->dpos = offsetof(PVtx, nrm) / 4;
        else { a->dpos = offsetof(PVtx, tc[u0]) / 4; a->dst &= ~(1 << u0); }
    }
}

static void decode_vtx(const u8 *p, APlan *pl, int np, const PVtx *tm, PVtx *o) {
    *o = *tm;
    for (int i = 0; i < np; i++) {
        APlan *a = &pl[i];
        const u8 *src = p + a->off;
        if (a->kind == K_PNMTX) {
            u32 pn = *src;
            if (pn % 3 || pn >= 30) { ONCE("position matrix row %lu not a PNMTXn\n", (unsigned long)pn); pn = 0; }
            o->pos[3] = (float)pn;
            continue;
        }
        if (a->d >= 2) {
            u32 ix = a->d == 2 ? src[0] : (u32)(src[0] << 8 | src[1]);
            if (ix > a->imax) a->imax = ix;
            src = a->base + ix * a->stride;
        }
        if (a->kind == K_CLR) {
            u8 clr[4];
            read_color(src, a->type, clr);
            for (int c = 0; c < 4; c++) if (mat_vtx[c]) o->mat[c] = clr[c];
            continue;
        }
        float *d = (float *)o + a->dpos;
        a->rd(src, a, d);
        if (a->dst) for (int u = 0; u < 3; u++) if (a->dst >> u & 1) memcpy(o->tc[u], d, 12);
    }
}

/* ---------------------------------------------------------------- vertex cache
 * Decoded vertices of display-list draws, keyed by the vertex data address,
 * count, and a signature of everything decode reads besides guest memory (plan,
 * template vertex). Valid while a hash of the draw's vertex bytes and of each
 * indexed array (see arr_hash) still matches. Entries live in a
 * linear-memory arena the GPU reads directly (no copy into vbuf); the arena
 * is bump-allocated and emptied at a frame start once it filled up (with
 * C3D_FRAME_SYNCDRAW the previous frame is done with it by then). */
#define VC_N 16384   /* 2-way: sets of slots 2k, 2k+1 */
#define VC_ARR 6
#define VC_NONE 0xffffffffu
typedef struct {
    const u8 *data; u32 n, sig, dhash, ahash, narr, used, gen, off, checked;
    u32 ixoff, ixn, prim;   /* the draw's index list in vc_ix (ixn 0: none yet) */
    const u8 *arr[VC_ARR]; u32 alen[VC_ARR];
} VCache;
static VCache vcache[VC_N];
static VCache *vc_last;   /* entry of the last vc_draw, NULL if not kept */
#define VC_IX (VC_VERTS * 3)
static u16 vc_ix[VC_IX];
static u32 vc_ixtop;

static u32 hash_mem(const u8 *p, u32 n, u32 h) {
    u32 w;
    for (; n >= 4; n -= 4, p += 4) { memcpy(&w, p, 4); h = (h ^ w) * 0x9e3779b1u; h ^= h >> 15; }
    for (; n; n--, p++) { h = (h ^ *p) * 0x9e3779b1u; h ^= h >> 15; }
    return h;
}

static u32 plan_sig(const APlan *pl, int np, int vs, const PVtx *tm) {
    u32 h = hash_mem((const u8 *)tm, sizeof *tm, (u32)vs * 0x85ebca6bu + (u32)np);
    for (int i = 0; i < np; i++) {
        const APlan *a = &pl[i];
        u32 f[8] = {(u32)a->attr | (u32)(u8)a->d << 8 | (u32)(u8)a->type << 16 | (u32)(u8)a->n << 24,
                    (u32)a->off | (u32)a->size << 8 | (u32)a->dst << 16 | (u32)a->dpos << 24,
                    fbits(a->scale), (u32)(uintptr_t)a->base, a->stride, (u32)(uintptr_t)a->rd, (u32)a->kind, 0};
        h = hash_mem((const u8 *)f, sizeof f, h);
    }
    return h;
}

/* Each vertex array (base address) is hashed once per frame over the extent
 * any cached draw read from it (a larger extent rehashes, invalidating older
 * entries once). Assumes the game does not rewrite a vertex array between
 * two display-list draws of the same frame that read it. */
static u32 arr_hash(const u8 *p, u32 len) {
    static struct { const u8 *p; u32 len, frame, h; } memo[1024];
    u32 k = (u32)(uintptr_t)p * 0x9e3779b1u >> 22;
    if (memo[k].p != p) { memo[k].p = p; memo[k].len = 0; memo[k].frame = 0; }
    if (len > memo[k].len) { memo[k].len = len; memo[k].frame = 0; }
    if (memo[k].frame != frames + 1) { memo[k].frame = frames + 1; memo[k].h = hash_mem(p, memo[k].len, 0x811c9dc5u); }
    return memo[k].h;
}

static u32 vc_arrays_hash(const VCache *e) {
    u32 h = 0;
    for (u32 i = 0; i < e->narr; i++) h = (h ^ arr_hash(e->arr[i], e->alen[i])) * 0x9e3779b1u;
    return h;
}

static void vc_frame_begin(void) {   /* emptied when it filled up or is 3/4 full */
    if (!vc_full && vc_top < VC_VERTS / 4 * 3) return;
    vc_gen++; vc_top = 0; vc_full = 0; vc_ixtop = 0;
}

/* Display-list draw: returns the arena vertex offset of its decoded vertices
 * (decoding them there on a miss), or VC_NONE when the caller decodes into vbuf */
static u32 vc_draw(const u8 *data, int n, int vs, APlan *pl, int np, const PVtx *tm) {
    vc_last = NULL;
    if (!vc_arena || n > 0x8000) return VC_NONE;
    u32 sig = plan_sig(pl, np, vs, tm);
    u32 slot = ((u32)(uintptr_t)data * 0x9e3779b1u ^ sig ^ (u32)n * 0x85ebca6bu) >> 18 & (VC_N - 1);
    VCache *e = &vcache[slot & ~1u], *f = e + 1;
    if (e->gen != vc_gen) e->gen = 0;
    if (f->gen != vc_gen) f->gen = 0;
    if (!(e->gen && e->data == data && e->n == (u32)n && e->sig == sig)) {
        if (f->gen && f->data == data && f->n == (u32)n && f->sig == sig) e = f;
        else if (e->gen && (!f->gen || f->used < e->used)) e = f;   /* replace the empty or least recently used way */
    }
    /* the list bytes and arrays are checked once per frame per entry (same
     * assumption as arr_hash: not rewritten between two draws of a frame) */
    int same = e->gen && e->data == data && e->n == (u32)n && e->sig == sig;
    if (same && e->checked == frames + 1) {
        e->used = frames;
        vc_hits++;
        vc_last = e;
        return e->off;
    }
    u32 dh = hash_mem(data, (u32)n * vs, 0x811c9dc5u);
    if (same && e->dhash == dh && vc_arrays_hash(e) == e->ahash) {
        e->checked = frames + 1;
        e->used = frames;
        vc_hits++;
        vc_last = e;
        return e->off;
    }
    vc_misses++;
    e->gen = 0;
    if (vc_top + n > VC_VERTS) { vc_full = 1; return VC_NONE; }
    u32 off = vc_top;
    vc_top += n;
    PVtx *o = vc_arena + off;
    for (int i = 0; i < n; i++) decode_vtx(data + i * vs, pl, np, tm, &o[i]);
    if (off < vc_dlo) vc_dlo = off;
    if (vc_top > vc_dhi) vc_dhi = vc_top;
    u32 narr = 0;
    for (int i = 0; i < np; i++) if (pl[i].d >= 2 && pl[i].kind != K_PNMTX) {
        if (narr == VC_ARR) return off;   /* drawn from the arena, not kept */
        e->arr[narr] = pl[i].base;
        e->alen[narr++] = pl[i].imax * pl[i].stride + pl[i].size;
    }
    e->data = data; e->n = n; e->sig = sig; e->dhash = dh; e->narr = narr; e->off = off;
    e->ahash = vc_arrays_hash(e);
    e->used = frames; e->gen = vc_gen; e->ixn = 0; e->checked = frames + 1;
    vc_last = e;
    return off;
}

/* ---------------------------------------------------------------- uniforms */
static void unif_set(int loc, const float (*v)[4], int n) {
    if (!memcmp(ushadow[loc], v, n * 16)) return;
    pend_flush();
    memcpy(ushadow[loc], v, n * 16);
    C3D_FVec *p = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, loc, n);
    for (int i = 0; i < n; i++) { p[i].x = v[i][0]; p[i].y = v[i][1]; p[i].z = v[i][2]; p[i].w = v[i][3]; }
}

/* eye space -> clip space of the rotated 3DS framebuffer: GX projection, then the
 * viewport (ndc -> EFB pixel), EFB -> 400x240 and the 90-degree rotation, all
 * affine in (cx, cy, cw) so one 4x4 matrix */
/* memo of a uniform setter's inputs: true when they equal the previous call's */
static int same_inputs(void *last, int *valid, const void *key, size_t n) {
    if (*valid && !memcmp(last, key, n)) return 1;
    memcpy(last, key, n); *valid = 1;
    return 0;
}

static void set_clip(void) {
    struct { float proj[6], vp[4]; u32 ortho; s32 sc_off[2], w, h; } key;
    static u8 last[sizeof key]; static int valid;
    memcpy(key.proj, gx.proj, sizeof key.proj); memcpy(key.vp, gx.vp, sizeof key.vp);
    key.ortho = gx.proj_ortho; key.sc_off[0] = gx.sc_off[0]; key.sc_off[1] = gx.sc_off[1]; key.w = gx.efb_w; key.h = gx.efb_h;
    if (same_inputs(last, &valid, &key, sizeof key)) return;
    const float *p = gx.proj;
    float P[4][4] = {{0}}, m[4][4];
    if (gx.proj_ortho) { P[0][0] = p[0]; P[0][3] = p[1]; P[1][1] = p[2]; P[1][3] = p[3]; P[2][2] = p[4]; P[2][3] = p[5]; P[3][3] = 1; }
    else { P[0][0] = p[0]; P[0][2] = p[1]; P[1][1] = p[2]; P[1][2] = p[3]; P[2][2] = p[4]; P[2][3] = p[5]; P[3][2] = -1; }
    float s = efb_scale();
    float ay = s * gx.vp[3] * 0.5f / 120.0f, by = 1 - (efb_y0() + s * (gx.vp[1] + gx.vp[3] * 0.5f - gx.sc_off[1])) / 120.0f;
    float ax = s * gx.vp[2] * 0.5f / 200.0f, bx = 1 - (efb_x0() + s * (gx.vp[0] + gx.vp[2] * 0.5f - gx.sc_off[0])) / 200.0f;
    for (int c = 0; c < 4; c++) {
        m[0][c] = ay * P[1][c] + by * P[3][c];
        m[1][c] = -ax * P[0][c] + bx * P[3][c];
        m[2][c] = P[2][c];   /* GX and PICA both clip z to [-w, 0]; depth map in setup_pixel */
        m[3][c] = P[3][c];
    }
    unif_set(u_clip, m, 4);
}

static u32 xf_gen = 1;   /* bumped on every XF matrix write */
static u32 light_gen = 1;   /* bumped on every GXLoadLightObjImm */

static void set_mtx(int lit) {
    static u32 pos_gen, nrm_gen;
    float m[30][4];
    if (pos_gen != xf_gen) { memcpy(m, gx.xf, sizeof m); unif_set(u_pos, m, 30); pos_gen = xf_gen; }
    if (!lit || nrm_gen == xf_gen) return;
    nrm_gen = xf_gen;
    for (int r = 0; r < 30; r++) { const float *n = &gx.xf[0x400 + r * 3]; m[r][0] = n[0]; m[r][1] = n[1]; m[r][2] = n[2]; m[r][3] = 0; }
    unif_set(u_nrm, m, 30);
}

/* texgen for PICA unit u: rows R0, R1, R2 with (s, t) = (R0.x / R2.x, R1.x / R2.x) */
static void set_texgen(const float tscale[3][2], const int tcoord[3], s8 tsrc[3], const u8 *v0, const APlan *pl, int np) {
    struct { float tscale[3][2]; int tcoord[3]; u32 n, xf; TexGen g[8]; } key;
    static u8 last[sizeof key]; static int valid; static s8 last_src[3];
    int per_vtx = 0;
    for (int u = 0; u < 3; u++) if (tcoord[u] >= 0 && tcoord[u] < 8 && gx.desc[1 + tcoord[u]]) per_vtx = 1;
    memset(&key, 0, sizeof key);
    memcpy(key.tscale, tscale, sizeof key.tscale); memcpy(key.tcoord, tcoord, sizeof key.tcoord);
    key.n = gx.n_texgens; key.xf = xf_gen;
    for (int i = 0; i < 8; i++) { key.g[i].func = gx.texgen[i].func; key.g[i].src = gx.texgen[i].src; key.g[i].normalize = gx.texgen[i].normalize; key.g[i].mtx = gx.texgen[i].mtx; }
    if (per_vtx) valid = 0;   /* reads the first vertex */
    else if (same_inputs(last, &valid, &key, sizeof key)) { memcpy(tsrc, last_src, 3); return; }
    float m[9][4];
    memset(m, 0, sizeof m);
    for (int u = 0; u < 3; u++) {
        float (*row)[4] = &m[u * 3];
        float g3[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}};
        tsrc[u] = 0;
        row[2][3] = 1;
        int tci = tcoord[u];
        if (tci < 0 || tci >= (int)gx.n_texgens) continue;
        const TexGen *g = &gx.texgen[tci];
        if (g->src == 0) tsrc[u] = 1;
        else if (g->src == 1) tsrc[u] = 2;
        else if (g->src >= 4 && g->src <= 11) tsrc[u] = 3 + g->src - 4;
        else { ONCE("texgen src %d not supported\n", g->src); tsrc[u] = 0; }
        u32 mid = g->mtx;
        if (gx.desc[1 + tci]) {   /* per-vertex TEXnMTXIDX: the first vertex's */
            for (int i = 0; i < np; i++) if (pl[i].attr == 1 + tci) mid = v0[pl[i].off];
            ONCE("per-vertex texture matrix index: first vertex's used for the draw\n");
        }
        if (g->func <= 1 && mid != 60) {
            const float *x = &gx.xf[(mid & 63) * 4];
            for (int r = 0; r < (g->func == 0 ? 3 : 2); r++) memcpy(g3[r], x + r * 4, 16);
            if (g->src == 1) for (int r = 0; r < 3; r++) g3[r][3] = 0;   /* normal: w = 0 */
            if (g->func == 0) ONCE("projective texgen divided per vertex\n");
        } else if (g->func > 1) ONCE("texgen func %d not supported\n", g->func);
        for (int c = 0; c < 4; c++) {
            row[0][c] = g3[0][c] * tscale[u][0];
            row[1][c] = g3[2][c] - g3[1][c] * tscale[u][1];   /* PICA v = 1 is memory row 0 */
            row[2][c] = g3[2][c];
        }
    }
    unif_set(u_tex, m, 9);
    memcpy(last_src, tsrc, 3);
}

static void set_blit(int on, int n) {   /* light loop on/off and its count */
    static int last_on = -1, last_n = -1;
    if (on == last_on && (!on || n == last_n)) return;
    pend_flush();
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, u_blit, on);
    if (on) C3D_IVUnifSet(GPU_VERTEX_SHADER, u_iloop, n - 1, 0, 5, 0);
    last_on = on; if (on) last_n = n;
}

static int set_lighting_inner(const Chan *c0, const Chan *c2, int e0, int e2);
/* COLOR0/ALPHA0 lighting; returns whether the shader's light loop runs */
static int set_lighting(void) {
    const Chan *c0 = &gx.chan[0], *c2 = &gx.chan[2];
    int e0 = gx.n_chans && c0->enable, e2 = gx.n_chans && c2->enable;
    mat_vtx[0] = mat_vtx[1] = mat_vtx[2] = gx.n_chans && c0->mat_src;
    mat_vtx[3] = gx.n_chans && c2->mat_src;
    for (int i = 0; i < 4; i++) mat_reg[i] = gx.n_chans ? gx.mat[0][i] : 0;
    struct { Chan c[4]; u32 n, lg; u8 amb[4]; } key;
    static u8 last[sizeof key]; static int valid, last_lit;
    memset(&key, 0, sizeof key);
    for (int i = 0; i < 4; i++) key.c[i] = gx.chan[i];
    key.n = gx.n_chans; key.lg = light_gen; memcpy(key.amb, gx.amb[0], 4);
    if (same_inputs(last, &valid, &key, sizeof key)) return last_lit;
    last_lit = set_lighting_inner(c0, c2, e0, e2);
    return last_lit;
}
static int set_lighting_inner(const Chan *c0, const Chan *c2, int e0, int e2) {
    if ((e0 && c0->amb_src) || (e2 && c2->amb_src)) ONCE("vertex ambient colour: register used\n");
    float amb[1][4];
    for (int i = 0; i < 4; i++) amb[0][i] = (i < 3 ? e0 : e2) ? gx.amb[0][i] / 255.0f : 1;
    unif_set(u_amb, amb, 1);
    u8 mask = (e0 ? c0->light_mask : 0) | (e2 ? c2->light_mask : 0);
    if (!mask) { set_blit(0, 0); return 0; }
    const Chan *c = e0 ? c0 : c2;
    if (e0 && e2 && (c0->attn_fn != c2->attn_fn || c0->diff_fn != c2->diff_fn)) ONCE("COLOR0/ALPHA0 light functions differ: COLOR0's used\n");
    if (c->attn_fn == 0) ONCE("specular light attenuation approximated\n");
    float cfg[1][4] = {{c->diff_fn ? 1 : 0, c->diff_fn ? 0 : 1, c->diff_fn == 2 ? 0 : -1e30f, 0}};
    unif_set(u_lcfg, cfg, 1);
    float L[15][4];
    memset(L, 0, sizeof L);
    int n = 0;
    for (int l = 0; l < 8; l++) {
        if (!(mask >> l & 1)) continue;
        if (n == 3) { ONCE("more than 3 lights in one draw\n"); break; }
        const Light *s = &gx.light[l];
        float (*d)[4] = &L[n++ * 5];
        memcpy(d[0], s->pos, 12); memcpy(d[1], s->dir, 12);
        if (c->attn_fn == 1) { memcpy(d[2], s->a, 12); memcpy(d[3], s->k, 12); }
        else { d[2][0] = 1; d[3][0] = 1; }
        for (int i = 0; i < 4; i++) {
            int on = i < 3 ? e0 && (c0->light_mask >> l & 1) : e2 && (c2->light_mask >> l & 1);
            d[4][i] = on ? s->color[i] / 255.0f : 0;
        }
    }
    unif_set(u_lights, L, n * 5);
    set_blit(1, n);
    return 1;
}

static int in_dl;   /* inside GXCallDisplayList */
static void draw_inner(int prim, int fmt, const u8 *data, int n);
static void draw(int prim, int fmt, const u8 *data, int n) { if (!recomp_first_draw) recomp_first_draw = recomp_ticks(); in_draw++; draw_inner(prim, fmt, data, n); in_draw--; }
static void draw_inner(int prim, int fmt, const u8 *data, int n) {
    if (prim >= 0xa8) { ONCE("lines/points (0x%x) not drawn\n", prim); return; }
    frame_begin();
    /* a full GPU command buffer is a libctru svcBreak(PANIC). The frame's buffer
     * is one block (a C3D_FrameSplit only hands back the rest of it), so when
     * under 1024 words (~9 draws) remain, the frame's remaining draws are skipped */
    u32 *cb_addr, cb_size, cb_off;
    GPUCMD_GetBuffer(&cb_addr, &cb_size, &cb_off);   /* words; C3D_GetCmdBufUsage is only updated per frame */
    recomp_gx_dbg[0] = cb_off; recomp_gx_dbg[1] = prim; recomp_gx_dbg[2] = n;
    if (cb_off + 1024 > cb_size) {
        if (!st_split++) recomp_log("# gx: GPU command buffer full at frame %lu: rest of the frame's draws skipped\n", (unsigned long)frames);
        return;
    }
    /* textures: GX texmaps used by the TEV stages -> PICA units 0..2 */
    int nunits = 0, tcoord[3] = {-1, -1, -1};
    float tscale[3][2] = {{1, 1}, {1, 1}, {1, 1}};
    for (int i = 0; i < 8; i++) unit_of_map[i] = -1;
    for (u32 si = 0; si < gx.n_stages; si++) {
        const TevStage *t = &gx.tev[si];
        if (t->map >= 8 || unit_of_map[t->map] >= 0) continue;
        if (nunits >= 3) { ONCE("more than 3 texmaps in one draw\n"); tex_miss("4th texmap in a draw", &gx.map[t->map]); continue; }
        u64 t0 = recomp_ticks();
        C3D_Tex *tex = tex_get(&gx.map[t->map], &tscale[nunits][0], &tscale[nunits][1]);
        recomp_prof[PROF_GX_TEX] += recomp_ticks() - t0;
        if (!tex) continue;
        static const C3D_Tex *bound[3]; static u32 bound_param[3]; static void *bound_data[3];
        if (bound[nunits] != tex || bound_param[nunits] != tex->param || bound_data[nunits] != tex->data) {
            pend_flush();
            C3D_TexBind(nunits, tex);
            bound[nunits] = tex; bound_param[nunits] = tex->param; bound_data[nunits] = tex->data;
        }
        tcoord[nunits] = t->coord < 8 ? t->coord : -1;
        unit_of_map[t->map] = nunits++;
       
    }
    u64 ts = recomp_ticks();
   
    setup_tev();
   
    setup_pixel();
    recomp_prof[PROF_GX_STATE] += recomp_ticks() - ts;
    int ntri = prim == 0x80 ? n / 4 * 2 : prim == 0x90 ? n / 3 : n >= 3 ? n - 2 : 0;
    if (!ntri) return;
    static u32 full_logged;
    if (ib_used + ntri * 3 > IB_INDICES) {
        if (full_logged < 8) { full_logged++; recomp_log("# gx: vertex buffer full at frame %lu (verts %lu + %d, indices %lu)\n",
            (unsigned long)frames, (unsigned long)vb_used, n, (unsigned long)ib_used); }
        return;
    }
    u64 t0 = recomp_ticks();
    APlan pl[21];
    int vs, np = make_plan(fmt, pl, &vs);
    s8 tsrc[3];
    u64 tu = recomp_ticks();
    set_clip();
   
    int lit = set_lighting();
   
    set_mtx(lit);
   
    set_texgen(tscale, tcoord, tsrc, data, pl, np);
    u64 td = recomp_ticks();
    recomp_prof[PROF_VX_UNIF] += td - tu;
    np = trim_plan(pl, np, tsrc);
    PVtx tm;
    plan_finish(pl, np, &tm);
    u32 co = in_dl ? vc_draw(data, n, vs, pl, np, &tm) : VC_NONE, v0, vadd = 0;
    if (co != VC_NONE) { u32 w = co & ~0x7fffu; use_buf(vc_arena + w); v0 = co - w; }
    else {
        if (vb_used + n > VB_VERTS) {
            if (full_logged < 8) { full_logged++; recomp_log("# gx: vertex buffer full at frame %lu (verts %lu + %d)\n", (unsigned long)frames, (unsigned long)vb_used, n); }
            return;
        }
        if (vb_used + n - vb_base > 0x10000) vb_base = vb_used;
        use_buf(vbuf[vb_half] + vb_base);
        PVtx *o = vbuf[vb_half] + vb_used;
        for (int i = 0; i < n; i++) decode_vtx(data + i * vs, pl, np, &tm, &o[i]);
        v0 = vb_used - vb_base; vadd = n;
    }
    recomp_prof[PROF_VX_DEC] += recomp_ticks() - td;
    u16 *ix = ibuf[vb_half] + ib_used;
    int k = 0;
    VCache *ve = co != VC_NONE ? vc_last : NULL;
    if (ve && ve->ixn && ve->prim == (u32)prim) { memcpy(ix, vc_ix + ve->ixoff, ve->ixn * 2); k = ve->ixn; ntri = 0; }
    for (int t = 0; t < ntri; t++) {
        int a, b, c;
        if (prim == 0x80) { int q = (t / 2) * 4; if (t & 1) { a = q; b = q + 2; c = q + 3; } else { a = q; b = q + 1; c = q + 2; } }
        else if (prim == 0x90) { a = 3 * t; b = a + 1; c = a + 2; }
        else if (prim == 0x98) { a = t; b = t + 1; c = t + 2; if (t & 1) { int x = a; a = b; b = x; } }
        else { a = 0; b = t + 1; c = t + 2; }
        ix[k++] = v0 + a; ix[k++] = v0 + b; ix[k++] = v0 + c;
    }
    if (ve && ntri && vc_ixtop + k <= VC_IX) {   /* keep the list for the entry's next hits */
        memcpy(vc_ix + vc_ixtop, ix, k * 2); ve->ixoff = vc_ixtop; ve->ixn = k; ve->prim = prim; vc_ixtop += k;
    }
    ntri = k / 3;
    t0 = recomp_ticks() - t0;
    recomp_prof[PROF_GX_VTX] += t0;
    if (in_dl) recomp_prof[PROF_VX_DL] += t0;
    st_draws++; st_tris += ntri; if (nunits) st_tex++;
    ts = recomp_ticks();
    if (batch_off == 2) { FILE *f = fopen("sdmc:/cr_batch_off.txt", "rb"); batch_off = f != NULL; if (f) fclose(f);
        recomp_log("# gx: draw batching %s\n", batch_off ? "off (cr_batch_off.txt)" : "on"); }
    int merge = last_end == ix && pend_n + k <= 0xfff0;
    if (merge) st_merged++;
    if (merge && pend_n) pend_n += k;
    else { pend_issue(); pend_ix = ix; pend_n = k; }
    if (batch_off) pend_issue();
    last_end = ix + k;
    recomp_prof[PROF_GX_STATE] += recomp_ticks() - ts;
    vb_used += vadd; ib_used += k;
}

/* ---------------------------------------------------------------- FIFO */
static u8 fifo[0x40000];
static u32 fifo_n;

/* GX command stream: draws are decoded; CP/XF/BP register writes are not
 * emulated (the GX calls above keep the state) and are logged once. Returns
 * the bytes consumed; an incomplete command at the end is left. */
static void call_display_list(u32 addr, u32 n);

static u32 parse_cmds(const u8 *buf, u32 len) {
    u32 pos = 0;
    while (pos < len) {
        u8 op = buf[pos];
        u32 need;
        if (op >= 0x80) {
            if (len - pos < 3) break;
            u32 n = buf[pos + 1] << 8 | buf[pos + 2];
            need = 3 + n * vtx_size(op & 7);
            if (len - pos < need) break;
            draw(op & 0xf8, op & 7, buf + pos + 3, n);
            pos += need;
            continue;
        }
        switch (op) {
        case 0x00: need = 1; break;
        case 0x08: need = 6; break;
        case 0x10: {
            if (len - pos < 5) { need = 5; break; }
            need = 5 + ((buf[pos + 1] << 8 | buf[pos + 2]) + 1) * 4; break; }
        case 0x20: case 0x28: case 0x30: case 0x38: need = 5; break;
        case 0x40: need = 9; break;
        case 0x48: need = 1; break;
        case 0x61: need = 5; break;
        default: ONCE("unknown GX command %02x, rest of stream dropped\n", op); return len;
        }
        if (len - pos < need) break;
        if (op == 0x40) {
            const u8 *q = buf + pos + 1;
            call_display_list((u32)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3], (u32)q[4] << 24 | q[5] << 16 | q[6] << 8 | q[7]);
        } else if (op && op != 0x48) ONCE("raw GX command %02x not emulated\n", op);
        pos += need;
    }
    return pos;
}

static void call_display_list(u32 addr, u32 n) {
    static int depth;
    if (depth > 2 || n > 0x1000000) { ONCE("display list nesting/size\n"); return; }
    depth++; in_dl++;
    u32 used = parse_cmds(ram + (GUEST(addr) & RAM_MASK), n);
    if (used < n) ONCE("display list with a truncated command\n");
    depth--; in_dl--;
}

void sdk_GXCallDisplayList(void) { LOG(); call_display_list(R[3], R[4]); }

static void fifo_parse(void) {
    u32 pos = parse_cmds(fifo, fifo_n);
    memmove(fifo, fifo + pos, fifo_n - pos);
    fifo_n -= pos;
}

static void fifo_put(u32 v, int size) {
    if (fifo_n + size > sizeof fifo) { ONCE("FIFO overflow\n"); fifo_n = 0; }
    for (int i = size - 1; i >= 0; i--) fifo[fifo_n++] = (u8)(v >> (8 * i));
}

/* write-gather pipe (hw_write 0xCC008000) */
void recomp_gx_fifo_write(u32 v, int size) {
    fifo_put(v, size);
    if (fifo_n >= 3 && fifo[0] >= 0x80) {
        u32 n = fifo[1] << 8 | fifo[2];
        if (fifo_n >= 3 + n * vtx_size(fifo[0] & 7)) fifo_parse();
    } else if (fifo_n && fifo[0] < 0x80) fifo_parse();
}

/* ---------------------------------------------------------------- draw-done */
static u32 draw_done_cb;
static int draw_done_pending;

static void deliver_draw_done(void) {
    if (!draw_done_pending) return;
    draw_done_pending = 0;
    if (draw_done_cb) recomp_call_guest(draw_done_cb, 0);
}

void sdk_GXSetDrawDoneCallback(void) { LOG(); u32 old = draw_done_cb; draw_done_cb = R[3]; R[3] = old; }
void sdk_GXWaitDrawDone(void) { LOG(); deliver_draw_done(); }
void sdk_GXDrawDone(void) { LOG(); draw_done_pending = 1; deliver_draw_done(); }

/* ---------------------------------------------------------------- vertex format */
void sdk_GXBegin(void) {
    LOG();
    if (fifo_n) { ONCE("GXBegin with %lu bytes of the previous primitive missing\n", (unsigned long)fifo_n); fifo_n = 0; }
    fifo_put((R[3] & 0xf8) | (R[4] & 7), 1);
    fifo_put(R[5] & 0xffff, 2);
    if ((R[5] & 0xffff) == 0 || vtx_size(R[4] & 7) == 0) fifo_n = 0;
}
void sdk_GXSetVtxDesc(void) { LOG(); if (R[3] < A_MAX) gx.desc[R[3]] = R[4] & 3; else ONCE("GXSetVtxDesc attr %lu\n", (unsigned long)R[3]); }
void sdk_GXClearVtxDesc(void) { LOG(); memset(gx.desc, 0, sizeof gx.desc); gx.desc[A_POS] = 1; }
void sdk_GXSetVtxAttrFmt(void) {
    LOG();
    if ((R[3] & 7) == R[3] && R[4] < A_MAX) gx.vat[R[3]][R[4]] = (VtxFmt){(u8)R[5], (u8)R[6], (u8)R[7]};
}
void sdk_GXSetArray(void) { LOG(); if (R[3] >= A_POS && R[3] <= 20) { gx.array_base[R[3] - A_POS] = R[4]; gx.array_stride[R[3] - A_POS] = R[5] & 0xff; } }
void sdk_GXInvalidateVtxCache(void) { LOG(); }

/* ---------------------------------------------------------------- transform */
static void load_mtx(u32 src, u32 addr, int rows, int cols) {
    xf_gen++;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++)
        if (addr + r * cols + c < 0x600) gx.xf[addr + r * cols + c] = u2f(rd32(src + (r * cols + c) * 4));
}
void sdk_GXLoadPosMtxImm(void) { LOG(); load_mtx(R[3], R[4] * 4, 3, 4); }
void sdk_GXLoadNrmMtxImm(void) {   /* source is a 3x4 matrix, the 3x3 part is used */
    LOG();
    xf_gen++;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) gx.xf[0x400 + R[4] * 3 + r * 3 + c] = u2f(rd32(R[3] + (r * 4 + c) * 4));
}
void sdk_GXLoadTexMtxImm(void) {
    LOG();
    if (R[4] >= 64) { ONCE("post-transform texture matrix %lu ignored\n", (unsigned long)R[4]); return; }
    load_mtx(R[3], R[4] * 4, R[5] == 1 ? 2 : 3, 4);
}
void sdk_GXSetCurrentMtx(void) { LOG(); gx.cur_mtx = R[3]; }
void sdk_GXSetProjection(void) {
    LOG();
    u32 m = R[3];
    gx.proj_ortho = R[4] == 1;
    gx.proj[0] = u2f(rd32(m + 0x00));
    gx.proj[1] = u2f(rd32(m + (gx.proj_ortho ? 0x0c : 0x08)));
    gx.proj[2] = u2f(rd32(m + 0x14));
    gx.proj[3] = u2f(rd32(m + (gx.proj_ortho ? 0x1c : 0x18)));
    gx.proj[4] = u2f(rd32(m + 0x28));
    gx.proj[5] = u2f(rd32(m + 0x2c));
}
void sdk_GXSetProjectionv(void) {
    LOG();
    u32 p = R[3];
    gx.proj_ortho = u2f(rd32(p)) != 0;
    for (int i = 0; i < 6; i++) gx.proj[i] = u2f(rd32(p + 4 + i * 4));
}
void sdk_GXGetProjectionv(void) {
    LOG();
    wr32(R[3], f2u(gx.proj_ortho ? 1.0f : 0.0f));
    for (int i = 0; i < 6; i++) wr32(R[3] + 4 + i * 4, f2u(gx.proj[i]));
}
void sdk_GXSetViewport(void) { LOG(); for (int i = 0; i < 6; i++) gx.vp[i] = (float)F[1 + i].ps0; }
void sdk_GXGetViewportv(void) { LOG(); for (int i = 0; i < 6; i++) wr32(R[3] + i * 4, f2u(gx.vp[i])); }
void sdk_GXSetScissor(void) { LOG(); gx.sc[0] = R[3]; gx.sc[1] = R[4]; gx.sc[2] = R[5]; gx.sc[3] = R[6]; }
void sdk_GXSetScissorBoxOffset(void) { LOG(); gx.sc_off[0] = (s32)R[3]; gx.sc_off[1] = (s32)R[4]; }
void sdk_GXSetClipMode(void) { LOG(); }
void sdk_GXSetCullMode(void) { LOG(); gx.cull = R[3]; }

/* ---------------------------------------------------------------- texgen / channels / lights */
void sdk_GXSetNumTexGens(void) { LOG(); gx.n_texgens = R[3] & 15; }
void sdk_GXSetTexCoordGen2(void) { LOG(); if (R[3] < 8) gx.texgen[R[3]] = (TexGen){(u8)R[4], (u8)R[5], (u8)R[7], R[6]}; }
void sdk_GXSetNumChans(void) { LOG(); gx.n_chans = R[3] & 3; }
void sdk_GXSetChanCtrl(void) {
    LOG();
    Chan c = {(u8)R[4], (u8)R[5], (u8)R[6], (u8)R[9], (u8)R[8], (u8)R[7]};
    u32 id = R[3];
    if (id == 4 || id == 5) { gx.chan[id - 4] = c; gx.chan[id - 2] = c; }
    else if (id < 4) gx.chan[id] = c;
}
static void set_color(u8 *dst, u32 ptr) { for (int i = 0; i < 4; i++) dst[i] = rd8(ptr + i); }
void sdk_GXSetChanMatColor(void) { LOG(); u32 id = R[3]; u8 c[4]; set_color(c, R[4]);
    if (id == 4 || id == 5) memcpy(gx.mat[id - 4], c, 4); else if (id < 2) memcpy(gx.mat[id], c, 3); else if (id < 4) gx.mat[id - 2][3] = c[3]; }
void sdk_GXSetChanAmbColor(void) { LOG(); u32 id = R[3]; u8 c[4]; set_color(c, R[4]);
    if (id == 4 || id == 5) memcpy(gx.amb[id - 4], c, 4); else if (id < 2) memcpy(gx.amb[id], c, 3); else if (id < 4) gx.amb[id - 2][3] = c[3]; }

/* GXLightObj, SDK layout (0x40 bytes, big-endian): +0xc color, +0x10 a0 a1 a2
 * k0 k1 k2, +0x28 position, +0x34 direction */
static float lf(u32 o, int i) { return u2f(rd32(o + i * 4)); }
static void lset(u32 o, int i, float v) { wr32(o + i * 4, f2u(v)); }
void sdk_GXInitLightPos(void) { LOG(); for (int i = 0; i < 3; i++) lset(R[3] + 0x28, i, F[1 + i].ps0); }
void sdk_GXInitLightDir(void) { LOG(); for (int i = 0; i < 3; i++) lset(R[3] + 0x34, i, -F[1 + i].ps0); }  /* SDK stores -dir */
void sdk_GXInitLightColor(void) { LOG(); wr32(R[3] + 0xc, rd32(R[4])); }
void sdk_GXInitLightAttn(void) { LOG(); for (int i = 0; i < 6; i++) lset(R[3] + 0x10, i, F[1 + i].ps0); }
void sdk_GXInitLightSpot(void) {
    LOG();
    float cut = F[1].ps0, a0, a1, a2; int fn = R[4];
    if (cut <= 0 || cut > 90) fn = 0;
    float cr = cosf(cut * 3.14159265f / 180), d = (1 - cr) * (1 - cr);
    switch (fn) {   /* GXInitLightSpot formulas (SDK) */
    case 1: a0 = -1000 * cr; a1 = 1000; a2 = 0; break;
    case 2: a0 = -cr / (1 - cr); a1 = 1 / (1 - cr); a2 = 0; break;
    case 3: a0 = 0; a1 = -cr / (1 - cr); a2 = 1 / (1 - cr); break;
    case 4: a0 = cr * (cr - 2) / d; a1 = 2 / d; a2 = -1 / d; break;
    case 5: a0 = -4 * cr / d; a1 = 4 * (1 + cr) / d; a2 = -4 / d; break;
    case 6: a0 = 1 - 2 * cr * cr / d; a1 = 4 * cr / d; a2 = -2 / d; break;
    default: a0 = 1; a1 = a2 = 0; break;
    }
    lset(R[3] + 0x10, 0, a0); lset(R[3] + 0x10, 1, a1); lset(R[3] + 0x10, 2, a2);
}
void sdk_GXInitLightDistAttn(void) {
    LOG();
    float ref_d = F[1].ps0, ref_b = F[2].ps0, k0 = 1, k1 = 0, k2 = 0; int fn = R[4];
    if (ref_d < 0 || ref_b <= 0 || ref_b >= 1) fn = 0;
    switch (fn) {
    case 1: k1 = (1 - ref_b) / (ref_b * ref_d); break;
    case 2: k1 = 0.5f * (1 - ref_b) / (ref_b * ref_d); k2 = 0.5f * (1 - ref_b) / (ref_b * ref_d * ref_d); break;
    case 3: k2 = (1 - ref_b) / (ref_b * ref_d * ref_d); break;
    }
    lset(R[3] + 0x10, 3, k0); lset(R[3] + 0x10, 4, k1); lset(R[3] + 0x10, 5, k2);
}
void sdk_GXLoadLightObjImm(void) {
    LOG();
    for (int i = 0; i < 8; i++) if (R[4] == (1u << i)) {
        Light *l = &gx.light[i]; u32 o = R[3];
        light_gen++;
        for (int k = 0; k < 3; k++) { l->a[k] = lf(o + 0x10, k); l->k[k] = lf(o + 0x1c, k); l->pos[k] = lf(o + 0x28, k); l->dir[k] = -lf(o + 0x34, k); }
        for (int k = 0; k < 4; k++) l->color[k] = rd8(o + 0xc + k);
        return;
    }
}

/* ---------------------------------------------------------------- textures */
/* GXTexObj, SDK layout (0x20 bytes, big-endian): +0 mode0 (wrap s/t, mag, min),
 * +8 image0 (w-1, h-1, fmt), +0xc image3 (address >> 5), +0x10 user data,
 * +0x14 fmt, +0x18 tlut name, +0x1f flags (1 mipmap, 2 CI).
 * GXTlutObj (0xc): +0 fmt << 10, +4 address >> 5 | 0x64 << 24, +8 entries. */
static const u8 min_hw[6] = {0, 4, 1, 5, 2, 6};   /* GXTexFilter -> TX_SETMODE0 encoding */
static void texobj_init(u32 o, u32 img, u32 w, u32 h, u32 fmt, u32 ws, u32 wt, u32 mip, int ci, u32 tlut) {
    wr32(o + 0, (ws & 3) | (wt & 3) << 2 | 1 << 4 | (mip ? 6u : 4u) << 5);
    wr32(o + 4, 0);
    wr32(o + 8, ((w - 1) & 0x3ff) | ((h - 1) & 0x3ff) << 10 | (fmt & 15) << 20);
    wr32(o + 0xc, (img & RAM_MASK) >> 5);
    wr32(o + 0x14, fmt);
    wr32(o + 0x18, tlut);
    wr8(o + 0x1f, (mip ? 1 : 0) | (ci ? 2 : 0));
}
static void texobj_lod(u32 o, u32 min_f, u32 mag_f) {
    wr32(o, (rd32(o) & ~0xf0u) | (mag_f & 1) << 4 | min_hw[min_f < 6 ? min_f : 1] << 5);
}
static TexMap texobj_read(u32 o) {
    u32 m0 = rd32(o), i0 = rd32(o + 8), hw_min = (m0 >> 5) & 7, min_f = 1;
    for (int i = 0; i < 6; i++) if (min_hw[i] == hw_min) min_f = i;
    return (TexMap){0x80000000u | rd32(o + 0xc) << 5, (u16)((i0 & 0x3ff) + 1), (u16)(((i0 >> 10) & 0x3ff) + 1),
                    (u8)((i0 >> 20) & 15), (u8)(m0 & 3), (u8)((m0 >> 2) & 3), (u8)min_f, (u8)((m0 >> 4) & 1),
                    (u8)((rd8(o + 0x1f) & 2) != 0), rd32(o + 0x18)};
}
static void tlutobj_init(u32 o, u32 lut, u32 fmt, u32 n) {
    wr32(o, (fmt & 3) << 10);
    wr32(o + 4, (lut & RAM_MASK) >> 5 | 0x64u << 24);
    wr16(o + 8, (u16)n);
}
void sdk_GXInitTexObj(void) { LOG(); texobj_init(R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10] & 0xff, 0, 0); }
void sdk_GXInitTexObjCI(void) { LOG(); texobj_init(R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10] & 0xff, 1, rd32(R[1] + 8)); }
void sdk_GXInitTexObjLOD(void) { LOG(); texobj_lod(R[3], R[4], R[5]); }
void sdk_GXLoadTexObj(void) { LOG(); if (R[4] < 8) gx.map[R[4]] = texobj_read(R[3]); }
void sdk_GXInitTlutObj(void) { LOG(); tlutobj_init(R[3], R[4], R[5], R[6] & 0xffff); }
void sdk_GXLoadTlut(void) {
    LOG();
    if (R[4] >= 20) { ONCE("tlut name %lu\n", (unsigned long)R[4]); return; }
    gx.tlut[R[4]] = (Tlut){0x80000000u | (rd32(R[3] + 4) & 0xffffff) << 5, (u8)((rd32(R[3]) >> 10) & 3), rd16(R[3] + 8)};
}
static u32 tex_region_cb, tlut_region_cb;
void sdk_GXSetTexRegionCallback(void) { LOG(); u32 o = tex_region_cb; tex_region_cb = R[3]; R[3] = o; }
void sdk_GXSetTlutRegionCallback(void) { LOG(); u32 o = tlut_region_cb; tlut_region_cb = R[3]; R[3] = o; }

/* TPL helpers (TPL library, SDK): descriptors are {image header*, palette header*};
 * image header: +0 h, +2 w, +4 fmt, +8 data, +0xc/+0x10 wrap s/t, +0x14/+0x18 min/mag,
 * +0x21/+0x22 min/max LOD (mipmap when they differ) */
void sdk_TPLGet(void) { LOG(); R[3] = rd32(R[3] + 8) + R[4] * 8; }
static void tpl_texobj(u32 obj, u32 img, int ci, u32 tlut) {
    texobj_init(obj, rd32(img + 8), rd16(img + 2), rd16(img), rd32(img + 4), rd32(img + 0xc), rd32(img + 0x10),
                rd8(img + 0x21) != rd8(img + 0x22), ci, tlut);
    texobj_lod(obj, rd32(img + 0x14), rd32(img + 0x18));
}
void sdk_TPLGetGXTexObjFromPalette(void) {
    LOG();
    u32 d = rd32(R[3] + 8) + R[5] * 8;
    tpl_texobj(R[4], rd32(d), 0, 0);
}
void sdk_TPLGetGXTexObjFromPaletteCI(void) {
    LOG();
    u32 d = rd32(R[3] + 8) + R[7] * 8, pal = rd32(d + 4);
    tlutobj_init(R[5], rd32(pal + 8), rd32(pal + 4), rd16(pal));
    tpl_texobj(R[4], rd32(d), 1, R[6]);
}

/* ---------------------------------------------------------------- TEV */
static TevSide *side(int alpha) { return R[3] < 16 ? (alpha ? &gx.tev[R[3]].a : &gx.tev[R[3]].c) : NULL; }
void sdk_GXSetNumTevStages(void) { LOG(); gx.n_stages = R[3] & 31; }
void sdk_GXSetTevOrder(void) { LOG(); if (R[3] < 16) { TevStage *t = &gx.tev[R[3]]; t->coord = R[4]; t->map = R[5] == 0xff ? 0xff : R[5] & 7; t->chan = R[6]; } }
void sdk_GXSetTevColorIn(void) { LOG(); TevSide *s = side(0); if (s) { s->a = R[4]; s->b = R[5]; s->c = R[6]; s->d = R[7]; } }
void sdk_GXSetTevAlphaIn(void) { LOG(); TevSide *s = side(1); if (s) { s->a = R[4]; s->b = R[5]; s->c = R[6]; s->d = R[7]; } }
void sdk_GXSetTevColorOp(void) { LOG(); TevSide *s = side(0); if (s) { s->op = R[4]; s->bias = R[5]; s->scale = R[6]; s->clamp = R[7]; s->out = R[8]; } }
void sdk_GXSetTevAlphaOp(void) { LOG(); TevSide *s = side(1); if (s) { s->op = R[4]; s->bias = R[5]; s->scale = R[6]; s->clamp = R[7]; s->out = R[8]; } }
void sdk_GXSetTevOp(void) {
    LOG();
    if (R[3] >= 16) return;
    TevStage *t = &gx.tev[R[3]];
    int c = R[3] ? 0 : 10, a = R[3] ? 0 : 5;   /* CPREV/APREV after stage 0, RASC/RASA at stage 0 */
    static const u8 cin[5][4] = {{15, 8, 99, 15}, {99, 8, 9, 15}, {99, 12, 8, 15}, {15, 15, 15, 8}, {15, 15, 15, 99}};
    static const u8 ain[5][4] = {{7, 4, 99, 7}, {7, 7, 7, 99}, {7, 4, 99, 7}, {7, 7, 7, 4}, {7, 7, 7, 99}};
    int m = R[4] < 5 ? R[4] : 0;
    u8 *cp = &t->c.a, *ap = &t->a.a;
    for (int i = 0; i < 4; i++) { cp[i] = cin[m][i] == 99 ? c : cin[m][i]; ap[i] = ain[m][i] == 99 ? a : ain[m][i]; }
    t->c.op = t->c.bias = t->c.scale = t->c.out = 0; t->c.clamp = 1;
    t->a.op = t->a.bias = t->a.scale = t->a.out = 0; t->a.clamp = 1;
}
void sdk_GXSetTevColor(void) { LOG(); if (R[3] < 4) set_color(gx.tev_reg[R[3]], R[4]); }
void sdk_GXSetTevColorS10(void) { LOG(); if (R[3] < 4) for (int i = 0; i < 4; i++) { s16 v = rd16(R[4] + i * 2); gx.tev_reg[R[3]][i] = v < 0 ? 0 : v > 255 ? 255 : v; } }
void sdk_GXSetTevKColor(void) { LOG(); if (R[3] < 4) set_color(gx.kcolor[R[3]], R[4]); }
void sdk_GXSetTevKColorSel(void) { LOG(); if (R[3] < 16) gx.tev[R[3]].ksel_c = R[4]; }
void sdk_GXSetTevKAlphaSel(void) { LOG(); if (R[3] < 16) gx.tev[R[3]].ksel_a = R[4]; }
void sdk_GXSetTevSwapMode(void) { LOG(); if (R[3] < 16) { gx.tev[R[3]].ras_swap = R[4] & 3; gx.tev[R[3]].tex_swap = R[5] & 3; if (R[4] & 3) ONCE("TEV raster swap %lu ignored\n", (unsigned long)(R[4] & 3)); } }
void sdk_GXSetAlphaCompare(void) { LOG(); gx.alpha_cmp[0] = R[3]; gx.alpha_cmp[1] = R[4]; gx.alpha_cmp[2] = R[5]; gx.alpha_cmp[3] = R[6]; gx.alpha_cmp[4] = R[7]; }

/* ---------------------------------------------------------------- pixel engine */
void sdk_GXSetBlendMode(void) { LOG(); gx.blend_type = R[3]; gx.blend_src = R[4]; gx.blend_dst = R[5]; gx.logic_op = R[6]; }
void sdk_GXSetZMode(void) { LOG(); gx.z_enable = R[3]; gx.z_func = R[4]; gx.z_update = R[5]; }
void sdk_GXSetColorUpdate(void) { LOG(); gx.color_update = R[3]; }
void sdk_GXSetAlphaUpdate(void) { LOG(); gx.alpha_update = R[3]; }

/* ---------------------------------------------------------------- copy */
void sdk_GXSetDispCopySrc(void) { LOG(); if (R[5] && R[6]) { gx.efb_w = R[5]; gx.efb_h = R[6]; } }
void sdk_GXSetCopyClear(void) { LOG(); set_color(gx.clear_color, R[3]); gx.clear_z = R[4] & 0xffffff; }
void sdk_GXCopyTex(void) {
    LOG();
    u32 a = GUEST(R[3]);
    for (int i = 0; i < 8; i++) if (copy_dst[i] == a) return;
    for (int i = 0; i < 8; i++) if (!copy_dst[i]) { copy_dst[i] = a; return; }
    ONCE("more than 8 EFB copy destinations\n");
}

void sdk_GXCopyDisp(void) {
    LOG();
    frame_end();
    if (R[4]) clear_pending = 1;
}
