/* Guest memory snapshot to the SD card, for comparing a hardware run with Citra at the same point
 * (recomp_unknowns.md "Run 30 text hang"). Taken from the bottom-screen button "Dump RAM" (any
 * time) or at the retraces listed in sdmc:/cr_memdump.txt (one number per line; Citra replays).
 * Written at the retrace, after the GX thread synced (recomp_gx_retrace), so no GX record and no
 * guest code is running:
 *   sdmc:/cr_dump_r<n>_ram.bin    guest RAM 0x80000000.., 24 MB + the AX voices above it
 *   sdmc:/cr_dump_r<n>_aram.bin   ARAM, 16 MB
 *   sdmc:/cr_dump_r<n>.txt        retrace, GX frame, scene struct, DVD reads in flight
 *   sdmc:/cr_fb_<frame>.bmp       the next GX frame (gx.c fb dump)
 * The game stops for the write (~40 MB). */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "ppc.h"
#include "buildinfo.h"   /* RECOMP_BUILD */

#define DUMP_RAM 0x01808000u   /* 24 MB + VPB_COUNT * VPB_SIZE (ax.c voices at 0x81800000) */

static volatile int req;
static u32 want[16], nwant, ndone, read_flags;
u32 recomp_memdump_n;          /* dumps written (button state) */

void recomp_memdump_request(void) { req = 1; }

static int write_file(const char *name, const u8 *p, u32 n) {
    FILE *f = fopen(name, "wb");
    if (!f) return 0;
    u32 done = 0;
    while (done < n) {
        u32 k = n - done < 0x100000 ? n - done : 0x100000;
        if (fwrite(p + done, 1, k, f) != k) break;
        done += k;
    }
    fclose(f);
    return done == n;
}

static void dump(const char *why) {
    u32 r = recomp_retrace_count();
    u64 t0 = recomp_ticks();
    char name[64];
    printf("r%lu: dumping RAM to SD...\n", (unsigned long)r);
    recomp_gx_fb_dump_next();
    snprintf(name, sizeof name, "sdmc:/cr_dump_r%lu_ram.bin", (unsigned long)r);
    int ok_ram = write_file(name, ram, DUMP_RAM);
    snprintf(name, sizeof name, "sdmc:/cr_dump_r%lu_aram.bin", (unsigned long)r);
    int ok_aram = recomp_aram() && write_file(name, recomp_aram(), 0x01000000u);
    snprintf(name, sizeof name, "sdmc:/cr_dump_r%lu.txt", (unsigned long)r);
    FILE *f = fopen(name, "w");
    if (f) {
        fprintf(f, "build %s\nretrace %lu (%s)\ngx frames %lu\n", RECOMP_BUILD, (unsigned long)r, why, (unsigned long)recomp_gx_stat[0]);
        fprintf(f, "ram 0x80000000 + 0x%lx bytes (%s), aram 0x1000000 bytes (%s)\n", (unsigned long)DUMP_RAM,
                ok_ram ? "ok" : "FAILED", ok_aram ? "ok" : "FAILED");
        fprintf(f, "scene %08lx sub %08lx flags %08lx fade %08lx cnt %08lx\n", (unsigned long)rd32(0x80321d88),
                (unsigned long)rd32(0x80321d98), (unsigned long)rd32(0x80321dc0), (unsigned long)rd32(0x80321dc8),
                (unsigned long)rd32(0x80321db4));
        fprintf(f, "cpu msr %08lx\n", (unsigned long)cpu.msr);
        recomp_dvd_describe(f);
        fclose(f);
    }
    recomp_memdump_n++;
    double ms = (recomp_ticks() - t0) * 1000.0 / SYSCLOCK_ARM11;
    recomp_log("# memdump r%lu (%s): sdmc:/cr_dump_r%lu_*, ram %s, aram %s, %.0f ms\n", (unsigned long)r, why,
               (unsigned long)r, ok_ram ? "ok" : "FAILED", ok_aram ? "ok" : "FAILED", ms);
    printf("r%lu: RAM dump %s (%.1f s)\n", (unsigned long)r, ok_ram && ok_aram ? "written" : "FAILED", ms / 1000);
}

/* every retrace, after the GX thread synced (recomp_gx_retrace) */
void recomp_memdump_retrace(void) {
    if (!read_flags) {
        read_flags = 1;
        FILE *f = fopen("sdmc:/cr_memdump.txt", "rb");
        if (f) {
            unsigned long v;
            while (nwant < 16 && fscanf(f, "%lu", &v) == 1) want[nwant++] = v;
            fclose(f);
            recomp_log("# memdump: %lu retraces listed in cr_memdump.txt\n", (unsigned long)nwant);
        }
    }
    u32 r = recomp_retrace_count();
    if (ndone < nwant && want[ndone] == r) { ndone++; dump("cr_memdump.txt"); }
    if (req) { req = 0; dump("button"); }
}
