/* OS module: arena, OSAlloc heaps, time, interrupts, reports.
 * Behaviour follows the Dolphin SDK 2003 OS library as run by Dolphin;
 * heap cells live in guest memory with the SDK's layout (prev, next, size,
 * 32-byte header) so addresses handed to the game match. */
#include <3ds.h>
#include <stdio.h>
#include "ppc.h"

#define LOG() recomp_sdk_log(__func__ + 4)
#define ROUNDUP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ROUNDDOWN(x, a) ((x) & ~((a) - 1))

static u32 arena_lo, arena_hi;

/* OSInit: arena from boot info, else the linker's __ArenaLo / 0x81700000 */
void sdk_OSInit(void) {
    LOG();
    arena_lo = rd32(0x80000030) ? rd32(0x80000030) : ROUNDUP(STACK_TOP, 32);
    arena_hi = rd32(0x80000034) ? rd32(0x80000034) : 0x81700000u;
    recomp_log("# OSInit: arena %08lx..%08lx\n", (unsigned long)arena_lo, (unsigned long)arena_hi);
}

void sdk_OSGetArenaLo(void) { LOG(); R[3] = arena_lo; }
void sdk_OSGetArenaHi(void) { LOG(); R[3] = arena_hi; }
void sdk_OSSetArenaLo(void) { LOG(); arena_lo = R[3]; }
void sdk_OSSetArenaHi(void) { LOG(); arena_hi = R[3]; }

void sdk_OSAllocFromArenaLo(void) {
    LOG();
    u32 size = R[3], align = R[4];
    u32 p = ROUNDUP(arena_lo, align);
    arena_lo = ROUNDUP(p + size, align);
    R[3] = p;
}

/* ---- OSAlloc: cell = {prev, next, size}, header 0x20, min object 0x40 ---- */
#define HDR 0x20u
#define MINOBJ 0x40u
#define MAXHEAPS 64
static struct { s32 size; u32 free, alloc; } heaps[MAXHEAPS];
static int nheaps;
/* __OSCurrHeap is a game-visible sdata variable (r13-0x5660, read inline by
 * the OSAlloc macro), so it lives in guest memory */
#define OS_CURR_HEAP 0x8044ed60u

static u32 c_prev(u32 c) { return rd32(c); }
static u32 c_next(u32 c) { return rd32(c + 4); }
static u32 c_size(u32 c) { return rd32(c + 8); }

static u32 dl_add_front(u32 list, u32 c) { wr32(c + 4, list); wr32(c, 0); if (list) wr32(list, c); return c; }
static u32 dl_extract(u32 list, u32 c) {
    if (c_next(c)) wr32(c_next(c), c_prev(c));
    if (!c_prev(c)) return c_next(c);
    wr32(c_prev(c) + 4, c_next(c));
    return list;
}
/* address-ordered insert with coalescing (OSAlloc.c DLInsert) */
static u32 dl_insert(u32 list, u32 c) {
    u32 prev = 0, next = list;
    while (next && next < c) { prev = next; next = c_next(next); }
    wr32(c + 4, next); wr32(c, prev);
    if (next) {
        wr32(next, c);
        if (c + c_size(c) == next) {
            wr32(c + 8, c_size(c) + c_size(next));
            next = c_next(next);
            wr32(c + 4, next);
            if (next) wr32(next, c);
        }
    }
    if (prev) {
        wr32(prev + 4, c);
        if (prev + c_size(prev) == c) {
            wr32(prev + 8, c_size(prev) + c_size(c));
            wr32(prev + 4, next);
            if (next) wr32(next, prev);
        }
        return list;
    }
    return c;
}

void sdk_OSInitAlloc(void) {
    LOG();
    u32 lo = R[3], max = R[5];
    nheaps = max < MAXHEAPS ? (int)max : MAXHEAPS;
    if ((int)max > MAXHEAPS) recomp_log("# OSInitAlloc: %lu heaps, runtime keeps %d\n", (unsigned long)max, MAXHEAPS);
    for (int i = 0; i < nheaps; i++) heaps[i].size = -1, heaps[i].free = heaps[i].alloc = 0;
    wr32(OS_CURR_HEAP, (u32)-1);
    R[3] = ROUNDUP(lo + 12 * max, 32);   /* HeapDesc array (12 bytes each) stays in the arena */
}

