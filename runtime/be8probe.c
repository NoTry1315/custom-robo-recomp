/* BE8 feasibility probe (sdmc:/cr_bench.txt, at startup; unknowns "BE8 big-endian data mode"):
 * 1. what `setend` costs on this core: a loop of 8 x (setend be; setend le) against the same loop with nops;
 * 2. that loads in big-endian data mode read guest RAM bytes as the guest does (ldr, ldrh, vldr of a float and
 *    of a double), with no rev;
 * 3. whether the process can make a page of its own .text writable (svcControlProcessMemory MEMOP_PROT): a BE8
 *    build would have to byte-swap the relocated words in .text literal pools once at startup.
 * Writes "# be8 ..." lines only; changes nothing (the .text word is written back with its own value). */
#include <3ds.h>
#include "ppc.h"

#define N 200000
static u64 loop_setend(void) {
    u32 n = N;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "setend be\nsetend le\nsetend be\nsetend le\nsetend be\nsetend le\nsetend be\nsetend le\n"
        "subs %0, %0, #1\nbne 1b\n" : "+r"(n) : : "cc", "memory");
    return svcGetSystemTick() - t0;
}
static u64 loop_nop(void) {
    u32 n = N;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
        "subs %0, %0, #1\nbne 1b\n" : "+r"(n) : : "cc", "memory");
    return svcGetSystemTick() - t0;
}
/* a call-boundary-shaped sequence: setend le, a load of a host word, setend be, a big-endian load */
static u64 loop_switch_load(const u32 *host, const u32 *guest) {
    u32 n = N, a, b;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "setend be\nldr %1, [%4]\nsetend le\nldr %2, [%3]\n"
        "setend be\nldr %1, [%4]\nsetend le\nldr %2, [%3]\n"
        "subs %0, %0, #1\nbne 1b\n" : "+r"(n), "=&r"(a), "=&r"(b) : "r"(host), "r"(guest) : "cc", "memory");
    return svcGetSystemTick() - t0;
}
static u64 loop_load(const u32 *host, const u32 *guest) {
    u32 n = N, a, b;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "nop\nldr %1, [%4]\nnop\nldr %2, [%3]\n"
        "nop\nldr %1, [%4]\nnop\nldr %2, [%3]\n"
        "subs %0, %0, #1\nbne 1b\n" : "+r"(n), "=&r"(a), "=&r"(b) : "r"(host), "r"(guest) : "cc", "memory");
    return svcGetSystemTick() - t0;
}

void recomp_be8_probe(void) {
    /* 2: big-endian loads of guest-order bytes */
    static const u8 g[16] __attribute__((aligned(8))) = { 0x11, 0x22, 0x33, 0x44, 0x3f, 0x80, 0x00, 0x00,
                                                          0x40, 0x09, 0x21, 0xfb, 0x54, 0x44, 0x2d, 0x18 };
    u32 w; u16 h; float f; double d;
    __asm__ volatile("setend be\nldr %0, [%4]\nldrh %1, [%4]\nvldr %2, [%4, #4]\nvldr %P3, [%4, #8]\nsetend le\n"
                     : "=&r"(w), "=&r"(h), "=&t"(f), "=&w"(d) : "r"(g) : "memory");
    recomp_log("# be8 loads in BE mode: ldr %08lx ldrh %04x vldr.f32 %08lx vldr.f64 %016llx (want 11223344 1122 3f800000 400921fb54442d18)\n",
               (unsigned long)w, h, (unsigned long)f2u(f), d2u(d));
    /* 1: setend cost */
    u64 a = loop_nop(), b = loop_setend();
    static u32 host_w = 1;
    u64 c = loop_load(&host_w, (const u32 *)g), e = loop_switch_load(&host_w, (const u32 *)g);
    double tick_ns = 1e9 / SYSCLOCK_ARM11, per = 8.0 * N;
    recomp_log("# be8 setend: loop of 8 nops %.2f ns per instruction, 8 setends %.2f ns per setend (%.2f cycles at 804 MHz); "
               "4 loads with nops between %.2f ns per pair, with setend be/le between %.2f ns per pair\n",
               a * tick_ns / per, b * tick_ns / per, b * tick_ns / per * 0.804, c * tick_ns / (4.0 * N), e * tick_ns / (4.0 * N));
    /* 3: own .text page writable? */
    u32 page = (u32)(uintptr_t)&recomp_be8_probe & ~0xfffu;
    Handle ph = 0;
    Result r0 = svcControlProcessMemory(CUR_PROCESS_HANDLE, page, page, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    Result rd = 0, r1 = 0, r2 = 0;
    if (r0) {
        rd = svcDuplicateHandle(&ph, CUR_PROCESS_HANDLE);
        if (!rd) r1 = svcControlProcessMemory(ph, page, page, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    }
    int ok = !r0 || (!rd && !r1);
    if (ok) {
        volatile u32 *p = (volatile u32 *)page;
        u32 v = p[0];
        p[0] = v;   /* the same value: no code change, no I-cache invalidation needed */
        r2 = svcControlProcessMemory(r0 ? ph : CUR_PROCESS_HANDLE, page, page, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_EXECUTE);
    }
    if (ph) svcCloseHandle(ph);
    recomp_log("# be8 .text page %08lx RWX: with CUR_PROCESS_HANDLE %08lx, duplicate handle %08lx then %08lx; %s; back to RX %08lx\n",
               (unsigned long)page, (unsigned long)r0, (unsigned long)rd, (unsigned long)r1,
               ok ? "word written back" : "not writable", (unsigned long)r2);
}
