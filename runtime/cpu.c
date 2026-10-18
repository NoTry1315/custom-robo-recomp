/* CPU state, guest RAM and the hardware-layer hooks the translated code
 * calls. Phase 2: logging placeholders only; later phases replace them. */
#include <stdio.h>
#include <stdlib.h>
#include "ppc.h"

CPU cpu;
u8 *ram;

void cpu_init(void) {
    ram = calloc(RAM_MASK + 1 + 8, 1);
    memset(&cpu, 0, sizeof cpu);
    R[1] = STACK_TOP;
    R[2] = 0x804581a0u;
    R[13] = 0x804543c0u;
}

void recomp_skipped(u32 addr) { recomp_log("recomp_skipped %08lx lr=%08lx\n", (unsigned long)addr, (unsigned long)cpu.lr); }

/* hardware registers: not emulated in phase 3, each address logged a few times */
static int hw_seen(u32 a) {
    static u32 seen[256];
    static u8 n[256];
    u32 h = (a >> 1) & 255;
    if (seen[h] != a) { seen[h] = a; n[h] = 0; }
    return n[h] < 4 ? ++n[h] : 0;
}
u32 hw_read(u32 a, int size) {
    if (hw_seen(a)) recomp_log("hw_read %08lx/%d lr=%08lx\n", (unsigned long)a, size, (unsigned long)cpu.lr);
    return 0;
}
void hw_write(u32 a, u32 v, int size) {
    if (hw_seen(a)) recomp_log("hw_write %08lx/%d = %08lx lr=%08lx\n", (unsigned long)a, size, (unsigned long)v, (unsigned long)cpu.lr);
}
u32 ppc_mfspr(int n) { return cpu.spr[n & 1023]; }
void ppc_mtspr(int n, u32 v) { cpu.spr[n & 1023] = v; }
u32 ppc_mftb(int tbr) { u64 t = recomp_timebase(); return tbr == 269 ? (u32)(t >> 32) : (u32)t; }

/* an interrupt-style guest call (VI retrace callback etc.): the interrupted
 * code's registers are saved and restored around it, as OSContext does */
void recomp_call_guest(u32 fn, u32 arg) {
    static CPU saved;   /* not reentrant: callbacks do not nest */
    memcpy(&saved, &cpu, sizeof cpu);
    R[3] = arg;
    R[1] = (R[1] - 0x200) & ~7u;
    cpu.lr = 0;
    call_indirect(fn);
    memcpy(&cpu, &saved, sizeof cpu);
}
void ppc_mtmsr(u32 v) { cpu.msr = v; }
void ppc_sc(u32 pc) { recomp_bad(pc, "sc"); }
void ppc_rfi(u32 pc) { recomp_bad(pc, "rfi"); }
void ppc_trap(u32 pc) { recomp_bad(pc, "trap"); }
void ppc_mtfsf(u32 mask, u64 v) {
    u32 m = 0;
    for (int i = 0; i < 8; i++) if (mask & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
    cpu.fpscr = (cpu.fpscr & ~m) | ((u32)v & m);
}
void ppc_fp_record(void) { setcrf(1, cpu.fpscr >> 28); }
/* TODO(phase 7): Dolphin's table-based estimates; see docs/recomp_unknowns.md */
double ppc_fres(double b) { return 1.0 / b; }
double ppc_frsqrte(double b) { return 1.0 / sqrt(b); }