void sdk_OSCreateHeap(void) {
    LOG();
    u32 start = ROUNDUP(R[3], 32), end = ROUNDDOWN(R[4], 32);
    for (int i = 0; i < nheaps; i++) {
        if (heaps[i].size >= 0) continue;
        wr32(start, 0); wr32(start + 4, 0); wr32(start + 8, end - start);
        heaps[i].size = (s32)(end - start);
        heaps[i].free = start;
        heaps[i].alloc = 0;
        R[3] = (u32)i;
        return;
    }
    R[3] = (u32)-1;
}

void sdk_OSDestroyHeap(void) { LOG(); if (R[3] < (u32)nheaps) heaps[R[3]].size = -1; }
void sdk_OSSetCurrentHeap(void) { LOG(); u32 old = rd32(OS_CURR_HEAP); wr32(OS_CURR_HEAP, R[3]); R[3] = old; }

void sdk_OSAllocFromHeap(void) {
    LOG();
    s32 h = (s32)R[3];
    u32 size = ROUNDUP(R[4] + HDR, 32), c;
    if (h < 0 || h >= nheaps || heaps[h].size < 0) { recomp_log("# OSAllocFromHeap: bad heap %ld\n", (long)h); R[3] = 0; return; }
    for (c = heaps[h].free; c; c = c_next(c)) if (size <= c_size(c)) break;
    if (!c) { recomp_log("# OSAllocFromHeap: heap %ld out of memory for %08lx\n", (long)h, (unsigned long)R[4]); R[3] = 0; return; }
    u32 left = c_size(c) - size;
    if (left < MINOBJ) {
        heaps[h].free = dl_extract(heaps[h].free, c);
    } else {
        u32 n = c + size;
        wr32(c + 8, size);
        wr32(n + 8, left);
        wr32(n, c_prev(c));
        wr32(n + 4, c_next(c));
        if (c_next(n)) wr32(c_next(n), n);
        if (c_prev(n)) wr32(c_prev(n) + 4, n); else heaps[h].free = n;
    }
    heaps[h].alloc = dl_add_front(heaps[h].alloc, c);
    R[3] = c + HDR;
}

void sdk_OSFreeToHeap(void) {
    LOG();
    s32 h = (s32)R[3];
    u32 c = R[4] - HDR;
    if (h < 0 || h >= nheaps || heaps[h].size < 0) { recomp_log("# OSFreeToHeap: bad heap %ld\n", (long)h); return; }
    heaps[h].alloc = dl_extract(heaps[h].alloc, c);
    heaps[h].free = dl_insert(heaps[h].free, c);
}

/* ---- time: Gekko timebase = bus clock / 4 = 40.5 MHz ---- */
u64 recomp_timebase(void) {
    u64 t = svcGetSystemTick();
    const u64 host = SYSCLOCK_ARM11, tb = 40500000;
    return t / host * tb + t % host * tb / host;
}
void sdk_OSGetTime(void) { LOG(); u64 t = recomp_timebase(); R[3] = (u32)(t >> 32); R[4] = (u32)t; }
void sdk_OSGetTick(void) { LOG(); R[3] = (u32)recomp_timebase(); }

/* ---- interrupts: MSR[EE] only, nothing is delivered asynchronously yet ---- */
void sdk_OSDisableInterrupts(void) { LOG(); R[3] = (cpu.msr >> 15) & 1; cpu.msr &= ~0x8000u; }
void sdk_OSEnableInterrupts(void) { LOG(); R[3] = (cpu.msr >> 15) & 1; cpu.msr |= 0x8000u; }
void sdk_OSRestoreInterrupts(void) {
    LOG();
    u32 old = (cpu.msr >> 15) & 1;
    if (R[3]) cpu.msr |= 0x8000u; else cpu.msr &= ~0x8000u;
    R[3] = old;
}

void sdk_OSGetConsoleType(void) { LOG(); R[3] = rd32(0x8000002c); }

/* ---- OSReport / OSPanic: printf subset over the guest varargs ---- */
static void guest_str(char *out, int cap, u32 a) {
    int j = 0;
    while (j < cap - 1 && a && rd8(a + j)) { out[j] = (char)rd8(a + j); j++; }
    out[j] = 0;
}

