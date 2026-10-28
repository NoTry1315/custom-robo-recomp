/* Bottom screen (320x240, 40x30 console cells of 8x8): build id + sdmc flags, a fixed stats
 * table (log.c, once per 60-retrace window), a 3-row message area (printf), and a grid of
 * touch buttons, one per live A/B toggle. Game thread only (consoleSelect is not
 * thread-safe); a cell is redrawn only when its text changed (the console draws on the CPU).
 *
 *   row  0     build <id>
 *   rows 1-2   flags: <sdmc flag files present at boot, cr_ prefix dropped>
 *   rows 4-12  stats, one metric per row (STAT_ROWS)
 *   rows 14-16 messages (printf scrolls here)
 *   rows 18-29 buttons: 2 columns x 4 rows, each 19x2 cells + 1 gap row (more than 8 buttons:
 *              up to 6 rows without the gap row, BTN_PITCH)
 *
 * Adding a button: one line in btn[] below. Either get/toggle functions (the toggle is a
 * request the owner applies at a safe point, as gx.c and ax.c do), or a flag variable bound at
 * start with recomp_menu_bind(name, &var): the menu then flips *var itself in
 * recomp_menu_retrace (called from recomp_gx_retrace after the GX thread synced, so no GX
 * record is in flight) and logs "# menu r<n>: <name> on|off" (tools/time_avg.py drops the
 * windows with one). inv = 1 for "_off" flags: the button shows ON while *var is 0. An action
 * button (count set) has no state: it shows TAP and how often it ran. */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "ppc.h"

static PrintConsole *con_msg, con_ui;
static int menu_up;   /* recomp_menu_init ran */

#define BTN_ROW0 18
#define BTN_W 19

static int vt_get(void) { return recomp_vt_on; }

static struct btn {
    const char *name, *hint;      /* label, second line before the state */
    int (*get)(void);             /* 1 on, 0 off, -1 unavailable */
    void (*toggle)(void);         /* request; applied by the owner */
    int inv;                      /* bound var: shown ON while *var == 0 */
    int *var;                     /* recomp_menu_bind */
    void (*changed)(void);        /* recomp_menu_bind: called after *var flipped (game thread) */
    int req, shown;
    u32 *count;                   /* action button (no on/off): shows TAP and how often it ran */
} btn[] = {
    { "VRAM textures", "",   vt_get,   recomp_vt_toggle, 0, 0, 0, 0, -2 },   /* cr_vramtex_off */
    { "Vertex pack",   "",   0,        0,                1, 0, 0, 0, -2 },   /* cr_vtxpack_off, bound by gx.c (plans/cache entries are keyed by the mode) */
    { "Lazy vtx desc", "",   0,        0,                1, 0, 0, 0, -2 },   /* cr_lazydesc_off, bound by gx.c */
    { "Native sin/cos", "",  0,        0,                1, 0, 0, 0, -2 },   /* cr_libm_off (libm.c binds it) */
    { "Ptr extent",    "",   0,        0,                0, 0, 0, 0, -2 },   /* recomp_ptr_extent, cr_ptrext_off (main.c binds it) */
    { "Inline sites",  "",   0,        0,                0, 0, 0, 0, -2 },   /* recomp_inline_sites, cr_inline_off (main.c binds it) */
    { "Dump RAM",      "",   0,  recomp_memdump_request, 0, 0, 0, 0, -2, &recomp_memdump_n },   /* memdump.c: guest RAM + ARAM to SD */
    { "Perf counters", "",   0,        0,                0, 0, 0, 0, -2 },   /* recomp_perfctr_on, cr_perfctr (perfctr.c binds it) */
    { "Widescreen",    "",   0,        0,                0, 0, 0, 0, -2 },   /* recomp_widescreen, cr_widescreen_off (gx.c binds it) */
    { "Native THP",    "",   0,        0,                1, 0, 0, 0, -2 },   /* cr_thpnative_off (thp.c binds it) */
};
#define NBTN (int)(sizeof btn / sizeof btn[0])
#define BTN_PITCH (NBTN <= 8 ? 3 : 2)   /* rows per button row: 12 rows (18-29) hold 4 with gaps, 6 without */

static int btn_state(const struct btn *b) {
    if (b->count) return (int)*b->count;
    if (b->var) return (*b->var != 0) ^ b->inv;
    return b->get ? b->get() : -1;
}

void recomp_menu_bind(const char *name, int *var, void (*changed)(void)) {
    for (int i = 0; i < NBTN; i++)
        if (!strcmp(btn[i].name, name)) { btn[i].var = var; btn[i].changed = changed; return; }
    recomp_log("# menu: no button %s to bind\n", name);
}

static void at(int row, int col) { printf("\x1b[%d;%dH", row + 1, col + 1); }   /* window-relative; setting cursorX directly lost a row's first character */

