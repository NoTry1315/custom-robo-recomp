/* GX module -> PICA200 (citro3d).
 *
 * GX state is kept host-side, set by the hand-written GX calls below. Vertex
 * data reaches us as a byte stream: GXBegin pushes the draw command (opcode,
 * vertex count) and the game then writes the vertices to the write-gather
 * pipe 0xCC008000 itself (inline GXPosition* etc.), see gx_fifo_write. A
 * primitive is decoded and drawn as soon as its last byte arrives.
 *
 * The GX transform, lighting and texgen run on the CPU (Dolphin's software
 * vertex loader semantics); the PICA gets clip-space positions already mapped
 * from the EFB to the 400x240 top screen, so its vertex shader is a
 * pass-through. TEV stages are translated to PICA texenv stages; what the
 * PICA cannot express is logged once (docs/recomp_unknowns.md, "GX"). */
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include "ppc.h"
#include "vshader_shbin.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define ONCE(...) do { static int once_; if (!once_) { once_ = 1; recomp_log("# gx: " __VA_ARGS__); } } while (0)
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
#define VB_VERTS 0xC000   /* per frame half; the title tunnel is ~33k vertices */
typedef struct { float pos[4], clr[4], tc[3][2]; } PVtx;

static C3D_RenderTarget *target;
static DVLB_s *vsh_dvlb;
static shaderProgram_s vsh;
static PVtx *vbuf[2];
static u32 vb_used, vb_half;
static int frame_open, clear_pending = 1;
static u32 frames;

void recomp_gx_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
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
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
    vbuf[0] = linearAlloc(sizeof(PVtx) * VB_VERTS * 2);
    vbuf[1] = vbuf[0] + VB_VERTS;
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vbuf[0], sizeof(PVtx), 5, 0x43210);
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

static void frame_begin(void) {
    if (frame_open) return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameDrawOn(target);
    frame_open = 1;
    vb_half = frames & 1; vb_used = 0;
    if (clear_pending) {
        u32 c = gx.clear_color[0] << 24 | gx.clear_color[1] << 16 | gx.clear_color[2] << 8 | gx.clear_color[3];
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, c, gx.clear_z);
        clear_pending = 0;
    }
}

static u32 st_draws, st_tris, st_tex;

