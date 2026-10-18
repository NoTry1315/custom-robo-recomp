/* DOL loader and the boot info block at 0x80000000.
 * main.dol and boot.bin come from the user's disc via sdmc:/custom-robo/
 * (never from the repository). Boot info values follow Dolphin's HLE BS2
 * (Core/Boot/Boot_BS2Emu.cpp) plus what the real apploader writes; the open
 * ones are listed in docs/recomp_unknowns.md. */
#include <stdio.h>
#include <stdlib.h>
#include "ppc.h"

#define DISC_DIR "sdmc:/custom-robo/"

static u8 *read_file(const char *path, u32 *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = malloc(n ? n : 1);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = (u32)n;
    return b;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void boot_info(const u8 *bootbin) {
    u32 fst_max = be32(bootbin + 0x42c);
    u32 fst = (0x81800000u - fst_max) & ~31u;   /* apploader: FST at the top of 24 MB */
    memcpy(ram, bootbin, 0x20);                 /* disc ID, maker, version, ... */
    wr32(0x8000001c, 0xc2339f3d);               /* GC disc magic */
    wr32(0x80000020, 0x0d15ea5e);               /* booted from the boot ROM */
    wr32(0x80000024, 1);
    wr32(0x80000028, 0x01800000);               /* physical memory size, 24 MB */
    wr32(0x8000002c, 0x10000006);               /* console type: Dolphin uses LatestDevkit */
    wr32(0x80000030, 0);                        /* arena lo: 0 -> OSInit uses __ArenaLo */
    wr32(0x80000034, fst);                      /* arena hi = FST address */
    wr32(0x80000038, fst);                      /* FST location (contents: phase 4, DVD) */
    wr32(0x8000003c, fst_max);
    wr32(0x800000cc, 0);                        /* VI mode from IPL: NTSC */
    wr32(0x800000d0, 0x01000000);               /* ARAM size, 16 MB */
    wr32(0x800000f8, 0x09a7ec80);               /* bus clock, 162 MHz */
    wr32(0x800000fc, 0x1cf7c580);               /* CPU clock, 486 MHz */
}

int recomp_load_dol(void) {
    u32 n, bn;
    u8 *d = read_file(DISC_DIR "main.dol", &n);
    u8 *b = read_file(DISC_DIR "boot.bin", &bn);
    if (!d || n < 0x100 || !b || bn < 0x440) {
        recomp_log("# DOL: need " DISC_DIR "main.dol and boot.bin from the disc\n");
        return -1;
    }
    /* bss first: its range covers the small-data sections loaded below */
    u32 bss = be32(d + 0xd8), bss_size = be32(d + 0xdc);
    memset(ram + (bss & RAM_MASK), 0, bss_size);
    recomp_log("# DOL: bss %08lx+%lx, entry %08lx\n", (unsigned long)bss, (unsigned long)bss_size, (unsigned long)be32(d + 0xe0));
    for (int i = 0; i < 18; i++) {
        u32 off = be32(d + 4 * i), addr = be32(d + 0x48 + 4 * i), size = be32(d + 0x90 + 4 * i);
        if (!size) continue;
        if (off + size > n) { recomp_log("# DOL: section %d past end of file\n", i); return -1; }
        memcpy(ram + (addr & RAM_MASK), d + off, size);
        recomp_log("# DOL: section %2d %08lx..%08lx\n", i, (unsigned long)addr, (unsigned long)(addr + size));
    }
    boot_info(b);
    free(d);
    free(b);
    return 0;
}
