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
    R[1] = 0x80462ac0u;
    R[2] = 0x804581a0u;
    R[13] = 0x804543c0u;
}

void recomp_bad(u32 pc, const char *why) { printf("recomp_bad %08lx: %s\n", (unsigned long)pc, why); }
void recomp_skipped(u32 addr) { (void)addr; }
u32 hw_read(u32 a, int size) { printf("hw_read %08lx/%d\n", (unsigned long)a, size); return 0; }
void hw_write(u32 a, u32 v, int size) { (void)v; printf("hw_write %08lx/%d\n", (unsigned long)a, size); }
u32 ppc_mfspr(int n) { return cpu.spr[n & 1023]; }
void ppc_mtspr(int n, u32 v) { cpu.spr[n & 1023] = v; }
u32 ppc_mftb(int tbr) { (void)tbr; return 0; }
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
