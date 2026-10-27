/* Card layout of the bottom screen (`make CARDUI=1`, menu.c calls these instead of its console):
 * title, build id, four stat tiles (fps, game, GX, audio) and the toggle buttons as rounded cards
 * with an on/off dot. Drawn on the CPU into the single-buffered RGB565 framebuffer that
 * consoleInit set up, with the 3DS system font (shared font, A4 glyph sheets, box-filtered down
 * to the text size), only where something changed. printf output (boot stages, audio mode) is
 * the dim footer line. One lock around all drawing: printf may come from any thread.
 *
 *   y   8-25   title            y  27-38  build id + sdmc flags
 *   y  46-75   stat tiles (4)   y  86-..  buttons, 2 columns, row step 35 (less if > 8 buttons)
 *   y 226-237  last printf line */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include "ppc.h"

#define SCR_W 320
#define SCR_H 240
#define MARGIN 12        /* side margin */
#define TILE_Y 46
#define TILE_H 30
#define BTN_Y 86
#define BTN_BOTTOM 220
#define FOOT_Y 226

static u16 *fb;                  /* 240x320, column-major: pixel (x, y) at x * 240 + 239 - y */
static LightLock lock;
static CFNT_s *font;             /* NULL: no system font, text is not drawn */
static TGLP_s *tglp;

typedef struct { u8 r, g, b; } rgb;
static const rgb C_BG = {14, 20, 38};   /* flat: a gradient bands in RGB565 */
static const rgb C_TEXT = {242, 244, 248}, C_DIM = {128, 137, 158}, C_FOOT = {92, 101, 122};
static const rgb C_TILE = {30, 37, 54};
static const rgb C_ON_BG = {19, 41, 107}, C_ON_EDGE = {91, 127, 224}, C_ON_DOT = {121, 230, 160};
static const rgb C_OFF_BG = {28, 33, 48}, C_OFF_EDGE = {58, 65, 82}, C_OFF_DOT = {92, 101, 121};

static void blend(int x, int y, rgb c, int a) {   /* a 0..256 */
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H || a <= 0) return;
    u16 *p = &fb[x * 240 + 239 - y];
    if (a < 256) {
        int r = (*p >> 11) << 3, g = ((*p >> 5) & 63) << 2, b = (*p & 31) << 3;
        c.r = (u8)(r + ((c.r - r) * a >> 8)); c.g = (u8)(g + ((c.g - g) * a >> 8)); c.b = (u8)(b + ((c.b - b) * a >> 8));
    }
    *p = (u16)((c.r >> 3) << 11 | (c.g >> 2) << 5 | c.b >> 3);
}

static void fill_bg(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) blend(x, y, C_BG, 256);
}

/* rounded rectangle, anti-aliased edge (signed distance to the rounded box) */
static void rrect(float x0, float y0, float w, float h, float r, rgb c) {
    float cx = x0 + w * 0.5f, cy = y0 + h * 0.5f, hw = w * 0.5f - r, hh = h * 0.5f - r;
    for (int y = (int)y0; y < (int)ceilf(y0 + h); y++)
        for (int x = (int)x0; x < (int)ceilf(x0 + w); x++) {
            float qx = fabsf(x + 0.5f - cx) - hw, qy = fabsf(y + 0.5f - cy) - hh;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
            float cov = 0.5f - (sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r);
            if (cov > 0) blend(x, y, c, cov >= 1 ? 256 : (int)(cov * 256));
        }
}

static void dot(float cx, float cy, float r, rgb c) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov > 0) blend(x, y, c, cov >= 1 ? 256 : (int)(cov * 256));
        }
}

/* ---------------------------------------------------------------- text (system font) */
static const u8 mx[8] = {0, 1, 4, 5, 16, 17, 20, 21}, my[8] = {0, 2, 8, 10, 32, 34, 40, 42};   /* 8x8 tile Z order */