static void frame_end(void) {
    frame_begin();
    if (frames % 300 == 0) {   /* draw stats every 300 copies */
        recomp_log("# gx frame %lu: draws %lu tris %lu textured %lu efb %dx%d clear %02x%02x%02x%02x\n", (unsigned long)frames,
            (unsigned long)st_draws, (unsigned long)st_tris, (unsigned long)st_tex, gx.efb_w, gx.efb_h,
            gx.clear_color[0], gx.clear_color[1], gx.clear_color[2], gx.clear_color[3]);
    }
    st_draws = st_tris = st_tex = 0;
    /* a frame with no draws still shows the clear color (frame_begin above) */
    u64 t0 = recomp_ticks();
    C3D_FrameEnd(0);
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

typedef struct { u32 img, tlut_addr, hash, last; u16 w, h; u8 fmt, tlut_fmt, used; C3D_Tex tex; } TexEntry;
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

/* morton offset inside an 8x8 PICA tile */
static u32 morton(u32 x, u32 y) {
    u32 m = 0;
    for (int i = 0; i < 3; i++) m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
    return m;
}

static C3D_Tex *tex_bind_params(TexEntry *e, const TexMap *m, float *su, float *sv);

static C3D_Tex *tex_get(const TexMap *m, float *su, float *sv) {
    if (!m->img || !tile_w[m->fmt & 15] || !m->w || !m->h) return NULL;
    const Tlut *tl = m->ci ? &gx.tlut[m->tlut < 20 ? m->tlut : 0] : NULL;
    /* hashed once per frame: the title draws one big texture as many strips */
    for (int i = 0; i < TEX_CACHE; i++) {
        TexEntry *t = &tcache[i];
        if (t->used && t->last == frames && t->img == m->img && t->w == m->w && t->h == m->h && t->fmt == m->fmt &&
            t->tlut_addr == (tl ? tl->addr : 0)) return tex_bind_params(t, m, su, sv);
    }
    u32 n = tex_bytes(m->w, m->h, m->fmt), hash = hash_bytes(m->img, n);
    if (tl) hash ^= hash_bytes(tl->addr, tl->n * 2) * 31 + tl->fmt;
    TexEntry *e = NULL, *victim = NULL;
    for (int i = 0; i < TEX_CACHE; i++) {
        TexEntry *t = &tcache[i];
        if (t->used && t->img == m->img && t->w == m->w && t->h == m->h && t->fmt == m->fmt && t->hash == hash) { e = t; break; }
        if (!t->used) { if (!victim || victim->used) victim = t; }
        else if ((!victim || (victim->used && t->last < victim->last)) && t->last + 2 < frames) victim = t;
    }
    if (!e) {
        if (!victim) { ONCE("texture cache full\n"); return NULL; }
        e = victim;
        if (e->used) C3D_TexDelete(&e->tex);
        u32 W = pow2(m->w), H = pow2(m->h);
        if (!C3D_TexInit(&e->tex, W, H, GPU_RGBA8)) { e->used = 0; ONCE("C3D_TexInit failed\n"); return NULL; }
        if (!decode_buf) decode_buf = malloc(1024 * 1024 * 4);
        int w = m->w > 1024 ? 1024 : m->w, h = m->h > 1024 ? 1024 : m->h;
        tex_decode(decode_buf, m->img, w, h, m->fmt, tl);
        u32 *dst = e->tex.data;
        /* GX row y is PICA memory row y, sampled at v = 1 - y/H (see emit); padding repeats the edge */
        for (u32 y = 0; y < H; y++) for (u32 x = 0; x < W; x++) {
            u32 c = decode_buf[(y < (u32)h ? y : h - 1) * w + (x < (u32)w ? x : w - 1)];
            u32 r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
            dst[((y >> 3) * (W >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7)] = a | b << 8 | g << 16 | r << 24;
        }
        GSPGPU_FlushDataCache(dst, W * H * 4);
        e->img = m->img; e->w = m->w; e->h = m->h; e->fmt = m->fmt; e->hash = hash; e->used = 1;
    }
    e->last = frames;
    e->tlut_addr = tl ? tl->addr : 0;
    return tex_bind_params(e, m, su, sv);
}

static C3D_Tex *tex_bind_params(TexEntry *e, const TexMap *m, float *su, float *sv) {
    static const GPU_TEXTURE_WRAP_PARAM wrap[4] = {GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT};
    C3D_TexSetWrap(&e->tex, wrap[m->wrap_s & 3], wrap[m->wrap_t & 3]);
    C3D_TexSetFilter(&e->tex, m->mag_f ? GPU_LINEAR : GPU_NEAREST, (m->min_f == 0 || m->min_f == 2 || m->min_f == 3) ? GPU_NEAREST : GPU_LINEAR);
    *su = (float)m->w / e->tex.width; *sv = (float)m->h / e->tex.height;
    return &e->tex;
}

/* ---------------------------------------------------------------- TEV -> texenv */
static int unit_of_map[8];

typedef struct { GPU_TEVSRC src; int opnd; } Src;
static u8 buf_val[4];   /* combiner buffer color: never updated, so a second per-draw constant */
static int buf_used;  /* opnd: 0 color, 1 alpha, 2 one-minus color */

static int konst_value(int sel, int alpha, u8 rgba[4]) {
    if (sel < 8) { u8 v = (u8)((8 - sel) * 255 / 8); rgba[0] = rgba[1] = rgba[2] = rgba[3] = v; return 1; }
    if (!alpha && sel >= 0x0c && sel <= 0x0f) { memcpy(rgba, gx.kcolor[sel - 0x0c], 4); return 1; }
    if (sel >= 0x10) { u8 v = gx.kcolor[sel & 3][(sel - 0x10) >> 2]; rgba[0] = rgba[1] = rgba[2] = rgba[3] = v; return 1; }
    return 0;
}

/* GX color arg -> PICA source; constants go to the stage constant (one per stage) */
static Src color_src(const TevStage *t, int arg, int si, int *cused, u8 cval[4], int alpha) {
    Src s = {GPU_PREVIOUS, alpha};
    u8 v[4] = {0, 0, 0, 0};
    int map_unit = t->map < 8 ? unit_of_map[t->map] : -1;
    static const u8 alpha_arg[8] = {1, 3, 5, 7, 9, 11, 14, 15};   /* GX_CA_* -> GX_CC_* */
    if (alpha) arg = alpha_arg[arg & 7];
    switch (arg) {
    case 0: case 1:
        s.opnd = arg & 1;
        if (si == 0) { memcpy(v, gx.tev_reg[0], 4); goto konst; }
        s.src = GPU_PREVIOUS; return s;
    case 2: case 3: case 4: case 5: case 6: case 7:
        memcpy(v, gx.tev_reg[(arg - 2) / 2 + 1], 4); s.opnd = alpha ? 1 : (arg & 1); goto konst;
    case 8: case 9:
        if (map_unit < 0) { ONCE("TEXC without texture\n"); v[0] = v[1] = v[2] = v[3] = 255; goto konst; }
        s.src = (GPU_TEVSRC)(GPU_TEXTURE0 + map_unit); s.opnd = alpha ? 1 : (arg & 1);
        if (!alpha && !(arg & 1) && t->tex_swap) s.opnd = 2 + t->tex_swap;   /* default swap tables 1-3: RRR, GGG, BBB */
        if (t->tex_swap) ONCE("TEV texture swap %d (alpha kept)\n", t->tex_swap);
        return s;
    case 10: case 11: s.src = GPU_PRIMARY_COLOR; s.opnd = alpha ? 1 : (arg & 1); return s;
    case 12: v[0] = v[1] = v[2] = v[3] = 255; s.opnd = alpha; goto konst;
    case 13: v[0] = v[1] = v[2] = v[3] = 128; s.opnd = alpha; goto konst;
    case 14: konst_value(alpha ? t->ksel_a : t->ksel_c, alpha, v); s.opnd = alpha; goto konst;
    default: v[0] = v[1] = v[2] = v[3] = 0; s.opnd = alpha; goto konst;
    }
konst:
    s.src = GPU_CONSTANT;
    if (alpha) {
        u8 a = v[s.opnd ? 3 : 0];
        if (s.opnd == 0) a = v[0];
        s.opnd = 1;
        if ((*cused & 2) && cval[3] != a) {
            if ((buf_used & 2) && buf_val[3] != a) ONCE("three different alpha constants in one draw\n");
            buf_val[3] = a; buf_used |= 2; s.src = GPU_PREVIOUS_BUFFER;
        } else { cval[3] = a; *cused |= 2; }
    } else {
        if (s.opnd == 1) { v[0] = v[1] = v[2] = v[3]; }
        s.opnd = 0;
        if ((*cused & 1) && memcmp(cval, v, 3)) {   /* second constant: the combiner buffer color */
            if ((buf_used & 1) && memcmp(buf_val, v, 3)) ONCE("three different color constants in one draw\n");
            memcpy(buf_val, v, 3); buf_used |= 1; s.src = GPU_PREVIOUS_BUFFER;
        } else { memcpy(cval, v, 3); *cused |= 1; }
    }
    return s;
}

/* one GX side (color or alpha) -> up to 2 PICA ops */
typedef struct { GPU_COMBINEFUNC f; Src s[3]; int scale; } POp;

static int is_const(int arg, int alpha, int val) {   /* ZERO/ONE literal args */
    if (alpha) return val == 0 ? arg == 7 : 0;
    return val == 0 ? arg == 15 : val == 1 ? arg == 12 : 0;
}

static int side_ops(const TevStage *t, int si, int alpha, POp *ops, int *cused, u8 cval[4]) {
    const TevSide *g = alpha ? &t->a : &t->c;
    int z_a = is_const(g->a, alpha, 0), z_b = is_const(g->b, alpha, 0), z_c = is_const(g->c, alpha, 0), z_d = is_const(g->d, alpha, 0);
    int one_c = !alpha && is_const(g->c, 0, 1);
    Src A = color_src(t, g->a, si, cused, cval, alpha), B = color_src(t, g->b, si, cused, cval, alpha);
    Src C = color_src(t, g->c, si, cused, cval, alpha), D = color_src(t, g->d, si, cused, cval, alpha);
    int n = 0, scale = g->scale == 1 ? 1 : g->scale == 2 ? 2 : 0;
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
    else l = (POp){GPU_INTERPOLATE, {B, A, C}};
    if (z_d) { l.scale = scale; if (g->op == 1) ONCE("TEV 0 - x\n"); ops[n++] = l; return n; }
    if (g->op == 0 && l.f == GPU_MODULATE) { ops[n++] = (POp){GPU_MULTIPLY_ADD, {B, C, D}, scale}; return n; }
    if (l.f == GPU_REPLACE) {
        ops[n++] = (POp){g->op ? GPU_SUBTRACT : GPU_ADD, {g->op ? D : l.s[0], g->op ? l.s[0] : D, D}, scale};
        return n;
    }
    ops[n++] = l;
    Src prev = {GPU_PREVIOUS, alpha};
    if (D.src == GPU_PREVIOUS) ONCE("TEV lerp + CPREV needs the combiner buffer\n");
    ops[n++] = (POp){g->op ? GPU_SUBTRACT : GPU_ADD, {g->op ? D : prev, g->op ? prev : D, D}, scale};
    return n;
}

static int opnd_rgb(int o) {
    static const int m[6] = {GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B};
    return m[o < 6 ? o : 0];
}

static void setup_tev(void) {
    int pstage = 0;
    buf_used = 0;
    for (u32 si = 0; si < gx.n_stages && si < 16; si++) {
        const TevStage *t = &gx.tev[si];
        if (t->c.out || t->a.out) ONCE("TEV output to REG%d not supported (stage %lu)\n", t->c.out ? t->c.out : t->a.out, (unsigned long)si);
        POp co[2], ao[2];
        int cused = 0; u8 cval[4] = {0, 0, 0, 0};
        int nc = side_ops(t, si, 0, co, &cused, cval), na = side_ops(t, si, 1, ao, &cused, cval);
        int n = nc > na ? nc : na;
        for (int k = 0; k < n; k++) {
            if (pstage >= 6) { ONCE("more than 6 PICA texenv stages needed\n"); break; }
            C3D_TexEnv *env = C3D_GetTexEnv(pstage);
            C3D_TexEnvInit(env);
            POp c = k < nc ? co[k] : (POp){GPU_REPLACE, {{GPU_PREVIOUS, 0}}}, a = k < na ? ao[k] : (POp){GPU_REPLACE, {{GPU_PREVIOUS, 1}}};
            C3D_TexEnvSrc(env, C3D_RGB, c.s[0].src, c.s[1].src, c.s[2].src);
            C3D_TexEnvOpRgb(env, opnd_rgb(c.s[0].opnd), opnd_rgb(c.s[1].opnd), opnd_rgb(c.s[2].opnd));
            C3D_TexEnvFunc(env, C3D_RGB, c.f);
            C3D_TexEnvScale(env, C3D_RGB, (GPU_TEVSCALE)(k + 1 == nc ? c.scale : 0));
            C3D_TexEnvSrc(env, C3D_Alpha, a.s[0].src, a.s[1].src, a.s[2].src);
            C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(env, C3D_Alpha, a.f);
            C3D_TexEnvScale(env, C3D_Alpha, (GPU_TEVSCALE)(k + 1 == na ? a.scale : 0));
            C3D_TexEnvColor(env, cval[0] | cval[1] << 8 | cval[2] << 16 | (u32)cval[3] << 24);
            pstage++;
        }
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_TexEnvBufColor(buf_val[0] | buf_val[1] << 8 | buf_val[2] << 16 | (u32)buf_val[3] << 24);
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

static void setup_pixel(void) {
    u32 mask = (gx.color_update ? GPU_WRITE_COLOR : 0) | (gx.alpha_update ? GPU_WRITE_ALPHA : 0);
    if (gx.z_enable) {
        if (gx.z_update) mask |= GPU_WRITE_DEPTH;
        C3D_DepthTest(true, test_func(gx.z_func), (GPU_WRITEMASK)mask);
    } else C3D_DepthTest(false, GPU_ALWAYS, (GPU_WRITEMASK)mask);
    /* GX window depth = far + (far - near) * z/w, clamped to [0, 1] by the depth buffer */
    C3D_DepthMap(true, gx.vp[5] - gx.vp[4], gx.vp[5]);
    if (gx.blend_type == 1)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, blend_factor(gx.blend_src, 0), blend_factor(gx.blend_dst, 1),
                       blend_factor(gx.blend_src, 0), blend_factor(gx.blend_dst, 1));
    else if (gx.blend_type == 3)
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    else {
        if (gx.blend_type == 2) ONCE("logic op blend %d\n", gx.logic_op);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    /* alpha compare: PICA has one test; take whichever GX comparison is not ALWAYS */
    u8 *ac = gx.alpha_cmp;
    if (ac[0] != 7 && ac[3] != 7 && !(ac[2] == 0 && (ac[0] == 7 || ac[3] == 7))) ONCE("two-sided alpha compare %d %d op %d\n", ac[0], ac[3], ac[2]);
    if (ac[0] != 7) C3D_AlphaTest(true, test_func(ac[0]), ac[1]);
    else if (ac[3] != 7) C3D_AlphaTest(true, test_func(ac[3]), ac[4]);
    else C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_CullFace(GPU_CULL_NONE);
    if (gx.cull) ONCE("cull mode %d drawn without culling\n", gx.cull);
    /* scissor: GX EFB rect -> screen -> framebuffer (240 wide, 400 tall, rotated) */
    float s = efb_scale(), x0 = efb_x0(), y0 = efb_y0();
    float l = x0 + (gx.sc[0] - gx.sc_off[0]) * s, t = y0 + (gx.sc[1] - gx.sc_off[1]) * s;
    float r = l + gx.sc[2] * s, b = t + gx.sc[3] * s;
    l = l < x0 ? x0 : l;
    t = t < y0 ? y0 : t;
    r = r > 400 - x0 ? 400 - x0 : r < l ? l : r;
    b = b > 240 - y0 ? 240 - y0 : b < t ? t : b;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - b), (u32)(400 - r), (u32)(240 - t), (u32)(400 - l));
}

/* ---------------------------------------------------------------- vertices */
typedef struct { float pos[3], nrm[3], tex[8][2]; u8 clr[2][4]; u8 pnmtx, texmtx[8]; } Vtx;

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

static float read_comp(const u8 *p, int type, int frac) {
    switch (type) {
    case 0: return p[0] / (float)(1 << frac);
    case 1: return (s8)p[0] / (float)(1 << frac);
    case 2: return (u16)(p[0] << 8 | p[1]) / (float)(1 << frac);
    case 3: return (s16)(p[0] << 8 | p[1]) / (float)(1 << frac);
    default: { u32 v = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; return u2f(v); }
    }
}

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

static void decode_vtx(const u8 *p, int fmt, Vtx *v) {
    memset(v, 0, sizeof *v);
    v->pnmtx = gx.cur_mtx;
    for (int i = 0; i < 8; i++) v->texmtx[i] = gx.tex_mtx_idx[i];
    for (int a = 0; a <= 20; a++) {
        int d = gx.desc[a];
        if (!d) continue;
        if (a == 0) { v->pnmtx = *p++; continue; }
        if (a <= 8) { v->texmtx[a - 1] = *p++; continue; }
        const VtxFmt *f = &gx.vat[fmt][a];
        const u8 *src = p;
        u8 tmp[40];
        if (d >= 2) {
            u32 idx = d == 2 ? p[0] : (u32)(p[0] << 8 | p[1]);
            p += d == 2 ? 1 : 2;
            int arr = a - A_POS;
            u32 base = GUEST(gx.array_base[arr]) + idx * gx.array_stride[arr];
            int n = attr_size(fmt, a) ? 0 : 0;
            gx.desc[a] = 1; n = attr_size(fmt, a); gx.desc[a] = d;
            for (int i = 0; i < n && i < 40; i++) tmp[i] = rd8(base + i);
            src = tmp;
        } else p += attr_size(fmt, a);
        int cs = comp_size[f->type > 4 ? 4 : f->type];
        switch (a) {
        case A_POS:
            for (int i = 0; i < (f->cnt ? 3 : 2); i++) v->pos[i] = read_comp(src + i * cs, f->type, f->frac);
            break;
        case A_NRM: {
            int fr = f->type == 1 ? 6 : f->type == 3 ? 14 : 0;
            for (int i = 0; i < 3; i++) v->nrm[i] = read_comp(src + i * cs, f->type, fr);
            break; }
        case A_CLR0: case A_CLR1: read_color(src, f->type, v->clr[a - A_CLR0]); break;
        default:
            for (int i = 0; i < (f->cnt ? 2 : 1); i++) v->tex[a - A_TEX0][i] = read_comp(src + i * cs, f->type, f->frac);
            break;
        }
    }
}

static void mtx_mul(const float *m, const float *in, float w, float *out, int rows) {
    for (int r = 0; r < rows; r++) out[r] = m[r * 4] * in[0] + m[r * 4 + 1] * in[1] + m[r * 4 + 2] * in[2] + m[r * 4 + 3] * w;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* GX lighting for one channel side: COLOR0 (rgb) or ALPHA0 (a) */
static void light_chan(int ci, const Vtx *v, const float *vpos, const float *vnrm, float *out, int comps, int first) {
    const Chan *c = &gx.chan[ci];
    int which = ci & 1;
    float mat[4], amb[4];
    for (int i = 0; i < 4; i++) {
        mat[i] = (c->mat_src ? v->clr[which][i] : gx.mat[which][i]) / 255.0f;
        amb[i] = (c->amb_src ? v->clr[which][i] : gx.amb[which][i]) / 255.0f;
    }
    for (int k = 0; k < comps; k++) {
        int i = first + k;
        float lit = 1;
        if (c->enable) {
            lit = amb[i];
            for (int l = 0; l < 8; l++) {
                if (!(c->light_mask & (1 << l))) continue;
                const Light *L = &gx.light[l];
                float d[3] = {L->pos[0] - vpos[0], L->pos[1] - vpos[1], L->pos[2] - vpos[2]};
                float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist > 0) for (int j = 0; j < 3; j++) d[j] /= dist;
                float diff = d[0] * vnrm[0] + d[1] * vnrm[1] + d[2] * vnrm[2];
                if (c->diff_fn == 0) diff = 1; else if (c->diff_fn == 2 && diff < 0) diff = 0;
                float attn = 1;
                if (c->attn_fn == 1) {   /* GX_AF_SPOT */
                    float cs = -(d[0] * L->dir[0] + d[1] * L->dir[1] + d[2] * L->dir[2]);
                    float num = L->a[0] + L->a[1] * cs + L->a[2] * cs * cs; if (num < 0) num = 0;
                    float den = L->k[0] + L->k[1] * dist + L->k[2] * dist * dist;
                    attn = den > 0 ? num / den : 0;
                } else if (c->attn_fn == 0) {   /* GX_AF_SPEC: approximated as no attenuation */
                    ONCE("specular light attenuation approximated\n");
                }
                lit += diff * attn * L->color[i] / 255.0f;
            }
            lit = clamp01(lit);
        }
        out[i] = mat[i] * lit;
    }
}

static void emit(const Vtx *v, PVtx *o, const float tscale[3][2], const int tcoord[3]) {
    const float *pm = &gx.xf[(v->pnmtx & 63) * 4];
    float ep[3], en[3] = {0, 0, 0};
    mtx_mul(pm, v->pos, 1, ep, 3);
    if (gx.desc[A_NRM]) {
        const float *nm = &gx.xf[0x400 + (v->pnmtx & 63) * 3];
        for (int r = 0; r < 3; r++) en[r] = nm[r * 3] * v->nrm[0] + nm[r * 3 + 1] * v->nrm[1] + nm[r * 3 + 2] * v->nrm[2];
        float l = sqrtf(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
        if (l > 0) for (int i = 0; i < 3; i++) en[i] /= l;
    }
    /* projection (GX packed form) */
    const float *p = gx.proj;
    float cx, cy, cz, cw;
    if (gx.proj_ortho) { cx = p[0] * ep[0] + p[1]; cy = p[2] * ep[1] + p[3]; cz = p[4] * ep[2] + p[5]; cw = 1; }
    else { cx = p[0] * ep[0] + p[1] * ep[2]; cy = p[2] * ep[1] + p[3] * ep[2]; cz = p[4] * ep[2] + p[5]; cw = -ep[2]; }
    /* viewport: ndc -> EFB pixel, then EFB -> screen pixel -> rotated framebuffer clip space */
    float s = efb_scale();
    float ex = gx.vp[0] + gx.vp[2] * 0.5f * (1 + cx / cw) - gx.sc_off[0];
    float ey = gx.vp[1] + gx.vp[3] * 0.5f * (1 - cy / cw) - gx.sc_off[1];
    float sx = efb_x0() + ex * s, sy = efb_y0() + ey * s;
    o->pos[0] = (1 - sy / 120.0f) * cw;
    o->pos[1] = (1 - sx / 200.0f) * cw;
    o->pos[2] = cz;   /* GX and PICA both clip z to [-w, 0]; depth map in setup_pixel */
    o->pos[3] = cw;
    /* color channel 0 */
    float c[4] = {0, 0, 0, 0};
    if (gx.n_chans) { light_chan(0, v, ep, en, c, 3, 0); light_chan(2, v, ep, en, c, 1, 3); }
    memcpy(o->clr, c, sizeof c);
    /* texgen */
    for (int u = 0; u < 3; u++) {
        o->tc[u][0] = o->tc[u][1] = 0;
        int tci = tcoord[u];
        if (tci < 0 || tci >= (int)gx.n_texgens) continue;
        const TexGen *g = &gx.texgen[tci];
        float in[3] = {0, 0, 1}, w = 1;
        if (g->src == 0) { memcpy(in, v->pos, sizeof in); }
        else if (g->src == 1) { memcpy(in, v->nrm, sizeof in); w = 0; }
        else if (g->src >= 4 && g->src <= 11) { in[0] = v->tex[g->src - 4][0]; in[1] = v->tex[g->src - 4][1]; in[2] = 1; }
        else { ONCE("texgen src %d not supported\n", g->src); }
        float out[3] = {in[0], in[1], 1};
        u32 mid = gx.desc[1 + tci] ? v->texmtx[tci] : g->mtx;
        if (g->func <= 1 && mid != 60) {
            const float *m = &gx.xf[(mid & 63) * 4];
            if (g->src >= 4) mtx_mul(m, in, 1, out, g->func == 0 ? 3 : 2);
            else mtx_mul(m, in, w, out, g->func == 0 ? 3 : 2);
            if (g->func == 1) out[2] = 1;
        } else if (g->func > 1) ONCE("texgen func %d not supported\n", g->func);
        if (g->func == 0 && out[2] != 0 && out[2] != 1) { out[0] /= out[2]; out[1] /= out[2]; ONCE("projective texgen divided per vertex\n"); }
        o->tc[u][0] = out[0] * tscale[u][0];
        o->tc[u][1] = 1 - out[1] * tscale[u][1];   /* PICA v = 1 is memory row 0 */
    }
}

static void draw(int prim, int fmt, const u8 *data, int n) {
    if (prim >= 0xa8) { ONCE("lines/points (0x%x) not drawn\n", prim); return; }
    frame_begin();
    /* textures: GX texmaps used by the TEV stages -> PICA units 0..2 */
    int nunits = 0, tcoord[3] = {-1, -1, -1};
    float tscale[3][2] = {{1, 1}, {1, 1}, {1, 1}};
    for (int i = 0; i < 8; i++) unit_of_map[i] = -1;
    for (u32 si = 0; si < gx.n_stages; si++) {
        const TevStage *t = &gx.tev[si];
        if (t->map >= 8 || unit_of_map[t->map] >= 0) continue;
        if (nunits >= 3) { ONCE("more than 3 texmaps in one draw\n"); continue; }
        u64 t0 = recomp_ticks();
        C3D_Tex *tex = tex_get(&gx.map[t->map], &tscale[nunits][0], &tscale[nunits][1]);
        recomp_prof[PROF_GX_TEX] += recomp_ticks() - t0;
        if (!tex) continue;
        C3D_TexBind(nunits, tex);
        tcoord[nunits] = t->coord < 8 ? t->coord : -1;
        unit_of_map[t->map] = nunits++;
    }
    setup_tev();
    setup_pixel();
    int ntri = prim == 0x80 ? n / 4 * 2 : prim == 0x90 ? n / 3 : n >= 3 ? n - 2 : 0;
    if (!ntri) return;
    if (vb_used + ntri * 3 > VB_VERTS) { ONCE("vertex buffer full\n"); return; }
    static Vtx vt[0x10000 / 4];
    static PVtx pv[0x10000 / 4];
    int vs = vtx_size(fmt);
    if (n > (int)(sizeof vt / sizeof vt[0])) n = sizeof vt / sizeof vt[0];
    u64 t0 = recomp_ticks();
    for (int i = 0; i < n; i++) { decode_vtx(data + i * vs, fmt, &vt[i]); emit(&vt[i], &pv[i], tscale, tcoord); }
    recomp_prof[PROF_GX_VTX] += recomp_ticks() - t0;
    PVtx *o = vbuf[vb_half] + vb_used;
    int k = 0;
    for (int t = 0; t < ntri; t++) {
        int a, b, c;
        if (prim == 0x80) { int q = (t / 2) * 4; if (t & 1) { a = q; b = q + 2; c = q + 3; } else { a = q; b = q + 1; c = q + 2; } }
        else if (prim == 0x90) { a = 3 * t; b = a + 1; c = a + 2; }
        else if (prim == 0x98) { a = t; b = t + 1; c = t + 2; if (t & 1) { int x = a; a = b; b = x; } }
        else { a = 0; b = t + 1; c = t + 2; }
        o[k++] = pv[a]; o[k++] = pv[b]; o[k++] = pv[c];
    }
    st_draws++; st_tris += ntri; if (nunits) st_tex++;
    GSPGPU_FlushDataCache(o, k * sizeof(PVtx));
    C3D_DrawArrays(GPU_TRIANGLES, vb_half * VB_VERTS + vb_used, k);
    vb_used += k;
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
    depth++;
    u32 used = parse_cmds(ram + (GUEST(addr) & RAM_MASK), n);
    if (used < n) ONCE("display list with a truncated command\n");
    depth--;
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
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++)
        if (addr + r * cols + c < 0x600) gx.xf[addr + r * cols + c] = u2f(rd32(src + (r * cols + c) * 4));
}
void sdk_GXLoadPosMtxImm(void) { LOG(); load_mtx(R[3], R[4] * 4, 3, 4); }
void sdk_GXLoadNrmMtxImm(void) {   /* source is a 3x4 matrix, the 3x3 part is used */
    LOG();
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
void sdk_GXCopyDisp(void) {
    LOG();
    frame_end();
    if (R[4]) clear_pending = 1;
}