static void guest_format(char *out, int cap, u32 fmt, int gpr) {
    int o = 0, fpr = 1;
    for (u32 p = fmt; o < cap - 32; p++) {
        char ch = (char)rd8(p);
        if (!ch) break;
        if (ch != '%') { out[o++] = ch; continue; }
        char spec[16]; int k = 0;
        spec[k++] = '%';
        while (k < 12) { ch = (char)rd8(++p); if (!ch || !strchr("lh", ch)) spec[k++] = ch; if (!ch || strchr("diuxXcspfgeE%", ch)) break; }
        spec[k] = 0;
        if (!ch) break;
        if (ch == '%') { out[o++] = '%'; continue; }
        u32 v = gpr <= 10 ? R[gpr] : 0;
        if (ch == 's') {
            char s[128];
            guest_str(s, sizeof s, v); gpr++;
            o += snprintf(out + o, cap - o, "%s", s);
        } else if (strchr("fgeE", ch)) {
            o += snprintf(out + o, cap - o, spec, fpr <= 8 ? F[fpr].ps0 : 0.0); fpr++;
        } else {
            spec[k - 1] = 0;
            char fs[24]; snprintf(fs, sizeof fs, "%sl%c", spec, ch == 'p' ? 'x' : ch);
            o += snprintf(out + o, cap - o, fs, (long)(s32)v); gpr++;
        }
        if (o > cap - 32) o = cap - 32;
    }
    out[o] = 0;
}

void sdk_OSReport(void) {
    char buf[512];
    LOG();
    guest_format(buf, sizeof buf, R[3], 4);
    recomp_log("OSReport: %s%s", buf, strchr(buf, '\n') ? "" : "\n");
}

void sdk_OSPanic(void) {
    char buf[512], file[64];
    LOG();
    guest_str(file, sizeof file, R[3]);
    guest_format(buf, sizeof buf, R[5], 6);
    recomp_log("OSPanic: %s:%ld: %s\n", file, (long)R[4], buf);
    recomp_done("OSPanic");
}

/* ---- caches. The locked cache is plain RAM at LC_BASE (ppc.h); a DMA between it and main
 * RAM is a copy that finishes at once, so LCQueueWait has nothing to wait for. ---- */
static u32 hid2;
void sdk_PPCMfhid2(void) { LOG(); R[3] = hid2; }
void sdk_LCEnable(void) { LOG(); hid2 |= 0x10000000u; }    /* HID2[LCE] */
void sdk_LCDisable(void) { LOG(); hid2 &= ~0x10000000u; }
void sdk_LCQueueWait(void) { LOG(); }

/* main RAM range [a, a+n) without wrapping the mirror */
static int ram_range(u32 a, u32 n) { return !IS_HW(a) && (a & RAM_MASK) + n <= RAM_MASK + 1; }
static void copy_lines(u32 d, u32 s, u32 n) {
    if (ram_range(d, n) && ram_range(s, n)) { recomp_mark_ram(d, n); memmove(ram + (d & RAM_MASK), ram + (s & RAM_MASK), n); return; }
    for (u32 i = 0; i < n; i += 4) wr32(d + i, rd32(s + i));
}

/* LCStoreData(dst r3, lc src r4, n r5) -> number of DMA transactions (128 lines each);
 * whole 32-byte lines, addresses taken as given (the SDK masks them to 32 bytes) */
void sdk_LCStoreData(void) {
    LOG();
    u32 lines = (R[5] + 31) >> 5;
    copy_lines(R[3] & ~31u, R[4] & ~31u, lines << 5);
    R[3] = (lines + 127) >> 7;
}
/* LCStoreBlocks(dst r3, lc src r4, blocks r5): 0 blocks = 128 */
void sdk_LCStoreBlocks(void) {
    LOG();
    u32 lines = R[5] & 0x7f ? R[5] & 0x7f : 128;
    copy_lines(R[3] & ~31u, R[4] & ~31u, lines << 5);
}

/* DCZeroRange(a r3, n r4): dcbz every line touching [a, a+n) */
void sdk_DCZeroRange(void) {
    LOG();
    u32 a = R[3] & ~31u, n = R[4];
    if (!n) return;
    n = ((n + (R[3] & 31) + 31) >> 5) << 5;
    if (ram_range(a, n)) { recomp_mark_ram(a, n); memset(ram + (a & RAM_MASK), 0, n); return; }
    for (u32 i = 0; i < n; i += 4) wr32(a + i, 0);
}

/* SRAM sound mode (flags bit 2): stereo, as in Dolphin's default SRAM. The game reads it at
 * boot (0x8009921c) and otherwise mixes in mono, e.g. the THP movie's downmix (0x80017a0c). */
static u32 sound_mode = 1;
void sdk_OSGetSoundMode(void) { LOG(); R[3] = sound_mode; }
void sdk_OSSetSoundMode(void) { LOG(); sound_mode = R[3] & 1; }   /* rlwinm r3, 2, 29, 29 */

/* reset button never pressed; the game latches a reset request on non-zero */
void sdk_OSGetResetButtonState(void) { LOG(); R[3] = 0; }
