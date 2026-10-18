/* Run log (sdmc:/cr_autotest.log): every SDK call, recomp_bad, hardware
 * access; a summary and the closing "# DONE" line that autotest.ps1 waits for.
 * A watchdog thread ends the log if nothing new is logged for HANG_MS: the
 * guest is stuck, either silent or polling the same SDK calls. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include "ppc.h"

#define LOG_PATH "sdmc:/cr_autotest.log"
#define PER_NAME 20         /* calls logged in full per SDK function */
#define HANG_MS 20000      /* a 30-retrace log gap on the title's 3D background in real-time Citra exceeds 4 s */

static FILE *log_fp;
static LightLock lock;
static volatile u32 heartbeat;
static const char *volatile ring[32];
static volatile u32 ring_pos;
jmp_buf recomp_exit_jmp;

static struct { const char *name; u32 n; } counts[2048];

static u32 *count_of(const char *name) {
    u32 h = (u32)(uintptr_t)name * 2654435761u >> 21;
    for (;; h = (h + 1) & 2047) {
        if (!counts[h].name) counts[h].name = name;
        if (counts[h].name == name) return &counts[h].n;
    }
}

void recomp_log(const char *fmt, ...) {
    va_list ap;
    if (!log_fp) return;
    LightLock_Lock(&lock);
    heartbeat++;
    va_start(ap, fmt); vfprintf(log_fp, fmt, ap); va_end(ap);
    fflush(log_fp);
    LightLock_Unlock(&lock);
}

void recomp_sdk_log(const char *name) {
    u32 *n = count_of(name);
    ring[ring_pos++ & 31] = name;
    if (++*n <= PER_NAME)
        recomp_log("sdk %s r3=%08lx r4=%08lx r5=%08lx r6=%08lx lr=%08lx\n", name, (unsigned long)R[3],
                   (unsigned long)R[4], (unsigned long)R[5], (unsigned long)R[6], (unsigned long)cpu.lr);
}

void recomp_sdk_call(const char *name) { recomp_sdk_log(name); }

void recomp_bad(u32 pc, const char *why) {
    u32 *n = count_of(why);
    if (++*n <= PER_NAME) recomp_log("recomp_bad %08lx: %s (lr=%08lx)\n", (unsigned long)pc, why, (unsigned long)cpu.lr);
}

static int by_count(const void *a, const void *b) {
    const u32 *x = a, *y = b;
    return (int)y[1] - (int)x[1];
}

static void summary(void) {
    static u32 rows[2048][2];
    int k = 0;
    for (int i = 0; i < 2048; i++) if (counts[i].name) { rows[k][0] = i; rows[k][1] = counts[i].n; k++; }
    qsort(rows, k, sizeof rows[0], by_count);
    recomp_log("# summary: %d distinct names (SDK calls and recomp_bad reasons)\n", k);
    for (int i = 0; i < k; i++) recomp_log("#  %8lu %s\n", (unsigned long)rows[i][1], counts[rows[i][0]].name);
    recomp_log("# last calls:");
    for (int i = 0; i < 32; i++) { const char *s = ring[(ring_pos + i) & 31]; if (s) recomp_log(" %s", s); }
    recomp_log("\n# lr=%08lx r1=%08lx\n", (unsigned long)cpu.lr, (unsigned long)R[1]);
}

static volatile int finished;

void recomp_done(const char *why) {
    if (!finished) {
        finished = 1;
        summary();
        recomp_ax_close();
        recomp_log("# DONE %s\n", why);
    }
    longjmp(recomp_exit_jmp, 1);
}

static void watchdog(void *arg) {
    (void)arg;
    u32 last = heartbeat - 1;
    while (!finished) {
        svcSleepThread((s64)HANG_MS * 1000000);
        if (finished) break;
        if (heartbeat == last) {
            finished = 1;
            recomp_log("# nothing new logged for %d ms: guest hang\n", HANG_MS);
            summary();
            recomp_log("# DONE hang\n");
            break;
        }
        last = heartbeat;
    }
}

void recomp_log_open(void) {
    LightLock_Init(&lock);
    log_fp = fopen(LOG_PATH, "w");
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    threadCreate(watchdog, NULL, 0x2000, prio - 1, -2, true);
}

void recomp_log_close(void) {
    finished = 1;
    if (log_fp) fclose(log_fp);
    log_fp = NULL;
}