static int sheet_alpha(const u8 *sh, int x, int y) {   /* A4 texel 0..15 */
    if (x < 0 || y < 0 || x >= tglp->sheetWidth || y >= tglp->sheetHeight) return 0;
    int i = ((y >> 3) * (tglp->sheetWidth >> 3) << 6) + ((x >> 3) << 6) + mx[x & 7] + my[y & 7];
    return (sh[i >> 1] >> ((i & 1) * 4)) & 15;
}

enum { T_BOLD = 1, T_TABULAR = 2 };

/* ASCII glyphs, spaced by their ink (columns with any alpha) rather than the font's advances: those are
 * for full size and uneven at half size (Citra's replacement font more so) */
#define GAP 2.4f   /* sheet texels between two glyphs' ink */
static struct glyph { const u8 *sh; s16 sx0, sy0, ink0, ink1; u8 adv; } gl[128];
static int digit_w;   /* widest digit ink, for tabular numbers */

static void glyphs_init(void) {
    int per = tglp->nRows * tglp->nLines;
    for (int ch = ' '; ch < 127; ch++) {
        struct glyph *g = &gl[ch];
        int gi = fontGlyphIndexFromCodePoint(font, ch), in = gi % per;
        g->sh = fontGetGlyphSheetTex(font, gi / per);
        g->sx0 = in % tglp->nRows * (tglp->cellWidth + 1) + 1;
        g->sy0 = in / tglp->nRows * (tglp->cellHeight + 1) + 1;
        charWidthInfo_s *cw = fontGetCharWidthInfo(font, gi);
        g->adv = cw->charWidth;
        g->ink0 = tglp->cellWidth; g->ink1 = -1;
        for (int u = 0; u < cw->glyphWidth && u < tglp->cellWidth; u++)   /* past glyphWidth a cell can hold stray texels */
            for (int v = 0; v < tglp->cellHeight; v++)
                if (sheet_alpha(g->sh, g->sx0 + u, g->sy0 + v)) { if (u < g->ink0) g->ink0 = u; g->ink1 = u; break; }
        if (ch >= '0' && ch <= '9' && g->ink1 - g->ink0 + 1 > digit_w) digit_w = g->ink1 - g->ink0 + 1;
    }
}

/* width of one glyph's box (ink, or a tabular digit cell) in sheet texels; 0 for no ink */
static int glyph_w(int ch, int fl, int bold) {
    const struct glyph *g = &gl[ch];
    if (g->ink1 < 0) return 0;
    return ((fl & T_TABULAR) && ch >= '0' && ch <= '9' ? digit_w : g->ink1 - g->ink0 + 1) + bold;
}
static int bold_of(float s, int fl) { return (fl & T_BOLD) ? (int)(0.6f / s + 0.5f) : 0; }   /* emboldening, in texels */

static float text_w(const char *t, float size, int fl) {
    if (!font) return 0;
    float s = size / tglp->cellHeight, w = 0;
    int bold = bold_of(s, fl);
    for (; *t; t++) {
        int ch = (u8)*t < 127 ? (u8)*t : '?', gw = glyph_w(ch, fl, bold);
        w += gw ? (gw + GAP) * s : gl[ch].adv * s;
    }
    return w;
}

