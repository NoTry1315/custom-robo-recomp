/* Bottom screen (320x240, 40x30 console cells of 8x8): build id + sdmc flags, a fixed stats
 * table (log.c, once per 60-retrace window) and a 3-row message area (printf). Game thread only (consoleSelect is not
 * thread-safe); a cell is redrawn only when its text changed (the console draws on the CPU).
 *
 *   row  0     build <id>
 *   rows 1-2   flags: <sdmc flag files present at boot, cr_ prefix dropped>
 *   rows 4-12  stats, one metric per row (STAT_ROWS)
 *   rows 14-16 messages (printf scrolls here)
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "ppc.h"

static PrintConsole *con_msg, con_ui;

static void at(int row, int col) { printf("\x1b[%d;%dH", row + 1, col + 1); }   /* window-relative; setting cursorX directly lost a row's first character */

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

void recomp_menu_init(void) {
    con_msg = consoleInit(GFX_BOTTOM, NULL);
    con_ui = *con_msg;
    consoleSetWindow(&con_ui, 0, 0, 40, 30);
    consoleSetWindow(con_msg, 0, 15, 40, 3);   /* libctru draws a window one row above its y (Citra: y 14 printed over row 13) */
    consoleSelect(&con_ui);
    at(3, 0); printf("\x1b[36m------------- stats (1/s) -------------\x1b[0m");
    at(13, 0); printf("\x1b[36m--------------- messages --------------\x1b[0m");
    consoleSelect(con_msg);
}

/* row 0-2: build id and the sdmc flag files present at boot */
void recomp_menu_header(const char *build, const char *flags) {
    char t[80];
    consoleSelect(&con_ui);
    at(0, 0); printf("\x1b[33mbuild\x1b[0m %-33.33s", build);
    snprintf(t, sizeof t, "%-78.78s", flags);
    at(1, 0); printf("\x1b[33mflags\x1b[0m %-33.33s", t);
    at(2, 0); printf("      %-33.33s", t + 33);
    consoleSelect(con_msg);
}