static void btn_draw(int i) {
    struct btn *b = &btn[i];
    int s = btn_state(b);
    if (s == b->shown) return;
    b->shown = s;
#ifdef RECOMP_CARDUI
    recomp_card_button(i, NBTN, b->name, s, b->count != 0, b->count ? s : 0);
    return;
#endif
    int row = BTN_ROW0 + (i / 2) * BTN_PITCH, col = (i % 2) * 20;
    /* ON: black on green; OFF: white on red; unavailable: plain, "--"; action: black on white, TAP */
    const char *c = b->count ? "\x1b[30;47m" : s > 0 ? "\x1b[30;42m" : s == 0 ? "\x1b[37;41m" : "\x1b[0m";
    char l1[BTN_W + 1], l2[BTN_W + 1], st[8], hint[BTN_W];
    snprintf(st, sizeof st, "%s", b->count ? "TAP" : s > 0 ? "ON" : s == 0 ? "OFF" : "--");
    if (b->count) snprintf(hint, sizeof hint, "%d done", s);
    else snprintf(hint, sizeof hint, "%s", b->hint);
    snprintf(l1, sizeof l1, " %-*.*s", BTN_W - 1, BTN_W - 1, b->name);
    snprintf(l2, sizeof l2, " %-4.4s %*.*s", st, BTN_W - 6, BTN_W - 6, hint);
    consoleSelect(&con_ui);
    at(row, col); printf("%s%s", c, l1);
    at(row + 1, col); printf("%s\x1b[0m", l2);
    consoleSelect(con_msg);
}

/* PADRead (game thread): a new touch inside a button requests its toggle */
void recomp_menu_touch(int px, int py) {
#ifdef RECOMP_CARDUI
    int i = recomp_card_hit(px, py, NBTN);
    if (i < 0) return;
#else
    int col = px / 8, row = py / 8;
    if (row < BTN_ROW0) return;
    int r = row - BTN_ROW0, c = col / 20;
    if (r % BTN_PITCH == 2 || col % 20 >= BTN_W) return;   /* gaps */
    int i = r / BTN_PITCH * 2 + c;
    if (i >= NBTN) return;
#endif
    struct btn *b = &btn[i];
    if (b->var) b->req ^= 1;
    else if (b->toggle && (b->count || btn_state(b) >= 0)) b->toggle();
}

/* every retrace, after the GX thread synced (recomp_gx_retrace) */
u32 recomp_menu_flips;
void recomp_menu_retrace(void) {
    if (!menu_up) return;
    for (int i = 0; i < NBTN; i++) {
        struct btn *b = &btn[i];
        if (b->req && b->var) {
            b->req = 0; *b->var ^= 1; recomp_menu_flips++;
            recomp_log("# menu r%lu: %s %s (touch)\n", (unsigned long)recomp_retrace_count(), b->name, btn_state(b) ? "on" : "off");
            if (b->changed) b->changed();
        }
        btn_draw(i);
    }
}

/* stats rows (log.c), rows STAT_ROW0.. ; redrawn only when changed */
static char shown[RECOMP_STAT_ROWS][40];
void recomp_status_row(int row, const char *s) {
    char t[40];
    if (!con_msg || row < 0 || row >= RECOMP_STAT_ROWS) return;
    snprintf(t, sizeof t, "%-39.39s", s);   /* 39: a 40th character would wrap to the next row */
    if (!strcmp(t, shown[row])) return;
    strcpy(shown[row], t);
    consoleSelect(&con_ui);
    at(4 + row, 0);
    printf("%s", t);
    consoleSelect(con_msg);
}

/* the four stat tiles of the card layout; the console layout shows the stats rows instead */
void recomp_menu_stats(double fps, double game, double gx, double audio) {
#ifdef RECOMP_CARDUI
    if (menu_up) recomp_card_stats(fps, game, gx, audio);
#else
    (void)fps; (void)game; (void)gx; (void)audio;
#endif
}

void recomp_menu_init(void) {
    menu_up = 1;
#ifdef RECOMP_CARDUI
    recomp_card_init();
    return;
#endif
    con_msg = consoleInit(GFX_BOTTOM, NULL);
    con_ui = *con_msg;
    consoleSetWindow(&con_ui, 0, 0, 40, 30);
    consoleSetWindow(con_msg, 0, 15, 40, 3);   /* libctru draws a window one row above its y (Citra: y 14 printed over row 13) */
    consoleSelect(&con_ui);
    at(3, 0); printf("\x1b[36m------------- stats (1/s) -------------\x1b[0m");
    at(13, 0); printf("\x1b[36m--------------- messages --------------\x1b[0m");
    at(17, 0); printf("\x1b[36m------- toggles (tap, logged) ---------\x1b[0m");
    consoleSelect(con_msg);
}

/* row 0-2: build id and the sdmc flag files present at boot */
void recomp_menu_header(const char *build, const char *flags) {
#ifdef RECOMP_CARDUI
    recomp_card_header(build, flags);
    return;
#endif
    char t[80];
    consoleSelect(&con_ui);
    at(0, 0); printf("\x1b[33mbuild\x1b[0m %-33.33s", build);
    snprintf(t, sizeof t, "%-78.78s", flags);
    at(1, 0); printf("\x1b[33mflags\x1b[0m %-33.33s", t);
    at(2, 0); printf("      %-33.33s", t + 33);
    consoleSelect(con_msg);
}