/* size = glyph cell height in pixels; y = baseline; text right of xmax is cut */
static float text(float x, float y, const char *t, float size, rgb c, int fl, int xmax) {
    if (!font) return x;
    float s = size / tglp->cellHeight, inv = 1 / s;
    int bold = bold_of(s, fl);
    float top = floorf(y - tglp->baselinePos * s + 0.5f);
    for (; *t; t++) {
        int ch = (u8)*t < 127 ? (u8)*t : '?', gw = glyph_w(ch, fl, bold);
        const struct glyph *g = &gl[ch];
        if (!gw) { x += g->adv * s; continue; }
        int iw = g->ink1 - g->ink0 + 1 + bold;   /* drawn columns */
        float gx = floorf(x + (gw - iw) * 0.5f * s + 0.5f);   /* whole pixels: stems stay sharp */
        if (gx + iw * s > xmax) break;
        for (int dy = (int)top; dy < (int)ceilf(top + tglp->cellHeight * s); dy++)
            for (int dx = (int)gx; dx < (int)ceilf(gx + iw * s); dx++) {
                /* box filter: the destination pixel's footprint on the sheet, plus `bold` texels to its left */
                float u0 = (dx - gx) * inv, u1 = u0 + inv, v0 = (dy - top) * inv, v1 = v0 + inv;
                float sum = 0, area = 0;
                for (int v = (int)floorf(v0); v < (int)ceilf(v1); v++) {
                    float wy = fminf(v1, v + 1) - fmaxf(v0, v);
                    for (int u = (int)floorf(u0); u < (int)ceilf(u1); u++) {
                        float wx = fminf(u1, u + 1) - fmaxf(u0, u);
                        int a = 0;
                        if (v >= 0 && v < tglp->cellHeight)
                            for (int k = 0; k <= bold; k++) {
                                int uu = u - k;
                                if (uu >= 0 && uu <= g->ink1 - g->ink0) { int t2 = sheet_alpha(g->sh, g->sx0 + g->ink0 + uu, g->sy0 + v); if (t2 > a) a = t2; }
                            }
                        sum += wx * wy * a; area += wx * wy;
                    }
                }
                if (sum > 0) blend(dx, dy, c, (int)(sum / area * (256.0f / 15)));
            }
        x += (gw + GAP) * s;
    }
    return x;
}

/* ---------------------------------------------------------------- layout */
static int tile_x(int i) { return MARGIN + i * 76; }   /* 4 tiles of 69 + 7 gap */
#define TILE_W 69
#define BTN_W 145
static int btn_step(int n) { int rows = (n + 1) / 2, st = (BTN_BOTTOM - BTN_Y + 6) / (rows ? rows : 1); return st > 35 ? 35 : st; }
static int btn_x(int i) { return i % 2 ? SCR_W - MARGIN - BTN_W : MARGIN; }
static int btn_y(int i, int n) { return BTN_Y + i / 2 * btn_step(n); }

static const char *const tile_label[4] = {"fps", "game", "GX", "audio"};
static char tile_shown[4][16];

static void tile_value(int i, const char *v) {
    if (!strcmp(v, tile_shown[i])) return;
    snprintf(tile_shown[i], sizeof tile_shown[i], "%s", v);
    int x = tile_x(i);
    rrect(x + 3, TILE_Y + 15, TILE_W - 6, 13, 0, C_TILE);   /* value area only */
    text(x + 7, TILE_Y + 25, v, 15, C_TEXT, T_BOLD | T_TABULAR, x + TILE_W - 3);
}

static void tiles(void) {
    for (int i = 0; i < 4; i++) {
        int x = tile_x(i);
        rrect(x, TILE_Y, TILE_W, TILE_H, 4, C_TILE);
        text(x + 7, TILE_Y + 11, tile_label[i], 11, C_DIM, 0, x + TILE_W - 3);
        tile_shown[i][0] = 0;
        tile_value(i, "--");
    }
}

/* ---------------------------------------------------------------- printf -> footer */
static char foot[64], foot_cur[64];
static int foot_n;

static void foot_draw(void) {
    fill_bg(0, FOOT_Y, SCR_W, SCR_H - FOOT_Y);
    text(MARGIN, FOOT_Y + 9, foot, 11, C_FOOT, 0, SCR_W - MARGIN);
}

static ssize_t foot_write(struct _reent *r, void *fd, const char *p, size_t n) {
    (void)r; (void)fd;
    LightLock_Lock(&lock);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') { foot_cur[foot_n] = 0; strcpy(foot, foot_cur); foot_n = 0; foot_draw(); }
        else if (foot_n < (int)sizeof foot_cur - 1 && (u8)p[i] >= ' ') foot_cur[foot_n++] = p[i];
    }
    LightLock_Unlock(&lock);
    return (ssize_t)n;
}
static const devoptab_t foot_dev = { .name = "card", .write_r = foot_write };

/* ---------------------------------------------------------------- menu.c entry points */
void recomp_card_init(void) {
    LightLock_Init(&lock);
    consoleInit(GFX_BOTTOM, NULL);   /* screen format, single buffer, stdout unbuffered */
    devoptab_list[STD_OUT] = &foot_dev;
    fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    if (R_SUCCEEDED(fontEnsureMapped()) && (font = fontGetSystemFont()) && (tglp = fontGetGlyphInfo(font))->sheetFmt != GPU_A4) font = NULL;
    if (font) glyphs_init();
    LightLock_Lock(&lock);
    fill_bg(0, 0, SCR_W, SCR_H);
    text(MARGIN, 22, "custom-robo-recomp", 21, C_TEXT, T_BOLD, SCR_W - MARGIN);
    tiles();
    LightLock_Unlock(&lock);
}

/* build id without its build time (the last three words), then the sdmc flags */
void recomp_card_header(const char *build, const char *flags) {
    char b[96];
    recomp_log("# menu: card layout (CARDUI), system font %s\n", font ? "mapped" : "missing: no text");   /* the log opens after init */
    if (font) recomp_log("# menu: font cell %dx%d baseline %d\n", tglp->cellWidth, tglp->cellHeight, tglp->baselinePos);
    snprintf(b, sizeof b, "%s", build);
    for (int k = 0; k < 3; k++) { char *sp = strrchr(b, ' '); if (sp) *sp = 0; }
    LightLock_Lock(&lock);
    fill_bg(0, 27, SCR_W, 13);
    float x = text(MARGIN, 36, b, 13, C_DIM, 0, SCR_W - MARGIN);
    if (strcmp(flags, "none")) text(x + 8, 36, flags, 13, C_FOOT, 0, SCR_W - MARGIN);
    LightLock_Unlock(&lock);
}

void recomp_card_stats(double fps, double game, double gx, double audio) {
    char v[16];
    LightLock_Lock(&lock);
    snprintf(v, sizeof v, "%.1f", fps); tile_value(0, v);
    double ms[3] = {game, gx, audio};
    for (int i = 0; i < 3; i++) { snprintf(v, sizeof v, ms[i] < 100 ? "%.1f ms" : "%.0f ms", ms[i]); tile_value(1 + i, v); }   /* fits the tile */
    LightLock_Unlock(&lock);
}

/* button i of n: state 1 on, 0 off, -1 unavailable; action buttons have no state, count = times run */
void recomp_card_button(int i, int n, const char *name, int state, int action, int count) {
    int x = btn_x(i), y = btn_y(i, n), h = btn_step(n) - 6;
    int on = !action && state > 0;
    LightLock_Lock(&lock);
    fill_bg(x, y, BTN_W, h);
    rrect(x, y, BTN_W, h, 5, on ? C_ON_EDGE : C_OFF_EDGE);
    rrect(x + 1, y + 1, BTN_W - 2, h - 2, 4, on ? C_ON_BG : C_OFF_BG);
    if (action || state >= 0) dot(x + 11.5f, y + h * 0.5f, 3, on ? C_ON_DOT : C_OFF_DOT);
    float base = y + h * 0.5f + 4.5f;
    char c[16] = "";
    if (action && count) snprintf(c, sizeof c, "%d done", count);
    float cw = text_w(c, 11, 0);
    if (*c) text(x + BTN_W - 8 - cw, base - 0.5f, c, 11, C_DIM, 0, x + BTN_W - 4);
    text(x + 23, base, name, 15, state < 0 && !action ? C_DIM : C_TEXT, T_BOLD, x + BTN_W - 10 - (int)cw);
    LightLock_Unlock(&lock);
}

int recomp_card_hit(int px, int py, int n) {
    for (int i = 0; i < n; i++) {
        int x = btn_x(i), y = btn_y(i, n);
        if (px >= x && px < x + BTN_W && py >= y && py < y + btn_step(n) - 6) return i;
    }
    return -1;
}
