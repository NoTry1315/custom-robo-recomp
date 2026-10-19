# SDK function inventory (Phase 1)

Dolphin SDK 2003 (OS release build 0x2301) and supporting runtime functions found in Custom Robo `main.dol` (GXCE01, retail, no symbols).
Generated 2026-10-18 from the GXCE01 main.dol. Addresses are virtual (0x8000xxxx). Sizes are bytes to the next function start.

## Method

1. Function boundaries: bl-target set + prologue/post-blr scan over the text section (jump-table entries in data filtered out), plus a separate scan of the .init section. Leaf functions that nothing calls by `bl` and that sit mid-block may be merged with a neighbour (see Open questions).
2. Module extents: SDK libraries are linked as contiguous blocks. Anchors were the `<< Dolphin SDK - XX release build ... >>` version strings, `OSRegisterVersion` callers (one per library init, 13 of them), and hardware register windows (lis 0xCC00 + VI 0x2000, PI 0x3000, MI 0x4000, DSP/ARAM 0x5000, DI 0x6000, SI 0x6400, EXI 0x6800, AI 0x6C00, GX FIFO 0xCC008000).
3. Naming: (high) proven by a string, a hardware register/command constant, an SPR number, or a unique call-graph signature; (medium) call-graph position plus the known SDK file/function order; (low) guess from position/size only. Functions in a module range with no confident match are listed as `<module>_unk_<addr>`; they are not omitted.
4. GX uses the `__GXData` struct through r2 (`lwz rX,-0x6408(r2)`); function identity was recovered from the struct offset written (e.g. +0x1d0 cmode0, +0x1d8 zmode, +0x204 genMode, +0x130/+0x170 tevc/teva), XF/BP register ids and FIFO command bytes (0x08 CP, 0x10 XF, 0x61 BP).
5. Cross-check: the 13 `OSRegisterVersion` callers map 1:1 onto library init functions (OSInit, DVDInit, VIInit, AIInit, ARInit, ARQInit, AXInit, DSPInit, CARDInit, GXInit, EXIInit, SIInit, THPInit); externally called functions were checked against caller addresses in the game code (HSD wrappers call SDK functions in a recognisable init order, e.g. ARInit/ARQInit/OSGetSoundMode at 0x80099214-0x8009921c).

SDK build dates seen in the version strings: OS 2003-07-23, DVD 2003-09-16, VI/AI/AR/ARQ/DSP/CARD/EXI/SI 2003-04-17, GX 2003-07-23, AX 2003-07-29, THP 2002-08-27. PAD, MTX and DB carry no version string.

## Totals

| module | address range | identified / total | high | medium | low |
|---|---|---|---|---|---|
| OS | 0x80131c08-0x801394c4 | 184 / 184 | 124 | 44 | 16 |
| DVD | 0x8013a2c0-0x8013eb5c | 49 / 94 | 15 | 30 | 4 |
| PAD | 0x801409b0-0x80141fa4 | 6 / 14 | 3 | 3 | 0 |
| VI | 0x8013eb5c-0x801409b0 | 24 / 24 | 10 | 13 | 1 |
| GX | 0x801516c8-0x80159530 | 114 / 165 | 61 | 53 | 0 |
| AI | 0x80141fa4-0x8014288c | 11 / 19 | 3 | 8 | 0 |
| DSP/AR/ARQ/AX | 0x8014288c-0x8014af50 | 37 / 122 | 12 | 18 | 7 |
| CARD | 0x8014af50-0x801516c8 | 17 / 84 | 6 | 11 | 0 |
| EXI/SI | 0x80169484-0x8016d42c | 46 / 59 | 27 | 18 | 1 |
| MTX | 0x801394c4-0x8013a2c0 | 30 / 30 | 19 | 11 | 0 |
| THP | 0x801aeb40-0x801b65e0 | 3 / 23 | 1 | 0 | 2 |
| other | 0x80003100-0x80169484 | 38 / 321 | 16 | 18 | 4 |
| **SDK modules (excl. other)** | | **521 / 818** | | | |
| **overall (incl. other)** | | **559 / 1139** | | | |

"identified" means a name was assigned (any confidence); the remainder are `_unk_` rows in the module tables. Sub-module ranges: 
- base (PPCArch): 0x80131c08-0x80131d1c
- OS (incl. OSAlloc/OSFont/OSRtc/OSThread/OSTime + DB): 0x80131d1c-0x80139344
- init_cpp / exit: 0x80139344-0x801393d8
- DB: 0x801393d8-0x801394c4
- MTX (mtx/mtxvec/mtx44/vec): 0x801394c4-0x8013a2c0
- DVD (dvdlow/dvdfs/dvd/dvdqueue/fstload): 0x8013a2c0-0x8013eb5c
- VI: 0x8013eb5c-0x801409b0
- PAD (Padclamp/Pad): 0x801409b0-0x80141fa4
- AI: 0x80141fa4-0x8014288c
- AR: 0x8014288c-0x801443c4
- ARQ: 0x801443c4-0x80144760
- AX: 0x80144760-0x8014a42c
- DSP: 0x8014a42c-0x8014af50
- CARD: 0x8014af50-0x801516c8
- GX: 0x801516c8-0x80159530
- MetroTRK: 0x80159530-0x8015f6e8
- MWCC runtime: 0x8015f6e8-0x8015ff7c (phase 4: starts at the varargs helpers, not 0x8015f858; __va_arg is called from game code)
- MSL C library: 0x8015ff7c-0x80169484
- EXI: 0x80169484-0x8016beac
- SI: 0x8016beac-0x8016d42c
- THP (movie decode): 0x801aeb40-0x801b65e0
- .init (crt0, memcpy/memset): 0x80003100-0x800055e0

## OS (incl. PPCArch base, OSAlloc, OSFont, OSRtc, OSThread, OSTime) and DB

184 identified of 184 functions (high 124, medium 44, low 16).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x80131c08 | 0x8 | PPCMfmsr | high | mfmsr;blr in PPCArch block |
| 0x80131c10 | 0x8 | PPCMtmsr | high | mtmsr |
| 0x80131c18 | 0x8 | PPCMfhid0 | high | mfspr HID0(1008) |
| 0x80131c20 | 0x8 | PPCMthid0 | high | mtspr HID0 |
| 0x80131c28 | 0x8 | PPCMfl2cr | high | mfspr L2CR(1017) |
| 0x80131c30 | 0x8 | PPCMtl2cr | high | mtspr L2CR |
| 0x80131c38 | 0x8 | PPCMtdec | high | mtspr DEC(22) |
| 0x80131c40 | 0x8 | PPCSync | high | sync;blr |
| 0x80131c48 | 0x14 | PPCHalt | high | sync;nop;li r3,0;loop |
| 0x80131c5c | 0x8 | PPCMtmmcr0 | high | mtspr MMCR0(952) |
| 0x80131c64 | 0x8 | PPCMtmmcr1 | high | mtspr MMCR1(956) |
| 0x80131c6c | 0x8 | PPCMtpmc1 | high | mtspr PMC1(953) |
| 0x80131c74 | 0x8 | PPCMtpmc2 | high | mtspr PMC2(954) |
| 0x80131c7c | 0x8 | PPCMtpmc3 | high | mtspr PMC3(957) |
| 0x80131c84 | 0x8 | PPCMtpmc4 | high | mtspr PMC4(958) |
| 0x80131c8c | 0x20 | PPCMffpscr | high | mffs |
| 0x80131cac | 0x28 | PPCMtfpscr | high | mtfsf |
| 0x80131cd4 | 0x8 | PPCMfhid2 | high | mfspr HID2(920) |
| 0x80131cdc | 0x8 | PPCMthid2 | high | mtspr HID2 |
| 0x80131ce4 | 0x8 | PPCMtwpar | high | mtspr WPAR(921) |
| 0x80131cec | 0x28 | PPCDisableSpeculation | high | HID0 /= 0x200 via Mfhid0/Mthid0 |
| 0x80131d14 | 0x8 | PPCSetFpNonIEEEMode | high | mtfsb1 29;blr |
| 0x80131d1c | 0x128 | __OSFPRInit | high | HID2 PSE, loads FPRs/paired singles with zero |
| 0x80131e44 | 0x28 | OSGetConsoleType | high | reads BootInfo console type (r13), default 0x10000002 |
| 0x80131e6c | 0x128 | ClearArena | high | checks __OSGetResetCode, memset(ArenaLo..ArenaHi) |
| 0x80131f94 | 0x3c | InquiryCallback | high | writes DVD device code to 0x800030e6 |
| 0x80131fd0 | 0x3d8 | OSInit | high | xrefs 'Dolphin SDK - OS' version string + inits all OS subsystems (38 callees) |
| 0x801323a8 | 0x280 | OSExceptionInit | high | vector 0x80000060 / __OSEVStart copy, per-exception handler setup |
| 0x80132628 | 0x24 | __OSDBJUMPSTART | medium | asm stub: lr -> 0x40(0), mtmsr 0x30 (DBVECTOR copy source) |
| 0x8013264c | 0x4 | __OSDBJUMPEND | low | bla 0x60 stub |
| 0x80132650 | 0x1c | __OSSetExceptionHandler | high | writes OSExceptionTable[r13] (r3 index) |
| 0x8013266c | 0x14 | __OSGetExceptionHandler | high | reads OSExceptionTable |
| 0x80132680 | 0x58 | OSExceptionVector | high | asm: mtspr SPRG0, saves ctx via 0xC0(0), SRR0/SRR1 |
| 0x801326d8 | 0x10 | OSDefaultVector | medium | asm: mfmsr; ori 0x30; mtsrr1 tail of vector |
| 0x801326e8 | 0x30 | __OSEVSetNumber | medium | asm: exception number dispatch via 0x3000 table |
| 0x80132718 | 0x4 | OSExceptionVectorEnd | low | nop marker after vector |
| 0x8013271c | 0x58 | OSDefaultExceptionVector | medium | asm: save ctx+GQRs, DSISR/DAR, b __OSUnhandledException |
| 0x80132774 | 0x54 | __OSPSInit | high | HID2 /= 0xA0000000 (PSE/LSQE), zero GQR0-7; called by __init_hardware |
| 0x801327c8 | 0x14 | __OSGetDIConfig | high | reads DI config reg 0xCC006024 & 0xff |
| 0x801327dc | 0x2c | OSRegisterVersion | high | 13 callers, calls OSReport with the version string |
| 0x80132808 | 0x58 | OSInitAlarm | high | OSSetExceptionHandler(8,DecrementerHandler), OSRegisterResetFunction |
| 0x80132860 | 0x10 | OSCreateAlarm | high | zeroes alarm->handler/tag fields |
| 0x80132870 | 0x250 | InsertAlarm | medium | 64-bit time compare, sorted queue insert; called by OSSetAlarm |
| 0x80132ac0 | 0x68 | OSSetAlarm | high | OSDisableInterrupts, OSGetTime, InsertAlarm |
| 0x80132b28 | 0x11c | OSCancelAlarm | high | list removal + decrementer reprogram |
| 0x80132c44 | 0x230 | DecrementerExceptionCallback | medium | calls PPCMtdec, OSLoadContext, alarm handler dispatch |
| 0x80132e74 | 0x50 | DecrementerExceptionHandler | high | asm GQR save (913-919) then call callback |
| 0x80132ec4 | 0xa0 | OnReset | medium | walks AlarmQueue calling __DVDTestAlarm / OSCancelAlarm |
| 0x80132f64 | 0xac | DLInsert | medium | free-list insert + coalesce |
| 0x80133010 | 0xfc | OSAllocFromHeap | high | heap table r13-0x4ae0, 64B align, 3 ext callers |
| 0x8013310c | 0x7c | OSFreeToHeap | medium | DLInsert caller |
| 0x80133188 | 0x10 | OSSetCurrentHeap | high | swap r13 var, return old |
| 0x80133198 | 0x70 | OSInitAlloc | high | HeapArray/NumHeaps init loop |
| 0x80133208 | 0x6c | OSCreateHeap | medium | finds free heap slot |
| 0x80133274 | 0x14 | OSDestroyHeap | medium | sets size -1 |
| 0x80133288 | 0x360 | OSCheckHeap | high | string 'OSCheckHeap: Failed HeapArray' |
| 0x801335e8 | 0x8 | OSGetArenaHi | high | returns r13 arena hi |
| 0x801335f0 | 0x8 | OSGetArenaLo | high | returns r13 arena lo |
| 0x801335f8 | 0x8 | OSSetArenaHi | high | stores arena hi |
| 0x80133600 | 0x8 | OSSetArenaLo | high | stores arena lo |
| 0x80133608 | 0x2c | OSAllocFromArenaLo | high | bump arena lo, align |
| 0x80133634 | 0x1bc | __OSInitAudioSystem | high | DSP regs 0xCC005000 + DSP mem 0x81005000 copy |
| 0x801337f0 | 0xd8 | __OSStopAudioSystem | high | DSP regs 0xCC005000 |
| 0x801338c8 | 0x14 | DCEnable | high | HID0 /= 0x4000 |
| 0x801338dc | 0x2c | DCInvalidateRange | high | dcbi loop |
| 0x80133908 | 0x30 | DCFlushRange | high | dcbf loop + sync |
| 0x80133938 | 0x30 | DCStoreRange | high | dcbst loop + sync |
| 0x80133968 | 0x2c | DCFlushRangeNoSync | high | dcbf loop |
| 0x80133994 | 0x2c | DCZeroRange | high | dcbz loop |
| 0x801339c0 | 0x34 | ICInvalidateRange | high | icbi loop + sync;isync |
| 0x801339f4 | 0x10 | ICFlashInvalidate | high | HID0 /= 0x800 |
| 0x80133a04 | 0x14 | ICEnable | high | isync; HID0 /= 0x8000 |
| 0x80133a18 | 0xcc | __LCEnable | high | locked cache enable, DBAT3, HID2 |
| 0x80133ae4 | 0x38 | LCEnable | high | DisableInterrupts, __LCEnable |
| 0x80133b1c | 0x28 | LCDisable | high | dcbi 0xE0000000 range, HID2 clear LCE |
| 0x80133b44 | 0x24 | LCStoreBlocks | high | mtspr 922/923 (DMAU/DMAL) |
| 0x80133b68 | 0xac | LCStoreData | high | loops LCStoreBlocks 128 blocks at a time |
| 0x80133c14 | 0x14 | LCQueueWait | medium | spins on HID2 queue length |
| 0x80133c28 | 0x98 | L2GlobalInvalidate | high | string 'L2 INVALIDATE: SHOULD NEVER HAPPEN' + L2CR |
| 0x80133cc0 | 0x160 | DMAErrorHandler | medium | prints, OSDumpContext, PPCHalt |
| 0x80133e20 | 0xf4 | __OSCacheInit | high | ICE/DCE/L2 enable checks with DBPrintf strings |
| 0x80133f14 | 0x124 | __OSLoadFPUContext | high | HID2 PSE; lfd/psq_l from context |
| 0x80134038 | 0x128 | __OSSaveFPUContext | high | HID2 PSE; stfd/psq_st into context |
| 0x80134160 | 0x8 | OSSaveFPUContext | high | tail-jumps into __OSSaveFPUContext |
| 0x80134168 | 0x5c | OSSetCurrentContext | high | stores 0xD4 and 0xC0 in low mem, FP bit in MSR |
| 0x801341c4 | 0xc | OSGetCurrentContext | high | returns 0x800000D4 |
| 0x801341d0 | 0x80 | OSSaveContext | high | stmw r13; GQRs; returns 0 |
| 0x80134250 | 0xd8 | OSLoadContext | high | lmw; GQRs; rfi |
| 0x80134328 | 0x8 | OSGetStackPointer | high | mr r3,r1 |
| 0x80134330 | 0x24 | OSClearContext | high | clears state, 0xD8 fpu owner |
| 0x80134354 | 0x2a8 | OSDumpContext | high | string 'Context 0x%08x' |
| 0x801345fc | 0x84 | OSSwitchFPUContext | high | saves/loads FPU ctx, rfi |
| 0x80134680 | 0x174 | __OSContextInit | high | string 'FPU-unavailable handler installed' |
| 0x801347f4 | 0x80 | OSReport | high | 148 callers, varargs, calls vprintf |
| 0x80134874 | 0x12c | OSPanic | high | prints 'in "%s" on line %d' then halt |
| 0x801349a0 | 0x218 | OSSetErrorHandler | high | OSErrorTable[r13] store; FPE mask handling |
| 0x80134bb8 | 0x2e8 | __OSUnhandledException | high | prints exception report; calls OSDumpContext, PPCHalt |
| 0x80134ea0 | 0x19c | GetFontCode | low | OSFont.c: SJIS/ANSI code to glyph index |
| 0x8013503c | 0x174 | Decode | low | OSFont.c: compressed font bit unpack |
| 0x801351b0 | 0x58 | OSGetFontEncode | high | reads VI DTV status 0xCC00206E, r13 cache |
| 0x80135208 | 0x8c | ReadFont | low | OSFont.c: calls __OSReadROM wrapper |
| 0x80135294 | 0x334 | OSInitFont | medium | reads ROM font, builds header (0xCC00206E) |
| 0x801355c8 | 0x3b0 | ExpandFontSheet | low | OSFont.c: sheet expand using OSFreeToHeap region |
| 0x80135978 | 0xe0 | OSLoadFont | low | OSFont.c: uses OSGetFontEncode + OSInitFont |
| 0x80135a58 | 0x1a8 | OSGetFontData | low | OSFont.c: GetFontCode-based data lookup |
| 0x80135c00 | 0xc | OSDisableInterrupts | high | mfmsr;rlwinm;mtmsr 260 callers |
| 0x80135c0c | 0x8 | __OSInterruptNop | low | stub 8 bytes |
| 0x80135c14 | 0x14 | OSEnableInterrupts | high | mfmsr; ori MSR_EE |
| 0x80135c28 | 0x24 | OSRestoreInterrupts | high | 315 callers |
| 0x80135c4c | 0x1c | __OSSetInterruptHandler | high | indexes handler table r13-0x4ab8 |
| 0x80135c68 | 0x14 | __OSGetInterruptHandler | high | reads handler table |
| 0x80135c7c | 0x74 | __OSInterruptInit | high | PI 0xCC003000 mask init, memset table, sets ext int handler |
| 0x80135cf0 | 0x2d8 | SetInterruptMask | high | PI/MI/DSP/EXI/AI regs 0x3000/4000/5000/6800/6c00 |
| 0x80135fc8 | 0x88 | __OSMaskInterrupts | high | 19 callers; calls SetInterruptMask |
| 0x80136050 | 0x88 | __OSUnmaskInterrupts | high | 22 callers; calls SetInterruptMask |
| 0x801360d8 | 0x344 | __OSDispatchInterrupt | high | reads PI/MI/DSP/EXI/AI interrupt status |
| 0x8013641c | 0x50 | ExternalInterruptHandler | high | asm GQR save 913-919 then __OSDispatchInterrupt |
| 0x8013646c | 0x18 | OSMemory_unk_clear_30c8 | low | OSMemory.c area; zeroes 0x800030c8/cc/d0 |
| 0x80136484 | 0xc | OSGetPhysicalMemSize | high | returns 0x80000028 |
| 0x80136490 | 0x3c | OSProtectRange_part | low | writes MI 0xCC004010 and masks MI interrupts |
| 0x801364cc | 0x6c | MEMIntrruptHandler | high | reads MI 0xCC004024/1e/22, calls error handler or __OSUnhandledException(15) |
| 0x80136538 | 0x80 | Config24MB | high | DBAT/IBAT setup for 24MB (0x1ff) |
| 0x801365b8 | 0x80 | Config48MB | high | DBAT/IBAT setup for 48MB (0x3ff) |
| 0x80136638 | 0x18 | RealMode | high | clears MSR IR/DR via rfi |
| 0x80136650 | 0x118 | __OSInitMemoryProtection | medium | programs MI, installs MEMIntrruptHandler on irq 0..3 + MI mask |
| 0x80136768 | 0x70 | __OSUnlockAllMutex | medium | walks thread mutex queue (+0x2f4), OSWakeupThread |
| 0x801367d8 | 0x10 | OSReset_asm_jump | low | sync;isync;mtlr r3;blr |
| 0x801367e8 | 0xc | OSReset_unk_flag | low | sets r13 flag = 1 |
| 0x801367f4 | 0x340 | __OSShutdownDevices | medium | 23 callees: DVD/AI/PAD/EXI shutdown sequence |
| 0x80136b34 | 0x84 | OSRegisterResetFunction | high | appends to r13 reset function queue |
| 0x80136bb8 | 0x70 | Reset | high | PI 0xCC003000 reset write + HID0 |
| 0x80136c28 | 0x48 | __OSDoHotReset | medium | VI regs 0xCC002000, Reset() |
| 0x80136c70 | 0x2b8 | OSResetSystem | high | VI/PI regs, memset, shutdown, reset |
| 0x80136f28 | 0x30 | OSGetResetCode | high | reads PI 0xCC003024; called by ClearArena |
| 0x80136f58 | 0xf4 | __OSResetSWInterruptHandler | medium | PI reset button irq handler, OSGetTime |
| 0x8013704c | 0x298 | OSGetResetButtonState | high | PI 0xCC003000 reset switch debounce, uses OSGetTime and 64-bit divide |
| 0x801372e4 | 0x60 | WriteSramCallback | medium | OSRtc.c; calls WriteSram |
| 0x80137344 | 0x118 | WriteSram | medium | OSRtc.c; EXILock/Select/Imm/Deselect/Unlock |
| 0x8013745c | 0x13c | ReadSram | medium | OSRtc.c; DCInvalidateRange + EXI calls |
| 0x80137598 | 0x5c | __OSLockSram | medium | OSRtc.c; DisableInterrupts + lock flag |
| 0x801375f4 | 0x5c | __OSLockSramEx | medium | OSRtc.c; same with offset |
| 0x80137650 | 0x33c | UnlockSram | medium | OSRtc.c; EXI SRAM checksum + write |
| 0x8013798c | 0x24 | __OSUnlockSram | medium | calls UnlockSram |
| 0x801379b0 | 0x24 | __OSUnlockSramEx | medium | calls UnlockSram |
| 0x801379d4 | 0x10 | __OSSyncSram | medium | returns Scb.sync flag |
| 0x801379e4 | 0x124 | __OSReadROM | medium | DCInvalidateRange + EXI read of IPL ROM |
| 0x80137b08 | 0x80 | OSGetSoundMode | high | SRAM flags+0x13 bit2 |
| 0x80137b88 | 0xa4 | OSSetSoundMode | high | SRAM flags+0x13 bit2 write |
| 0x80137c2c | 0x70 | OSGetProgressiveMode | high | SRAM flags+0x13 bit7 |
| 0x80137c9c | 0xa4 | OSSetProgressiveMode | high | SRAM flags+0x13 bit7 write |
| 0x80137d40 | 0x84 | OSGetWirelessID | high | SRAM+0x2c u16[chan] |
| 0x80137dc4 | 0xac | OSSetWirelessID | high | SRAM+0x2c u16[chan] write |
| 0x80137e70 | 0x70 | OSGetGbsMode | high | SRAM+0x3c u16 |
| 0x80137ee0 | 0xb8 | OSSetGbsMode | high | SRAM+0x3c u16 write w/ validation |
| 0x80137f98 | 0x1c | Run | medium | OSReboot.c asm: HID0 flush + jump |
| 0x80137fb4 | 0x4 | OSReboot_unk_stub | low | 4-byte stub after Run |
| 0x80137fb8 | 0x64 | __OSReboot | medium | memset 0x80000c00, DCFlushRange/ICInvalidateRange |
| 0x8013801c | 0x4 | OSReboot_unk_stub2 | low | 4-byte stub |
| 0x80138020 | 0x158 | __OSThreadInit | high | sets up IdleThread/DefaultThread, current ctx 0xD8, r13 run queue |
| 0x80138178 | 0x10 | OSInitThreadQueue | high | zero head/tail |
| 0x80138188 | 0xc | OSGetCurrentThread | high | reads 0x800000E4 |
| 0x80138194 | 0x40 | OSDisableScheduler | medium | r13 reschedule counter ++ |
| 0x801381d4 | 0x40 | OSEnableScheduler | medium | r13 reschedule counter -- |
| 0x80138214 | 0x68 | UnsetRun | medium | OSThread.c run queue removal |
| 0x8013827c | 0x3c | __OSGetEffectivePriority | medium | OSThread.c mutex queue scan |
| 0x801382b8 | 0x1c0 | SetEffectivePriority | medium | OSThread.c; calls UnsetRun |
| 0x80138478 | 0x228 | SelectThread | high | OSSetCurrentContext/OSSaveContext/OSLoadContext/OSClearContext |
| 0x801386a0 | 0x30 | __OSReschedule | medium | calls SelectThread |
| 0x801386d0 | 0x1bc | OSExitThread | medium | OSClearContext, __OSUnlockAllMutex, wake joiners |
| 0x8013888c | 0x288 | OSResumeThread | medium | OSThread.c; calls Get/SetEffectivePriority, SelectThread |
| 0x80138b14 | 0x170 | OSSuspendThread | medium | OSThread.c; calls UnsetRun etc |
| 0x80138c84 | 0xec | OSSleepThread | medium | OSThread.c; Disable/Restore + SelectThread |
| 0x80138d70 | 0x104 | OSWakeupThread | medium | 8 callers incl DVD; SelectThread |
| 0x80138e74 | 0xac | OSClearStack | low | calls OSGetStackPointer |
| 0x80138f20 | 0x18 | OSGetTime | high | mftbu/mftb loop, 36 callers |
| 0x80138f38 | 0x8 | OSGetTick | high | mftb;blr |
| 0x80138f40 | 0x64 | __OSGetSystemTime | high | OSGetTime + system base 0x800030d8..dc, int disabled |
| 0x80138fa4 | 0x19c | GetDates | medium | OSTime.c; consts 0x92492493 / 0xb38cf9b1 |
| 0x80139140 | 0x204 | OSTicksToCalendarTime | medium | OSTime.c; calls GetDates and 64-bit div |
| 0x801393d8 | 0x28 | DBInit | high | stores DBInterface 0x80000040 in r13, exception dest 0x48(0x80000000) |
| 0x80139400 | 0x48 | __DBExceptionDestinationAux | high | prints, OSDumpContext, PPCHalt |
| 0x80139448 | 0x10 | __DBExceptionDestination | high | mfmsr ori 0x30 mtmsr then Aux |
| 0x80139458 | 0x1c | __DBIsExceptionMarked | high | DBInterface->exceptionMask & (1<<n) |
| 0x80139474 | 0x50 | DBPrintf | high | empty varargs stub (release build) |

## DVD (dvdlow, dvdfs, dvd, dvdqueue, fstload)

49 identified of 94 functions (high 15, medium 30, low 4).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x8013a2c0 | 0x40 | __DVDInitWA | medium | dvdlow.c first fn, calls OSInitAlarm and __DVDLowSetWAType |
| 0x8013a300 | 0x2e0 | __DVDInterruptHandler | high | DI regs 0xCC006000/04, OSSetCurrentContext/OSClearContext |
| 0x8013a5e0 | 0x84 | AlarmHandler | medium | dvdlow.c; DVDLowRead/Seek caller |
| 0x8013a664 | 0x70 | DVDLow_unk_8013a664 | low | dvdlow.c, ctx set/clear + mask |
| 0x8013a6d4 | 0x110 | DoJustRead | medium | dvdlow.c; writes DI regs, creates alarm |
| 0x8013a7e4 | 0x80 | SeekTwiceBeforeRead | medium | dvdlow.c; calls DVDLowSeek |
| 0x8013a864 | 0x298 | DVDLowRead | high | DI regs, 4 callers, calls DoJustRead/SeekTwice |
| 0x8013aafc | 0x94 | DVDLowSeek | high | DI cmd 0xAB000001 |
| 0x8013ab90 | 0x2c | DVDLowWaitCoverClose | medium | dvdlow.c order, DI status |
| 0x8013abbc | 0xa4 | DVDLowReadDiskID | high | DI cmd 0xA8000040 |
| 0x8013ac60 | 0x8c | DVDLowStopMotor | high | DI cmd 0xE3000001 |
| 0x8013acec | 0x8c | DVDLowRequestError | high | DI cmd 0xE0000001 |
| 0x8013ad78 | 0x9c | DVDLowInquiry | medium | dvdlow.c order, DI cmd |
| 0x8013ae14 | 0x98 | DVDLowAudioStream | medium | dvdlow.c order |
| 0x8013aeac | 0x8c | DVDLowRequestAudioStatus | medium | dvdlow.c order |
| 0x8013af38 | 0x9c | DVDLowAudioBufferConfig | medium | dvdlow.c order |
| 0x8013afd4 | 0xbc | DVDLowReset | high | PI 0xCC003000 + OSGetTime delay |
| 0x8013b090 | 0x14 | DVDLowBreak | medium | dvdlow.c order |
| 0x8013b0a4 | 0x1c | DVDLowClearCallback | medium | DI 0xCC006004 write, r13 callback |
| 0x8013b0c0 | 0x44 | __DVDLowSetWAType | medium | Disable/Restore, stores WA type in r13 |
| 0x8013b104 | 0x38 | __DVDLowTestAlarm | low | dvdlow.c last fn |
| 0x8013b13c | 0x38 | __DVDFSInit | high | loads 0x80000038 FST pointers into r13 (dvdfs.c) |
| 0x8013b174 | 0x2f4 | DVDConvertPathToEntrynum | high | string 'DVDConvertEntrynumToPath(possibly DVDOpen or DVDChangeDir...)' |
| 0x8013b468 | 0x74 | DVDFastOpen | medium | dvdfs.c order |
| 0x8013b4dc | 0xc8 | DVDOpen | high | string 'Warning: DVDOpen(): file ... not found' |
| 0x8013b5a4 | 0x24 | DVDClose | medium | 12 callers; calls DVDCancel |
| 0x8013b5c8 | 0x160 | entryToPath | medium | recursive path builder |
| 0x8013b728 | 0x154 | DVDConvertEntrynumToPath | medium | calls entryToPath |
| 0x8013b87c | 0xc4 | DVDGetCurrentDir | medium | calls entryToPath |
| 0x8013b940 | 0x60 | DVDChangeDir | medium | calls DVDConvertPathToEntrynum |
| 0x8013b9a0 | 0xc0 | DVDReadAsyncPrio | high | string 'DVDReadAsync(): specified area is out of the file' |
| 0x8013ba60 | 0x30 | cbForReadAsync | medium | dvdfs.c |
| 0x8013ba90 | 0x118 | DVDReadPrio | high | string 'DVDRead(): specified area is out of the file' |
| 0x8013bba8 | 0x24 | cbForReadSync | medium | dvdfs.c |
| 0x8013bbcc | 0xc0 | DVDOpenDir | high | string 'Warning: DVDOpenDir(): file ... not found' |
| 0x8013bc8c | 0xa4 | DVDReadDir | medium | dvdfs.c order |
| 0x8013bd30 | 0x4 | DVDCloseDir | low | 4-byte stub |
| 0x8013bd34 | 0xcc | DVDInit | high | OSRegisterVersion(DVD ver), __DVDInitWA, __DVDFSInit, string 'load fst' |
| 0x8013be00 | 0x94 | DVD_unk_8013be00 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8013be94 | 0x8c | DVD_unk_8013be94 | unk | in module range, no name match; callers=0 callees=4 |
| 0x8013bf20 | 0xac | DVD_unk_8013bf20 | unk | in module range, no name match; callers=11 callees=3 |
| 0x8013bfcc | 0x34 | DVD_unk_8013bfcc | unk | in module range, no name match; callers=2 callees=3 |
| 0x8013c000 | 0x28 | DVD_unk_8013c000 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013c028 | 0xb4 | DVD_unk_8013c028 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8013c0dc | 0x294 | DVD_unk_8013c0dc | unk | in module range, no name match; callers=0 hw=6020 callees=10 |
| 0x8013c370 | 0x68 | DVD_unk_8013c370 | unk | in module range, no name match; callers=0 callees=5 |
| 0x8013c3d8 | 0x98 | DVD_unk_8013c3d8 | unk | in module range, no name match; callers=0 hw=6000 callees=4 |
| 0x8013c470 | 0x28 | DVD_unk_8013c470 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8013c498 | 0x158 | DVD_unk_8013c498 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8013c5f0 | 0xe0 | DVD_unk_8013c5f0 | unk | in module range, no name match; callers=1 callees=7 |
| 0x8013c6d0 | 0x34 | DVD_unk_8013c6d0 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013c704 | 0x34 | DVD_unk_8013c704 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013c738 | 0x74 | DVD_unk_8013c738 | unk | in module range, no name match; callers=0 callees=5 |
| 0x8013c7ac | 0x38 | DVD_unk_8013c7ac | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013c7e4 | 0x114 | DVD_unk_8013c7e4 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8013c8f8 | 0xe4 | DVD_unk_8013c8f8 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8013c9dc | 0xfc | DVD_unk_8013c9dc | unk | in module range, no name match; callers=0 callees=6 |
| 0x8013cad8 | 0x44 | DVD_unk_8013cad8 | unk | in module range, no name match; callers=0 callees=3 |
| 0x8013cb1c | 0xcc | DVD_unk_8013cb1c | unk | in module range, no name match; callers=1 callees=5 |
| 0x8013cbe8 | 0x30 | DVD_unk_8013cbe8 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013cc18 | 0x70 | DVD_unk_8013cc18 | unk | in module range, no name match; callers=0 callees=5 |
| 0x8013cc88 | 0x28 | DVD_unk_8013cc88 | unk | in module range, no name match; callers=4 callees=1 |
| 0x8013ccb0 | 0xe4 | DVD_unk_8013ccb0 | unk | in module range, no name match; callers=0 hw=6004 callees=5 |
| 0x8013cd94 | 0x230 | DVD_unk_8013cd94 | unk | in module range, no name match; callers=29 callees=11 |
| 0x8013cfc4 | 0x320 | DVD_unk_8013cfc4 | unk | in module range, no name match; callers=3 hw=6000/6004 callees=9 |
| 0x8013d2e4 | 0x638 | DVD_unk_8013d2e4 | unk | in module range, no name match; callers=0 hw=6000/6020 callees=9 |
| 0x8013d91c | 0xdc | DVD_unk_8013d91c | unk | in module range, no name match; callers=4 callees=5 |
| 0x8013d9f8 | 0xd0 | DVD_unk_8013d9f8 | unk | in module range, no name match; callers=2 callees=5 |
| 0x8013dac8 | 0xd4 | DVD_unk_8013dac8 | unk | in module range, no name match; callers=2 callees=5 |
| 0x8013db9c | 0xbc | DVD_unk_8013db9c | unk | in module range, no name match; callers=1 callees=5 |
| 0x8013dc58 | 0xd0 | DVD_unk_8013dc58 | unk | in module range, no name match; callers=1 callees=5 |
| 0x8013dd28 | 0x44 | DVDReset | medium | calls DVDLowReset, 17 callers within dvd.c |
| 0x8013dd6c | 0x4c | DVDGetCommandBlockStatus | medium | Disable/Restore, no globals |
| 0x8013ddb8 | 0xac | DVDGetDriveStatus | medium | Disable/Restore, drive state map |
| 0x8013de64 | 0x10 | DVDSetAutoInvalidation | medium | swaps r13 flag, used by read fns |
| 0x8013de74 | 0x50 | DVDResume | medium | Disable/Restore, calls stateReady |
| 0x8013dec4 | 0x27c | DVDPause | low | Disable only |
| 0x8013e140 | 0xac | DVDCancel | medium | called by DVDClose; Disable/Restore, wakeup |
| 0x8013e1ec | 0x24 | DVD_unk_8013e1ec | unk | in module range, no name match; callers=0 callees=1 |
| 0x8013e210 | 0x8 | DVD_unk_8013e210 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8013e218 | 0xf8 | DVDCheckDisk | medium | Disable/Restore, drive state check |
| 0x8013e310 | 0x11c | DVD_unk_8013e310 | unk | in module range, no name match; callers=1 callees=6 |
| 0x8013e42c | 0x38 | __DVDTestAlarm | medium | called by OnReset, calls __DVDLowTestAlarm |
| 0x8013e464 | 0x38 | __DVDStoreErrorCode | medium | dvderror.c; 5 callers inside dvd.c |
| 0x8013e49c | 0x68 | DVD_unk_8013e49c | unk | in module range, no name match; callers=5 callees=2 |
| 0x8013e504 | 0xa0 | DVD_unk_8013e504 | unk | in module range, no name match; callers=2 callees=2 |
| 0x8013e5a4 | 0x58 | DVD_unk_8013e5a4 | unk | in module range, no name match; callers=1 callees=2 |
| 0x8013e5fc | 0x60 | DVD_unk_8013e5fc | unk | in module range, no name match; callers=1 callees=2 |
| 0x8013e65c | 0x11c | DVD_unk_8013e65c | unk | in module range, no name match; callers=1 callees=0 |
| 0x8013e778 | 0x7c | DVD_unk_8013e778 | unk | in module range, no name match; callers=22 callees=3 |
| 0x8013e7f4 | 0xf8 | DVD_unk_8013e7f4 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8013e8ec | 0x30 | DVD_unk_8013e8ec | unk | in module range, no name match; callers=1 callees=0 |
| 0x8013e91c | 0xd8 | DVD_unk_8013e91c | unk | in module range, no name match; callers=0 callees=3 |
| 0x8013e9f4 | 0x168 | __fstLoad | high | string 'Game Name ... %c%c%c%c', reads FST to arena hi |

## PAD

6 identified of 14 functions (high 3, medium 3, low 0).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x801409b0 | 0x1a8 | ClampStick | medium | Padclamp.c; stick dead-zone clamp, called twice by PADClamp |
| 0x80140b58 | 0x10c | PADClamp | high | Padclamp.c; caller adjacent to PADRead in game input code |
| 0x80140c64 | 0x1a4 | DoReset | medium | Pad.c; loops channels SIGetTypeAsync(chan) |
| 0x80140e08 | 0xc4 | PAD_unk_80140e08 | unk | in module range, no name match; callers=0 callees=6 |
| 0x80140ecc | 0xcc | PAD_unk_80140ecc | unk | in module range, no name match; callers=0 callees=5 |
| 0x80140f98 | 0xd8 | PAD_unk_80140f98 | unk | in module range, no name match; callers=0 callees=5 |
| 0x80141070 | 0x32c | PAD_unk_80141070 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8014139c | 0x140 | PAD_unk_8014139c | unk | in module range, no name match; callers=0 callees=5 |
| 0x801414dc | 0x110 | PADReset | medium | Pad.c order before PADRecalibrate; mask of channels, r13 reset bits; game calls it for empty ports |
| 0x801415ec | 0x114 | PADRecalibrate | medium | same shape as PADReset plus recalibrate bit; next in Pad.c |
| 0x80141700 | 0x300 | PADRead | high | Pad.c; copies SI response into PADStatus[4]; ext caller next to PADClamp |
| 0x80141a00 | 0xb8 | PADControlMotor | medium | SISetCommand + SITransferCommands with motor command, after PADRead |
| 0x80141ab8 | 0x470 | SPEC2_MakeStatus | medium | Pad.c; no callees, reads status 8 bytes (r13 spec fn table) |
| 0x80141f28 | 0x7c | __PADDisableRecalibration | high | toggles bit 0x40 of 0x800030E3; called by OSResetSystem |

## VI

24 identified of 24 functions (high 10, medium 13, low 1).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x8013eb5c | 0x274 | VIRetraceHandler | high | VI regs 0xCC002000/2034..203c, installed as interrupt, calls pre/post callbacks |
| 0x8013edd0 | 0x44 | VISetPreRetraceCallback | high | swaps r13 callback, Disable/Restore |
| 0x8013ee14 | 0x44 | VISetPostRetraceCallback | high | swaps r13 callback, Disable/Restore |
| 0x8013ee58 | 0xa0 | getTiming | medium | VI.c; returns timing table entry by mode |
| 0x8013eef8 | 0x200 | __VIInit | medium | VI regs 0xCC002000/2006/206c, clears registers per TV format |
| 0x8013f0f8 | 0x4b0 | VIInit | high | OSRegisterVersion + VI version string, programs VI, 0x4b0 bytes |
| 0x8013f5a8 | 0x54 | VIWaitForRetrace | high | 16 callers, OSSleepThread on retraceQueue |
| 0x8013f5fc | 0x164 | setFbbRegs | medium | VI.c; writes VI fb address regs |
| 0x8013f760 | 0x170 | AdjustPosition | low | VI.c; computes display position adjust |
| 0x8013f8d0 | 0x1a0 | setVerticalRegs | medium | VI.c; writes vertical timing regs |
| 0x8013fa70 | 0x590 | VIConfigure | high | string 'VIConfigure(): Tried to change mode from (%d) to (%d), which is forbidden' |
| 0x80140000 | 0x298 | VIConfigurePan | medium | VI.c order after VIConfigure; calls setFbbRegs/setVerticalRegs |
| 0x80140298 | 0x130 | VIFlush | medium | VI.c; commits shadow regs, 3 ext callers in HSD VI init |
| 0x801403c8 | 0x6c | VISetNextFrameBuffer | medium | VI.c; calls setFbbRegs |
| 0x80140434 | 0x7c | VISetBlack | medium | VI.c; calls setVerticalRegs |
| 0x801404b0 | 0x8 | VIGetRetraceCount | high | returns r13 retrace counter |
| 0x801404b8 | 0x3c | getCurrentHalfLine | medium | VI 0xCC002000 + 0x2c/0x2e read |
| 0x801404f4 | 0x68 | getCurrentFieldEvenOdd | medium | VI.c; field parity from half line |
| 0x8014055c | 0x9c | VIGetNextField | medium | VI.c; calls getCurrentHalfLine |
| 0x801405f8 | 0x98 | VIGetCurrentLine | high | called by SIInterruptHandler; VI half-line read |
| 0x80140690 | 0x68 | VIGetTvFormat | high | reads r13 tv format; called by SISetSamplingRate |
| 0x801406f8 | 0x3c | VIGetDTVStatus | high | VI 0xCC00206E & 1; ext callers in video init |
| 0x80140734 | 0x21c | __VIDisplayPositionToXY | medium | divides half-line pos by HorVer fields |
| 0x80140950 | 0x60 | __VIGetCurrentPosition | medium | reads VI 0xCC00202c/2e, calls DisplayPositionToXY |

## GX

114 identified of 165 functions (high 61, medium 53, low 0; includes the three TPL helpers at the end of the range).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x801516c8 | 0xfc | __GXDefaultTexRegionCallback | medium | GXInit.c order, first GX fn |
| 0x801517c4 | 0x24 | __GXDefaultTlutRegionCallback | medium | GXInit.c order |
| 0x801517e8 | 0x190 | __GXShutdown | medium | PPCSync + OSGetTime, FIFO write 0xCC008000 |
| 0x80151978 | 0x798 | GXInit | high | PPCMthid2/WPAR, OSRegisterResetFunction, ext caller in HSD GX init |
| 0x80152110 | 0x938 | __GXInitGX | high | GX version string, 20+ GXSet* init calls |
| 0x80152a48 | 0x134 | GXCPInterruptHandler | medium | ctx set/clear + OSResumeThread |
| 0x80152b7c | 0x6c | GX_unk_80152b7c | unk | in module range, no name match; callers=1 callees=2 |
| 0x80152be8 | 0x70 | GX_unk_80152be8 | unk | in module range, no name match; callers=1 callees=2 |
| 0x80152c58 | 0xc | GX_unk_80152c58 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80152c64 | 0x128 | GX_unk_80152c64 | unk | in module range, no name match; callers=1 callees=6 |
| 0x80152d8c | 0x178 | GX_unk_80152d8c | unk | in module range, no name match; callers=1 callees=8 |
| 0x80152f04 | 0x44 | GX_unk_80152f04 | unk | in module range, no name match; callers=1 callees=2 |
| 0x80152f48 | 0x4c | __GXFifoInit | medium | __OSSetInterruptHandler + __OSUnmaskInterrupts + OSGetCurrentThread |
| 0x80152f94 | 0x24 | GX_unk_80152f94 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80152fb8 | 0x24 | GX_unk_80152fb8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80152fdc | 0x34 | GX_unk_80152fdc | unk | in module range, no name match; callers=4 callees=0 |
| 0x80153010 | 0x30 | GX_unk_80153010 | unk | in module range, no name match; callers=7 callees=0 |
| 0x80153040 | 0x30 | GX_unk_80153040 | unk | in module range, no name match; callers=4 callees=0 |
| 0x80153070 | 0x8 | GX_unk_80153070 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80153078 | 0x26c | GXSetVtxDesc | high | vcdLo/vcdHi (gx+0x14/0x18), 73 ext callers |
| 0x801532e4 | 0x288 | GX_unk_801532e4 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015356c | 0xbc | __GXSetVCD | high | writes CP reg 0x50/0x60 via FIFO from vcd |
| 0x80153628 | 0x124 | __GXCalculateVLim | high | writes gx+6 vLim |
| 0x8015374c | 0x1b4 | GXGetVtxDesc | medium | reads vcdLo/vcdHi |
| 0x80153900 | 0x90 | GXGetVtxDescv | medium | calls GXGetVtxDesc |
| 0x80153990 | 0x38 | GXClearVtxDesc | high | zero vcd, 36 callers |
| 0x801539c8 | 0x25c | GXSetVtxAttrFmt | high | vat dirty bits 5ab, 54 ext callers |
| 0x80153c24 | 0x280 | GX_unk_80153c24 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80153ea4 | 0x9c | __GXSetVAT | high | CP regs 0x70-0x90 via FIFO |
| 0x80153f40 | 0x280 | GXGetVtxAttrFmt | medium | GXAttr.c order |
| 0x801541c0 | 0x74 | GXGetVtxAttrFmtv | medium | calls GXGetVtxAttrFmt |
| 0x80154234 | 0x8c | GXSetArray | high | gx+0x88 base/0x98 stride + CP array regs |
| 0x801542c0 | 0x10 | GXInvalidateVtxCache | high | FIFO cmd 0x48 |
| 0x801542d0 | 0x280 | GXSetTexCoordGen2 | high | 18 ext callers, writes XF tex gen, gx matIdx 0x80/0x84 |
| 0x80154550 | 0x3c | GXSetNumTexGens | high | XF reg 0x103f |
| 0x8015458c | 0x94 | GX_unk_8015458c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80154620 | 0x5c | GXFlush | medium | calls PPCSync after FIFO flush words |
| 0x8015467c | 0x16c | GX_unk_8015467c | unk | in module range, no name match; callers=1 hw=3000 callees=2 |
| 0x801547e8 | 0x4c | GXWaitDrawDone | high | OSDisableInterrupts, OSSleepThread loop on the draw-done flag r13-0x4720 (phase 3) |
| 0x80154834 | 0x80 | GXDrawDone | high | writes BP 0x45000002 (PE draw-done token), flushes, clears and waits on the draw-done flag (phase 3) |
| 0x801548b4 | 0x24 | GXPixModeSync | high | writes gx peCtrl (0x1dc) as BP, no args |
| 0x801548d8 | 0x14 | GX_unk_801548d8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801548ec | 0x20 | GX_unk_801548ec | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015490c | 0x18 | GX_unk_8015490c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80154924 | 0x64 | GX_unk_80154924 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80154988 | 0x18 | GX_unk_80154988 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801549a0 | 0x24 | GX_unk_801549a0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801549c4 | 0x18 | GX_unk_801549c4 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801549dc | 0x20 | GX_unk_801549dc | unk | in module range, no name match; callers=1 callees=0 |
| 0x801549fc | 0x44 | GX_unk_801549fc | unk | in module range, no name match; callers=1 callees=2 |
| 0x80154a40 | 0x88 | GX_unk_80154a40 | unk | in module range, no name match; callers=0 callees=2 |
| 0x80154ac8 | 0x44 | GXSetDrawDoneCallback | high | swaps callback r13-0x4724 under OSDisableInterrupts, returns old (phase 3) |
| 0x80154b0c | 0x80 | GX_unk_80154b0c | unk | in module range, no name match; callers=0 callees=3 |
| 0x80154b8c | 0x74 | GX_unk_80154b8c | unk | in module range, no name match; callers=1 callees=3 |
| 0x80154c00 | 0x80 | __GXSetDirtyState | high | calls __GXSetVCD/__GXCalculateVLim/__GXSetVAT |
| 0x80154c80 | 0xd0 | GXBegin | high | 41 ext callers, FIFO prim header, calls __GXSetDirtyState |
| 0x80154d50 | 0x88 | __GXSendFlushPrim | medium | FIFO cmd 0x98 + zero words |
| 0x80154dd8 | 0x40 | GXSetLineWidth | high | gx+0x7c lpSize |
| 0x80154e18 | 0x40 | GXSetPointSize | high | gx+0x7c lpSize |
| 0x80154e58 | 0x48 | GXSetTexCoordBias | medium | gx suTs0[coord] (0xb8+4*i) bits 18/19, BP write |
| 0x80154ea0 | 0x44 | GXSetCullMode | high | gx+0x204 genMode, 6 ext callers |
| 0x80154ee4 | 0x34 | GXSetCoPlanar | medium | BP 0xFE mask + genMode |
| 0x80154f18 | 0x24 | __GXSetGenMode | medium | writes gx+0x204 as BP |
| 0x80154f3c | 0x7c | GXSetDispCopySrc | high | gx+0x1e0/0x1e4 |
| 0x80154fb8 | 0x7c | GXSetTexCopySrc | high | gx+0x1f0/0x1f4 |
| 0x80155034 | 0x34 | GXSetDispCopyDst | high | gx+0x1e8 |
| 0x80155068 | 0x130 | GXSetTexCopyDst | high | gx+0x1f8/0x1fc/0x200 |
| 0x80155198 | 0x24 | GX_unk_80155198 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801551bc | 0x58 | GXSetCopyClamp | high | bits 0/1 of gx 0x1ec and 0x1fc from clamp&1, clamp&2; no FIFO |
| 0x80155214 | 0xcc | GXSetDispCopyYScale | medium | gx+0x1e4/0x1ec, calls __cvt_fp2unsigned |
| 0x801552e0 | 0x78 | GXSetCopyClear | medium | 2 BP writes |
| 0x80155358 | 0x208 | GXSetCopyFilter | high | BP 0x01/0x02 coef words (0x166/0x266 constants) |
| 0x80155560 | 0x14 | GXSetDispCopyGamma | medium | gx+0x1ec |
| 0x80155574 | 0x168 | GXCopyDisp | high | reads cmode0/zmode/peCtrl, 4 callers |
| 0x801556dc | 0x18c | GXCopyTex | medium | same save/restore pattern |
| 0x80155868 | 0x38 | GXClearBoundingBox | high | BP 0x56 write constant |
| 0x801558a0 | 0x1c | GXInitLightAttn | high | stores 6 floats a0-a2,k0-k2 at lightobj+0x10..0x24 |
| 0x801558bc | 0x190 | GXInitLightSpot | medium | calls cos (80169400), float math |
| 0x80155a4c | 0xd0 | GXInitLightDistAttn | medium | GXLight.c order |
| 0x80155b1c | 0x10 | GXInitLightPos | medium | GXLight.c order |
| 0x80155b2c | 0x1c | GXInitLightDir | medium | GXLight.c order |
| 0x80155b48 | 0xc | GXInitLightColor | medium | GXLight.c order |
| 0x80155b54 | 0x7c | GXLoadLightObjImm | medium | XF write |
| 0x80155bd0 | 0xf0 | GXSetChanAmbColor | high | gx+0xa8/0xac ambient |
| 0x80155cc0 | 0xf0 | GXSetChanMatColor | high | gx+0xb0/0xb4 material |
| 0x80155db0 | 0x3c | GXSetNumChans | high | XF 0x1009, gx genMode |
| 0x80155dec | 0xb8 | GXSetChanCtrl | high | XF 0x100e.., 25 ext callers |
| 0x80155ea4 | 0x15c | GX_unk_80155ea4 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x80156000 | 0xc8 | GX_unk_80156000 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801560c8 | 0x24c | GXInitTexObj | high | memset + 8 ext callers |
| 0x80156314 | 0x48 | GXInitTexObjCI | medium | calls GXInitTexObj |
| 0x8015635c | 0x164 | GXInitTexObjLOD | medium | GXTexture.c order |
| 0x801564c0 | 0x8 | GX_unk_801564c0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801564c8 | 0x18 | GX_unk_801564c8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801564e0 | 0x17c | GXLoadTexObjPreLoaded | medium | BP tex regs |
| 0x8015665c | 0x54 | GXLoadTexObj | high | calls GXLoadTexObjPreLoaded |
| 0x801566b0 | 0x38 | GXInitTlutObj | high | fills GXTlutObj: fmt<<10, (lut>>5)|0x64<<24 (BP 0x64 loadtlut0), n_entries; no FIFO |
| 0x801566e8 | 0x98 | GXLoadTlut | high | calls tlut region cb gx+0x4cc, BP loadtlut0/1 from region, updates tlut obj |
| 0x80156780 | 0xf4 | GX_unk_80156780 | unk | in module range, no name match; callers=3 callees=0 |
| 0x80156874 | 0x38 | GX_unk_80156874 | unk | in module range, no name match; callers=2 callees=0 |
| 0x801568ac | 0x48 | GXInvalidateTexAll | high | BP 0x66 (TX_INVALIDATE) 0x66001000 and 0x66001100 between two __GXFlushTextureState calls (was mislabelled GXLoadTlut) |
| 0x801568f4 | 0x14 | GXSetTexRegionCallback | high | swaps gx+0x4c8, returns old |
| 0x80156908 | 0x14 | GXSetTlutRegionCallback | high | swaps gx+0x4cc, returns old |
| 0x8015691c | 0xa0 | GX_unk_8015691c | unk | in module range, no name match; callers=2 hw=8000 callees=0 |
| 0x801569bc | 0x17c | GX_unk_801569bc | unk | in module range, no name match; callers=2 callees=1 |
| 0x80156b38 | 0x354 | __GXSetTmemConfig | medium | switch on r3 (0..2) writing TMEM region BP regs 0x8c-0x94 (GXInit; was mislabelled GXSetTevOp) |
| 0x80156e8c | 0x6c | GX_unk_80156e8c | unk | in module range, no name match; callers=1 hw=8000 callees=0 |
| 0x80156ef8 | 0x144 | GX_unk_80156ef8 | unk | in module range, no name match; callers=4 hw=8000 callees=0 |
| 0x8015703c | 0x24 | GX_unk_8015703c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80157060 | 0x48 | GX_unk_80157060 | unk | in module range, no name match; callers=1 callees=1 |
| 0x801570a8 | 0x4 | GX_unk_801570a8 | unk | in module range, no name match; callers=2 callees=0 |
| 0x801570ac | 0x30 | GX_unk_801570ac | unk | in module range, no name match; callers=4 ext=3 hw=8000 callees=0 |
| 0x801570dc | 0x24 | GX_unk_801570dc | unk | in module range, no name match; callers=8 hw=8000 callees=0 |
| 0x80157100 | 0x8c | GXSetTevOp | medium | tev stage r3, mode r4 indexes color/alpha-in tables (stage0 vs later), rewrites gx tevc 0x130 / teva 0x170 + BP, 18 ext callers |
| 0x8015718c | 0x44 | GXSetTevColorIn | high | gx tevc[stage] (0x130 + 4*stage) |
| 0x801571d0 | 0x44 | GXSetTevAlphaIn | high | gx teva[stage] (0x170 + 4*stage) |
| 0x80157214 | 0x68 | GXSetTevColorOp | high | gx tevc op bits |
| 0x8015727c | 0x68 | GXSetTevAlphaOp | high | gx teva op bits |
| 0x801572e4 | 0x7c | GXSetTevColor | medium | TEV color reg BP writes |
| 0x80157360 | 0x7c | GXSetTevColorS10 | medium | TEV color reg S10 |
| 0x801573dc | 0x74 | GXSetTevKColor | medium | TEV konst color BP |
| 0x80157450 | 0x5c | GXSetTevKColorSel | medium | ksel |
| 0x801574ac | 0x5c | GXSetTevKAlphaSel | medium | ksel |
| 0x80157508 | 0x48 | GXSetTevSwapMode | medium | BP tev ksel swap bits |
| 0x80157550 | 0x80 | GXSetTevSwapModeTable | medium | BP ksel |
| 0x801575d0 | 0x44 | GXSetAlphaCompare | medium | BP 0xF3 alpha compare |
| 0x80157614 | 0x8c | GXSetZTexture | medium | BP 0xF4/F5 |
| 0x801576a0 | 0x19c | GXSetTevOrder | high | 42 ext callers, tref dirty 5ac |
| 0x8015783c | 0x28 | GXSetNumTevStages | high | gx genMode 0x204, 32 ext callers |
| 0x80157864 | 0x224 | GXSetFog | medium | BP 0xEE..0xF2, calls __cvt_fp2unsigned |
| 0x80157a88 | 0x1b0 | GXInitFogAdjTable | medium | float math + __cvt_fp2unsigned |
| 0x80157c38 | 0x124 | GXSetFogRangeAdj | medium | BP 0xE8..0xED |
| 0x80157d5c | 0x54 | GXSetBlendMode | high | gx cmode0 (0x1d0), 19 ext callers |
| 0x80157db0 | 0x2c | GXSetColorUpdate | high | gx cmode0 bit |
| 0x80157ddc | 0x2c | GXSetAlphaUpdate | high | gx cmode0 bit |
| 0x80157e08 | 0x34 | GXSetZMode | high | gx zmode (0x1d8), 15 ext callers |
| 0x80157e3c | 0x34 | GXSetZCompLoc | high | gx peCtrl (0x1dc) |
| 0x80157e70 | 0xd4 | GXSetPixelFmt | high | gx 0x1d4/0x1dc/0x204 |
| 0x80157f44 | 0x2c | GXSetDither | medium | gx cmode0 |
| 0x80157f70 | 0x3c | GXSetDstAlpha | medium | gx cmode1 (0x1d4) |
| 0x80157fac | 0x38 | GXSetFieldMask | medium | BP 0x44 |
| 0x80157fe4 | 0x78 | GXSetFieldMode | medium | gx lpSize + BP 0x68 |
| 0x8015805c | 0x218 | GX_unk_8015805c | unk | in module range, no name match; callers=6 hw=8000 callees=0 |
| 0x80158274 | 0x250 | GX_unk_80158274 | unk | in module range, no name match; callers=1 ext=1 callees=10 |
| 0x801584c4 | 0x70 | GXCallDisplayList | high | (list r3, nbytes r4): __GXSetDirtyState if dirty, then FIFO 0x40, list, nbytes |
| 0x80158534 | 0xa4 | GXSetProjection | high | (mtx r3, type r4): stores p[0],p[5],p[10],p[11] + p[3],p[7] if ortho (type 1) else p[2],p[6]; writes XF 0x1020 (was mislabelled GXSetViewport) |
| 0x801585d8 | 0x8c | GXSetProjectionv | high | (f32 p[7]): p[0]==0 -> perspective, stores gx+0x4d8.., XF 0x1020 |
| 0x80158664 | 0x48 | GXGetProjectionv | high | (f32 p[7]) getter: type then the six gx+0x4dc.. floats |
| 0x801586ac | 0x50 | GXLoadPosMtxImm | high | XF 0x1000+id*4 write 12 floats, 19 ext callers |
| 0x801586fc | 0x50 | GXLoadNrmMtxImm | medium | XF normal matrix 9 floats |
| 0x8015874c | 0x34 | GXSetCurrentMtx | medium | gx matIdxA (0x80) |
| 0x80158780 | 0xb4 | GXLoadTexMtxImm | medium | XF tex matrix |
| 0x80158834 | 0x90 | GX_unk_80158834 | unk | in module range, no name match; callers=2 hw=8000 callees=0 |
| 0x801588c4 | 0x58 | GX_unk_801588c4 | unk | in module range, no name match; callers=1 ext=1 callees=1 |
| 0x8015891c | 0x48 | GXSetViewport | high | stores f1-f6 to gx+0x4f4..0x508 (vp left/top/wd/ht/nearz/farz), calls __GXSetViewport (XF 0x101a), 14 ext callers |
| 0x80158964 | 0x24 | GXGetViewportv | high | paired-single loads from gx+0x4f4..0x508 into out ptr |
| 0x80158988 | 0x78 | GXSetScissor | medium | gx suScis0/1 (0xf8/0xfc) |
| 0x80158a00 | 0x40 | GXSetScissorBoxOffset | high | BP 0x59, ((x+342)>>1, (y+342)>>1) |
| 0x80158a40 | 0x28 | GXSetClipMode | medium | XF 0x1005 |
| 0x80158a68 | 0x84 | GX_unk_80158a68 | unk | in module range, no name match; callers=2 hw=8000 callees=0 |
| 0x80158aec | 0x848 | GX_unk_80158aec | unk | in module range, no name match; callers=1 hw=8000 callees=0 |
| 0x80159334 | 0x10 | GX_unk_80159334 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80159344 | 0x10 | TPLGet | medium | returns pal->desc + id*8 (pal+8) |
| 0x80159354 | 0xd4 | TPLGetGXTexObjFromPalette | medium | TPL desc: GXInitTexObj (0x801560c8) + GXInitTexObjLOD |
| 0x80159428 | 0x108 | TPLGetGXTexObjFromPaletteCI | medium | TPL desc (pal+8, 8B/entry): GXInitTlutObj + GXInitTexObjCI + GXInitTexObjLOD |

## AI

11 identified of 19 functions (high 3, medium 8, low 0).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x80141fa4 | 0x44 | AIRegisterDMACallback | medium | swaps r13 callback under Disable/Restore |
| 0x80141fe8 | 0x88 | AIInitDMA | medium | writes DSP regs 0xCC005030/32/36 (AI DMA start/len) |
| 0x80142070 | 0x18 | AIStartDMA | medium | DSP 0xCC005036 /= 0x8000 |
| 0x80142088 | 0x1c | AIStopDMA | medium | DSP 0xCC005036 &= ~0x8000 |
| 0x801420a4 | 0xd8 | AISetStreamPlayState | high | AICR bit 0, SRC init on start |
| 0x8014217c | 0x10 | AIGetStreamPlayState | high | AICR bit 0 |
| 0x8014218c | 0xe0 | AISetDSPSampleRate | medium | AI regs 0xCC006C00; caller in AX init (0x8017980c) |
| 0x8014226c | 0x14 | AIGetDSPSampleRate | medium | AI 0xCC006C00 bit 0x40 |
| 0x80142280 | 0xd4 | AISetStreamSampleRate | high | AICR bit 1, saves/restores play state |
| 0x80142354 | 0x10 | AIGetStreamSampleRate | high | AICR bit 1 |
| 0x80142364 | 0x1c | AISetStreamVolLeft | high | AIVR low byte |
| 0x80142380 | 0x10 | AIGetStreamVolLeft | high | AIVR & 0xff |
| 0x80142390 | 0x1c | AISetStreamVolRight | high | AIVR bits 8..15 |
| 0x801423ac | 0x10 | AIGetStreamVolRight | high | AIVR >> 8 |
| 0x801423bc | 0x16c | AIInit | high | OSRegisterVersion + AI string, AI regs 0xCC006C00, DSP 0xCC005000 |
| 0x80142528 | 0x7c | __AISHandler | medium | AI stream irq; OSSetCurrentContext/OSClearContext, AI 0xCC006C00 |
| 0x801425a4 | 0xac | __AIDHandler | high | AI DMA irq; DSP 0xCC005000, calls __AICallbackStackSwitch |
| 0x80142650 | 0x58 | __AICallbackStackSwitch | medium | stack switch trampoline for AID callback |
| 0x801426a8 | 0x1e4 | __AI_SRC_INIT | high | AI 0xCC006C00, OSGetTime polling loops, 10 r13 vars |

## AR, ARQ, AX, DSP

37 identified of 122 functions (high 12, medium 18, low 7).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x8014288c | 0x44 | ARRegisterDMACallback | medium | swaps r13 callback under Disable/Restore |
| 0x801428d0 | 0x3c | ARGetDMAStatus | medium | DSP 0xCC00500A & 0x200 |
| 0x8014290c | 0xf0 | ARStartDMA | high | DSP 0xCC005020 ARAM DMA regs, 12 callers |
| 0x801429fc | 0x68 | ARAlloc | medium | ARAM bump allocator, r13 b758..b760 |
| 0x80142a64 | 0xc4 | ARInit | high | OSRegisterVersion + AR string, calls __ARChecksize, installs __ARHandler |
| 0x80142b28 | 0x78 | __ARHandler | medium | ARAM DMA irq: ctx set/clear, callback |
| 0x80142ba0 | 0x20 | __ARWaitForDMA | medium | spins on DSP 0xCC005000 bit |
| 0x80142bc0 | 0x10 | __ARClearInterrupt | medium | DSP 0xCC00500A write |
| 0x80142bd0 | 0x17f4 | __ARChecksize | high | 0xdeadbeef ARAM pattern test (0x17f4 bytes) |
| 0x801443c4 | 0x100 | __ARQServiceQueueLo | medium | calls ARStartDMA |
| 0x801444c4 | 0x4 | __ARQCallbackHack | low | empty 4-byte callback |
| 0x801444c8 | 0xcc | __ARQInterruptServiceRoutine | medium | ARQ.c; calls ARStartDMA + ServiceQueueLo |
| 0x80144594 | 0x70 | ARQInit | high | OSRegisterVersion + ARQ string, installs ARQ callback |
| 0x80144604 | 0x15c | ARQPostRequest | high | 8 callers, ARQ.c request enqueue |
| 0x80144760 | 0x24 | AXInit | medium | wrapper, calls AXInitEx-body, ext caller from game audio init |
| 0x80144784 | 0x4c | AXInitEx | medium | OSRegisterVersion + AX string, calls AX sub-inits |
| 0x801447d0 | 0x18 | AX_unk_801447d0 | unk | in module range, no name match; callers=2 callees=0 |
| 0x801447e8 | 0x74 | AX_unk_801447e8 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8014485c | 0xb4 | __AXAllocInit | medium | AXVPB.c; voice stack init |
| 0x80144910 | 0x20 | __AXGetStackHead | low | AXVPB.c |
| 0x80144930 | 0x10 | __AXPushFreeStack | low | AXVPB.c |
| 0x80144940 | 0x1c | __AXPushCallbackStack | low | AXVPB.c |
| 0x8014495c | 0x94 | __AXRemoveFromStack | low | AXVPB.c |
| 0x801449f0 | 0x80 | AXFreeVoice | high | removes from lists, depop if running, pushes free stack 0x8041fa20 |
| 0x80144a70 | 0x174 | AXAcquireVoice | medium | AXVPB.c; 5 ext callers from game audio |
| 0x80144be4 | 0xa8 | AXSetVoicePriority | high | moves VPB to priority list r4, stores r4 at +0xc (was AXFreeVoice) |
| 0x80144c8c | 0xe4 | AX_unk_80144c8c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144d70 | 0x34 | AX_unk_80144d70 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80144da4 | 0x20 | AX_unk_80144da4 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144dc4 | 0x1c | AX_unk_80144dc4 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80144de0 | 0x20 | AX_unk_80144de0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144e00 | 0x20 | AX_unk_80144e00 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144e20 | 0x20 | AX_unk_80144e20 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144e40 | 0x34 | AX_unk_80144e40 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144e74 | 0x1c | AX_unk_80144e74 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144e90 | 0x1c | AX_unk_80144e90 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144eac | 0x1c | AX_unk_80144eac | unk | in module range, no name match; callers=1 callees=0 |
| 0x80144ec8 | 0x23c | AX_unk_80144ec8 | unk | in module range, no name match; callers=1 callees=2 |
| 0x80145104 | 0xc | AXRegisterAuxACallback | medium | r13 globals -0x4868/-0x4860 |
| 0x80145110 | 0xc | AXRegisterAuxBCallback | medium | r13 globals -0x4864/-0x485c |
| 0x8014511c | 0x8 | AX_unk_8014511c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80145124 | 0x3c | AX_unk_80145124 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80145160 | 0x6b0 | AX_unk_80145160 | unk | in module range, no name match; callers=3 callees=13 |
| 0x80145810 | 0x24 | AX_unk_80145810 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80145834 | 0x14 | AX_unk_80145834 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x80145848 | 0x1b8 | AX_unk_80145848 | unk | in module range, no name match; callers=2 callees=12 |
| 0x80145a00 | 0xbc | AX_unk_80145a00 | unk | in module range, no name match; callers=0 callees=4 |
| 0x80145abc | 0xc | AX_unk_80145abc | unk | in module range, no name match; callers=0 callees=0 |
| 0x80145ac8 | 0x58 | AX_unk_80145ac8 | unk | in module range, no name match; callers=0 callees=2 |
| 0x80145b20 | 0x2c | AX_unk_80145b20 | unk | in module range, no name match; callers=0 callees=1 |
| 0x80145b4c | 0xcc | AX_unk_80145b4c | unk | in module range, no name match; callers=1 callees=4 |
| 0x80145c18 | 0x3a0 | AX_unk_80145c18 | unk | in module range, no name match; callers=1 callees=6 |
| 0x80145fb8 | 0x44 | AXRegisterCallback | medium | swaps r13 frame callback under Disable/Restore, returns old |
| 0x80145ffc | 0x4 | AX_unk_80145ffc | unk | in module range, no name match; callers=1 callees=0 |
| 0x80146000 | 0x8 | AX_unk_80146000 | unk | in module range, no name match; callers=0 callees=0 |
| 0x80146008 | 0x3f8 | AX_unk_80146008 | unk | in module range, no name match; callers=1 callees=1 |
| 0x80146400 | 0x2c | AX_unk_80146400 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8014642c | 0x94 | AX_unk_8014642c | unk | in module range, no name match; callers=3 callees=0 |
| 0x801464c0 | 0x8 | AX_unk_801464c0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801464c8 | 0x74c | AX_unk_801464c8 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80146c14 | 0x278 | AX_unk_80146c14 | unk | in module range, no name match; callers=1 callees=7 |
| 0x80146e8c | 0xc | AX_unk_80146e8c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80146e98 | 0x40 | AX_unk_80146e98 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80146ed8 | 0x200 | AX_unk_80146ed8 | unk | in module range, no name match; callers=1 callees=2 |
| 0x801470d8 | 0xc4 | AXSetVoiceSrcType | high | AXVPB +0x140/+0x142, sync 0x1 |
| 0x8014719c | 0x5c | AXSetVoiceState | high | sth +0x146, sync 0x4, depop if 0 |
| 0x801471f8 | 0x254 | AXSetVoiceMix | high | copies AXPBMIX to +0x14a, mixerCtrl +0x144, sync 0x12 |
| 0x8014744c | 0x64 | AXSetVoiceItdOn | high | +0x16e=1, sync 0x20 |
| 0x801474b0 | 0x5c | AXSetVoiceItdTarget | high | +0x178/+0x17a, sync 0x40 |
| 0x8014750c | 0x58 | AXSetVoiceVe | high | +0x19c/+0x19e, sync 0x200 |
| 0x80147564 | 0x4c | AXSetVoiceVeDelta | high | +0x19e, sync 0x400 |
| 0x801475b0 | 0x110 | AXSetVoiceAddr | high | 16 bytes to +0x1a6, PCM gain, sync 0x21000 |
| 0x801476c0 | 0x4c | AXSetVoiceLoop | high | +0x1a6, sync 0x2000 |
| 0x8014770c | 0x54 | AXSetVoiceLoopAddr | high | +0x1aa/+0x1ac, sync 0x4000 |
| 0x80147760 | 0x54 | AXSetVoiceEndAddr | high | +0x1ae/+0x1b0, sync 0x8000 |
| 0x801477b4 | 0x54 | AXSetVoiceCurrentAddr | high | +0x1b2/+0x1b4, sync 0x10000 |
| 0x80147808 | 0xa4 | AXSetVoiceAdpcm | high | 0x28 bytes to +0x1b6, sync 0x20000 |
| 0x801478ac | 0x98 | AXSetVoiceSrc | high | 7 u16 to +0x1de, sync 0x40000 |
| 0x80147944 | 0x98 | AXSetVoiceSrcRatio | high | f1 -> 16.16 clamped 0x40000 at +0x1de, sync 0x80000 |
| 0x801479dc | 0x6c | AXSetVoiceAdpcmLoop | high | 3 u16 to +0x1ec, sync 0x100000 |
| 0x80147a48 | 0x48 | AX_unk_80147a48 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80147a90 | 0x588 | AX_unk_80147a90 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80148018 | 0x190 | AX_unk_80148018 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801481a8 | 0x50c | AX_unk_801481a8 | unk | in module range, no name match; callers=3 callees=0 |
| 0x801486b4 | 0x124 | AX_unk_801486b4 | unk | in module range, no name match; callers=5 callees=0 |
| 0x801487d8 | 0x70 | AXFXReverbHiInit | medium | clears +0x1c4, calls 80147a90 |
| 0x80148848 | 0x4c | AXFXReverbHiShutdown | medium | calls 801486b4, returns 1 |
| 0x80148894 | 0x104 | AX_unk_80148894 | unk | in module range, no name match; callers=0 callees=2 |
| 0x80148998 | 0x47c | AX_unk_80148998 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80148e14 | 0x3b4 | AX_unk_80148e14 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801491c8 | 0x124 | AX_unk_801491c8 | unk | in module range, no name match; callers=4 callees=0 |
| 0x801492ec | 0x6c | AXFXReverbStdInit | medium | clears +0x13c, calls 80148998 |
| 0x80149358 | 0x4c | AXFXReverbStdShutdown | medium | calls 801491c8, returns 1 |
| 0x801493a4 | 0x30 | AX_unk_801493a4 | unk | in module range, no name match; callers=0 callees=1 |
| 0x801493d4 | 0x198 | AX_unk_801493d4 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8014956c | 0x1ec | AX_unk_8014956c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80149758 | 0x248 | AXFXChorusInit | medium | range checks, 0x1680 alloc via AXFX hook |
| 0x801499a0 | 0x50 | AXFXChorusShutdown | medium | frees +0 via AXFX hook |
| 0x801499f0 | 0x110 | AX_unk_801499f0 | unk | in module range, no name match; callers=1 callees=2 |
| 0x80149b00 | 0x350 | AX_unk_80149b00 | unk | in module range, no name match; callers=0 callees=2 |
| 0x80149e50 | 0x1b0 | AX_unk_80149e50 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8014a000 | 0x20 | AX_unk_8014a000 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8014a020 | 0x2c8 | AX_unk_8014a020 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8014a2e8 | 0x48 | AX_unk_8014a2e8 | unk | in module range, no name match; callers=1 ext=1 callees=3 |
| 0x8014a330 | 0xa0 | AX_unk_8014a330 | unk | in module range, no name match; callers=3 ext=1 callees=2 |
| 0x8014a3d0 | 0x28 | __AXAlloc | low | OSAllocFromHeap wrapper for AX |
| 0x8014a3f8 | 0x28 | __AXFree | low | OSFreeToHeap wrapper for AX |
| 0x8014a420 | 0xc | AXFXSetHooks | high | alloc/free hooks r13-0x55a0/-0x559c |
| 0x8014a42c | 0x10 | DSPCheckMailToDSP | high | DSP 0xCC005000 bit 15, 40 callers |
| 0x8014a43c | 0x10 | DSPCheckMailFromDSP | high | DSP 0xCC005004 bit 15 |
| 0x8014a44c | 0x18 | DSPReadMailFromDSP | high | DSP 0xCC005004 read |
| 0x8014a464 | 0x14 | DSPSendMailToDSP | high | DSP 0xCC005000 write, 40 callers |
| 0x8014a478 | 0xc4 | DSPInit | high | string 'Dolphin SDK - DSP', OSRegisterVersion, DSP regs |
| 0x8014a53c | 0x8 | DSPCheckInit | medium | returns r13 init flag |
| 0x8014a544 | 0x70 | DSPAddTask | medium | calls __DSP_insert_task, __DSP_boot_task |
| 0x8014a5b4 | 0xc8 | DSP_unk_8014a5b4 | unk | in module range, no name match; callers=1 hw=5000 callees=2 |
| 0x8014a67c | 0x50 | DSP_unk_8014a67c | unk | in module range, no name match; callers=7 callees=0 |
| 0x8014a6cc | 0x424 | __DSPHandler | high | DSP irq handler; constants 0xdcd10003/0xdcd10002 (task resume codes) |
| 0x8014aaf0 | 0x1a0 | __DSP_exec_task | medium | DSP mail send of task parameters |
| 0x8014ac90 | 0x18c | __DSP_boot_task | high | string 'DSP is booting task: 0x%08X' |
| 0x8014ae1c | 0xa0 | __DSP_insert_task | medium | dsp_task.c; task list insert |
| 0x8014aebc | 0x94 | __DSP_remove_task | medium | dsp_task.c; task list remove |

## CARD

17 identified of 84 functions (high 6, medium 11, low 0).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x8014af50 | 0x4 | __CARDDefaultApiCallback | medium | empty 4-byte callback, first CARDBios fn |
| 0x8014af54 | 0x34 | __CARDSyncCallback | high | calls OSWakeupThread |
| 0x8014af88 | 0xd8 | __CARDExtHandler | high | OSCancelAlarm + EXISetExiCallback |
| 0x8014b060 | 0x118 | __CARDExiHandler | medium | OSCancelAlarm + EXI status read |
| 0x8014b178 | 0xa8 | __CARDTxHandler | medium | EXIDeselect + callback |
| 0x8014b220 | 0x84 | __CARDUnlockedHandler | medium | CARDBios.c |
| 0x8014b2a4 | 0xc0 | __CARDEnableInterrupt | high | EXI imm cmd 0x81000002 |
| 0x8014b364 | 0xf0 | __CARDReadStatus | high | EXI imm cmd 0x83000002 |
| 0x8014b454 | 0xac | __CARDClearStatus | high | EXI imm cmd 0x89000001 |
| 0x8014b500 | 0xa4 | CARD_unk_8014b500 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8014b5a4 | 0x22c | CARD_unk_8014b5a4 | unk | in module range, no name match; callers=2 callees=7 |
| 0x8014b7d0 | 0x110 | CARD_unk_8014b7d0 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014b8e0 | 0x1b4 | __CARDStart | medium | OSSetAlarm/OSCancelAlarm + EXI; called by Read/Write/Erase |
| 0x8014ba94 | 0x134 | __CARDReadSegment | medium | calls __CARDStart |
| 0x8014bbc8 | 0x11c | __CARDWritePage | medium | calls __CARDStart |
| 0x8014bce4 | 0xe0 | __CARDEraseSector | medium | calls __CARDStart |
| 0x8014bdc4 | 0xac | CARDInit | high | OSInitAlarm, OSRegisterVersion, DSPInit, OSRegisterResetFunction |
| 0x8014be70 | 0x8 | CARD_unk_8014be70 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8014be78 | 0x38 | CARD_unk_8014be78 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8014beb0 | 0xb8 | __CARDGetControlBlock | medium | Disable/Restore, 12 callers |
| 0x8014bf68 | 0x64 | __CARDPutControlBlock | medium | Disable/Restore, 43 callers |
| 0x8014bfcc | 0x30 | CARDGetResultCode | high | named from body + game call pattern (card.c) |
| 0x8014bffc | 0x150 | CARDFreeBlocks | high | named from body + game call pattern (card.c) |
| 0x8014c14c | 0x98 | __CARDSync | medium | OSSleepThread loop |
| 0x8014c1e4 | 0x50 | CARD_unk_8014c1e4 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8014c234 | 0x16c | CARD_unk_8014c234 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8014c3a0 | 0x144 | CARD_unk_8014c3a0 | unk | in module range, no name match; callers=4 callees=4 |
| 0x8014c4e4 | 0xc4 | CARD_unk_8014c4e4 | unk | in module range, no name match; callers=4 callees=1 |
| 0x8014c5a8 | 0xb58 | CARD_unk_8014c5a8 | unk | in module range, no name match; callers=1 callees=7 |
| 0x8014d100 | 0x70 | CARD_unk_8014d100 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014d170 | 0x324 | CARD_unk_8014d170 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8014d494 | 0xdc | CARD_unk_8014d494 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014d570 | 0x64 | CARD_unk_8014d570 | unk | in module range, no name match; callers=3 callees=1 |
| 0x8014d5d4 | 0xdc | CARD_unk_8014d5d4 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014d6b0 | 0x64 | CARD_unk_8014d6b0 | unk | in module range, no name match; callers=4 callees=1 |
| 0x8014d714 | 0x8 | CARD_unk_8014d714 | unk | in module range, no name match; callers=5 callees=0 |
| 0x8014d71c | 0xd4 | CARD_unk_8014d71c | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014d7f0 | 0xc8 | CARD_unk_8014d7f0 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014d8b8 | 0x118 | CARD_unk_8014d8b8 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8014d9d0 | 0x9c | CARD_unk_8014d9d0 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8014da6c | 0xac | CARD_unk_8014da6c | unk | in module range, no name match; callers=3 callees=3 |
| 0x8014db18 | 0x8 | CARD_unk_8014db18 | unk | in module range, no name match; callers=13 callees=0 |
| 0x8014db20 | 0xd0 | CARD_unk_8014db20 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014dbf0 | 0xc8 | CARD_unk_8014dbf0 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8014dcb8 | 0xc4 | CARD_unk_8014dcb8 | unk | in module range, no name match; callers=5 callees=3 |
| 0x8014dd7c | 0x1b0 | CARD_unk_8014dd7c | unk | in module range, no name match; callers=5 callees=0 |
| 0x8014df2c | 0x284 | CARD_unk_8014df2c | unk | in module range, no name match; callers=2 callees=4 |
| 0x8014e1b0 | 0x240 | CARD_unk_8014e1b0 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8014e3f0 | 0x284 | CARD_unk_8014e3f0 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8014e674 | 0x8c | CARD_unk_8014e674 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8014e700 | 0x590 | CARDCheckExAsync | high | named from body + game call pattern (card.c) |
| 0x8014ec90 | 0xcc | CARD_unk_8014ec90 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8014ed5c | 0x17c | CARDProbeEx | high | (chan, memSize*, sectorSize*): chan>=2 -> -128, EXIProbeEx -1 -> -3 NOCARD, 0x110-byte control block per channel |
| 0x8014eed8 | 0x410 | CARD_unk_8014eed8 | unk | in module range, no name match; callers=3 callees=15 |
| 0x8014f2e8 | 0x138 | CARD_unk_8014f2e8 | unk | in module range, no name match; callers=4 callees=5 |
| 0x8014f420 | 0x1a0 | CARDMountAsync | high | named from body + game call pattern (card.c) |
| 0x8014f5c0 | 0x9c | CARD_unk_8014f5c0 | unk | in module range, no name match; callers=2 callees=5 |
| 0x8014f65c | 0xac | CARDUnmount | high | named from body + game call pattern (card.c) |
| 0x8014f708 | 0x144 | CARD_unk_8014f708 | unk | in module range, no name match; callers=0 callees=4 |
| 0x8014f84c | 0x658 | CARD_unk_8014f84c | unk | in module range, no name match; callers=1 hw=206e callees=12 |
| 0x8014fea4 | 0x48 | CARDFormatAsync | high | named from body + game call pattern (card.c) |
| 0x8014feec | 0x68 | CARD_unk_8014feec | unk | in module range, no name match; callers=2 callees=0 |
| 0x8014ff54 | 0x94 | CARD_unk_8014ff54 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8014ffe8 | 0x134 | CARD_unk_8014ffe8 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8015011c | 0xf4 | CARD_unk_8015011c | unk | in module range, no name match; callers=2 callees=2 |
| 0x80150210 | 0x150 | CARD_unk_80150210 | unk | in module range, no name match; callers=1 callees=2 |
| 0x80150360 | 0x11c | CARDOpen | high | named from body + game call pattern (card.c) |
| 0x8015047c | 0x54 | CARDClose | high | named from body + game call pattern (card.c) |
| 0x801504d0 | 0x8 | CARD_unk_801504d0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801504d8 | 0x130 | CARD_unk_801504d8 | unk | in module range, no name match; callers=0 callees=6 |
| 0x80150608 | 0x220 | CARDCreateAsync | high | named from body + game call pattern (card.c) |
| 0x80150828 | 0x1b8 | CARD_unk_80150828 | unk | in module range, no name match; callers=2 callees=4 |
| 0x801509e0 | 0x130 | CARD_unk_801509e0 | unk | in module range, no name match; callers=0 callees=3 |
| 0x80150b10 | 0x144 | CARDReadAsync | high | named from body + game call pattern (card.c) |
| 0x80150c54 | 0x170 | CARD_unk_80150c54 | unk | in module range, no name match; callers=0 callees=7 |
| 0x80150dc4 | 0xb0 | CARD_unk_80150dc4 | unk | in module range, no name match; callers=0 callees=2 |
| 0x80150e74 | 0x114 | CARDWriteAsync | high | named from body + game call pattern (card.c) |
| 0x80150f88 | 0xa4 | CARD_unk_80150f88 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015102c | 0x110 | CARDDeleteAsync | high | named from body + game call pattern (card.c) |
| 0x8015113c | 0x1f8 | CARD_unk_8015113c | unk | in module range, no name match; callers=2 callees=0 |
| 0x80151334 | 0x114 | CARDGetStatus | high | named from body + game call pattern (card.c) |
| 0x80151448 | 0x174 | CARDSetStatusAsync | high | named from body + game call pattern (card.c) |
| 0x801515bc | 0x48 | CARDSetStatus | high | named from body + game call pattern (card.c) |
| 0x80151604 | 0xc4 | CARDGetSerialNo | high | named from body + game call pattern (card.c) |

## EXI and SI

46 identified of 59 functions (high 27, medium 18, low 1).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x80169484 | 0x260 | EXI_unk_80169484 | unk | in module range, no name match; callers=1 hw=6828 callees=4 |
| 0x801696e4 | 0x8c | EXI_unk_801696e4 | unk | in module range, no name match; callers=2 callees=3 |
| 0x80169770 | 0x9c | EXI_unk_80169770 | unk | in module range, no name match; callers=2 callees=4 |
| 0x8016980c | 0x54 | EXI_unk_8016980c | unk | in module range, no name match; callers=1 callees=3 |
| 0x80169860 | 0x78 | EXI_unk_80169860 | unk | in module range, no name match; callers=1 hw=6828 callees=3 |
| 0x801698d8 | 0x40 | EXI_unk_801698d8 | unk | in module range, no name match; callers=0 hw=3000 callees=0 |
| 0x80169918 | 0x3c | EXI_unk_80169918 | unk | in module range, no name match; callers=0 callees=0 |
| 0x80169954 | 0xac | EXI_unk_80169954 | unk | in module range, no name match; callers=1 hw=6800 callees=1 |
| 0x80169a00 | 0xdc | EXI_unk_80169a00 | unk | in module range, no name match; callers=1 hw=6800 callees=1 |
| 0x80169adc | 0xdc | EXI_unk_80169adc | unk | in module range, no name match; callers=1 hw=6800 callees=1 |
| 0x80169bb8 | 0xac | EXI_unk_80169bb8 | unk | in module range, no name match; callers=1 hw=6800 callees=1 |
| 0x80169c64 | 0x298 | EXI_unk_80169c64 | unk | in module range, no name match; callers=15 hw=6800/6838 callees=0 |
| 0x80169efc | 0xf4 | SetExiInterruptMask | medium | calls __OSGetInterruptHandler + Mask/Unmask |
| 0x80169ff0 | 0x25c | EXIImm | high | EXI regs 0xCC006800, 17 callers |
| 0x8016a24c | 0xa0 | EXIImmEx | high | loops EXIImm/EXISync |
| 0x8016a2ec | 0xec | EXIDma | high | EXI regs, Disable/Restore |
| 0x8016a3d8 | 0x24c | EXISync | high | calls OSGetConsoleType and __OSGetDIConfig (0xff check) |
| 0x8016a624 | 0x48 | EXIClearInterrupts | high | EXI 0xCC006800 write |
| 0x8016a66c | 0x7c | EXISetExiCallback | high | calls SetExiInterruptMask |
| 0x8016a6e8 | 0x174 | __EXIProbe | medium | OSGetTime + 64-bit div, EXI regs |
| 0x8016a85c | 0x80 | EXIProbe | high | calls __EXIProbe |
| 0x8016a8dc | 0xb4 | EXIProbeEx | medium | calls __EXIProbe |
| 0x8016a990 | 0x10c | EXIAttach | high | EXI.c order |
| 0x8016aa9c | 0xbc | EXIDetach | medium | EXI.c order |
| 0x8016ab58 | 0x12c | EXISelect | high | EXI regs 0xCC006800, 14 callers |
| 0x8016ac84 | 0x110 | EXIDeselect | high | EXI regs, 20 callers |
| 0x8016ad94 | 0xc8 | EXIIntrruptHandler | high | ctx set/clear, EXI regs |
| 0x8016ae5c | 0x218 | TCIntrruptHandler | high | ctx set/clear, EXI regs |
| 0x8016b074 | 0xd0 | EXTIntrruptHandler | high | ctx set/clear |
| 0x8016b144 | 0x1d4 | EXIInit | high | OSRegisterVersion, installs 3 EXI interrupt handlers |
| 0x8016b318 | 0xf4 | EXILock | high | calls SetExiInterruptMask, 12 callers |
| 0x8016b40c | 0xdc | EXIUnlock | high | 27 callers |
| 0x8016b4e8 | 0x18 | EXIGetState | medium | returns channel state |
| 0x8016b500 | 0x28 | UnlockedHandler | medium | calls EXIGetID |
| 0x8016b528 | 0x3b0 | EXIGetID | high | reads device ID via EXIImm, 10 callers |
| 0x8016b8d8 | 0x18c | EXI_unk_8016b8d8 | unk | in module range, no name match; callers=1 callees=8 |
| 0x8016ba64 | 0x1bc | InitializeUART | medium | EXIUart.c; constant 0xa5ff005a |
| 0x8016bc20 | 0x70 | ReadUARTN | low | EXIUart.c |
| 0x8016bc90 | 0x21c | WriteUARTN | medium | EXIUart.c; EXI imm 0xa0010000 |
| 0x8016beac | 0x3c | SIBusy | medium | SI helper called by PADRead |
| 0x8016bee8 | 0x2fc | CompleteTransfer | medium | SI regs 0xCC006400, OSGetTime |
| 0x8016c1e4 | 0x344 | SIInterruptHandler | high | SI regs, calls VIGetCurrentLine |
| 0x8016c528 | 0xb4 | SIInit | high | OSRegisterVersion + SI string, installs SI irq handler |
| 0x8016c5dc | 0x20c | __SITransfer | high | SI regs 0xCC006400/6438 |
| 0x8016c7e8 | 0x7c | SIGetStatus | high | SI 0xCC006438 |
| 0x8016c864 | 0x14 | SISetCommand | medium | SI 0xCC006400 |
| 0x8016c878 | 0x10 | SITransferCommands | medium | SI 0xCC006438 |
| 0x8016c888 | 0x6c | SISetXY | medium | SI 0xCC006430 poll reg |
| 0x8016c8f4 | 0x9c | SIEnablePolling | medium | SI 0xCC006400/6430 |
| 0x8016c990 | 0x6c | SIDisablePolling | medium | SI 0xCC006430, 5 callers |
| 0x8016c9fc | 0xd4 | SIGetResponseRaw | medium | SI 0xCC006400/6438 |
| 0x8016cad0 | 0xc4 | SIGetResponse | high | SI 0xCC006400, 5 callers |
| 0x8016cb94 | 0x8c | AlarmHandler | medium | calls __SITransfer |
| 0x8016cc20 | 0x16c | SITransfer | high | OSSetAlarm + __SITransfer, 11 callers |
| 0x8016cd8c | 0x298 | GetTypeCallback | high | calls OSGetWirelessID/OSSetWirelessID |
| 0x8016d024 | 0x1c4 | SIGetTypeAsync | high | OSGetTime + SITransfer |
| 0x8016d1e8 | 0x13c | SIGetType | high | calls SIGetTypeAsync, 9 callers |
| 0x8016d324 | 0xe4 | SISetSamplingRate | high | string 'SISetSamplingRate: unknown TV format. Use default.' |
| 0x8016d408 | 0x24 | SIRefreshSamplingRate | medium | calls SISetSamplingRate |

## MTX (mtx, mtxvec, mtx44, vec)

30 identified of 30 functions (high 19, medium 11, low 0).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x801394c4 | 0x2c | PSMTXIdentity | high | ps_merge identity, 35 callers |
| 0x801394f0 | 0x34 | PSMTXCopy | high | psq_l/psq_st x12, 84 callers |
| 0x80139524 | 0xcc | PSMTXConcat | high | ps_madd matrix concat, 65 callers |
| 0x801395f0 | 0x50 | PSMTXTranspose | high | ps_merge transpose |
| 0x80139640 | 0xf8 | PSMTXInverse | high | ps_sum/ps_res determinant, 14 callers |
| 0x80139738 | 0xc8 | PSMTXInvXpose | high | inverse-transpose 3x3 |
| 0x80139800 | 0x70 | PSMTXRotRad | high | calls sin/cos (801693dc/80169400) then PSMTXRotTrig |
| 0x80139870 | 0xb0 | PSMTXRotTrig | high | builds rotation from sin/cos |
| 0x80139920 | 0xb0 | __PSMTXRotAxisRadInternal | high | axis-angle ps ops |
| 0x801399d0 | 0x70 | PSMTXRotAxisRad | high | normalise + sin/cos + internal |
| 0x80139a40 | 0x34 | PSMTXTrans | high | identity + translation stores |
| 0x80139a74 | 0x28 | PSMTXScale | high | diagonal scale stores |
| 0x80139a9c | 0xa4 | PSMTXQuat | high | quaternion to matrix ps ops |
| 0x80139b40 | 0x18c | C_MTXLookAt | medium | calls PSVECNormalize/CrossProduct (8013a1c4/a284) |
| 0x80139ccc | 0x94 | C_MTXLightFrustum | medium | 9-arg frustum (stack arg) in mtx.c position |
| 0x80139d60 | 0xcc | C_MTXLightPerspective | medium | calls tanf-like 801693b8 |
| 0x80139e2c | 0x88 | C_MTXLightOrtho | medium | ortho w/ scale/trans in mtx.c position |
| 0x80139eb4 | 0x54 | PSMTXMultVec | high | 179 callers, ps point transform |
| 0x80139f08 | 0x54 | PSMTXMultVecSR | medium | 3x3 rotate only |
| 0x80139f5c | 0x9c | C_MTXFrustum | medium | 4x4 projection, mtx44.c position |
| 0x80139ff8 | 0xd0 | C_MTXPerspective | high | calls tanf 801693b8, 4x4 perspective |
| 0x8013a0c8 | 0x98 | C_MTXOrtho | medium | 4x4 ortho |
| 0x8013a160 | 0x24 | PSVECAdd | medium | vec.c order |
| 0x8013a184 | 0x24 | PSVECSubtract | medium | vec.c order |
| 0x8013a1a8 | 0x1c | PSVECScale | medium | vec.c order (ps_muls) |
| 0x8013a1c4 | 0x44 | PSVECNormalize | high | ps_rsqrte |
| 0x8013a208 | 0x18 | PSVECSquareMag | medium | vec.c order |
| 0x8013a220 | 0x44 | PSVECMag | high | rsqrte + sqrt |
| 0x8013a264 | 0x20 | PSVECDotProduct | high | ps_mul/ps_sum |
| 0x8013a284 | 0x3c | PSVECCrossProduct | high | ps cross product |

## THP (movie decode; Dolphin SDK add-on)

3 identified of 23 functions (high 1, medium 0, low 2).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x801aeb40 | 0x244 | THP_unk_801aeb40 | unk | in module range, no name match; callers=2 ext=2 callees=9 |
| 0x801aed84 | 0x44 | THP_unk_801aed84 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801aedc8 | 0x13c | THP_unk_801aedc8 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801aef04 | 0x11c | THP_unk_801aef04 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af020 | 0x3bc | THP_unk_801af020 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af3dc | 0x1e0 | THP_unk_801af3dc | unk | in module range, no name match; callers=1 ext=1 callees=3 |
| 0x801af5bc | 0xf0 | THP_unk_801af5bc | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af6ac | 0x68 | THP_unk_801af6ac | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af714 | 0x1bc | THP_unk_801af714 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af8d0 | 0x54 | THP_unk_801af8d0 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801af924 | 0x24c | THP_unk_801af924 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x801afb70 | 0x10c | THPVideoDecode | low | calls the three PS decode loops 0x801afc7c/0x801b1704/0x801b3190 |
| 0x801afc7c | 0x1a88 | THP_unk_801afc7c | unk | in module range, no name match; callers=1 ext=1 callees=5 |
| 0x801b1704 | 0x1a8c | THP_unk_801b1704 | unk | in module range, no name match; callers=1 ext=1 callees=5 |
| 0x801b3190 | 0x1aac | THP_unk_801b3190 | unk | in module range, no name match; callers=1 ext=1 callees=5 |
| 0x801b4c3c | 0x67c | THP_unk_801b4c3c | unk | in module range, no name match; callers=12 ext=12 callees=0 |
| 0x801b52b8 | 0x6a8 | THP_unk_801b52b8 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x801b5960 | 0x6a8 | THP_unk_801b5960 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x801b6008 | 0xa0 | THPInit | high | OSRegisterVersion caller (13th), GQR setup spr 914-917 |
| 0x801b60a8 | 0x464 | THPAudioDecode | low | ADPCM-style loop using __shl2i/__shr2i (64-bit shifts) |
| 0x801b650c | 0x90 | THP_unk_801b650c | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x801b659c | 0x3c | THP_unk_801b659c | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x801b65d8 | 0x8 | THP_unk_801b65d8 | unk | in module range, no name match; callers=0 callees=0 |

## other: crt0, init_cpp, MetroTRK, MWCC runtime, MSL C library

38 identified of 321 functions (high 16, medium 18, low 4).

| address | size | name | confidence | evidence |
|---|---|---|---|---|
| 0x80003100 | 0x40 | CRT0_unk_80003100 | unk | in module range, no name match; callers=0 callees=0 |
| 0x80003140 | 0xc | CRT0_unk_80003140 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8000314c | 0x8 | CRT0_unk_8000314c | unk | in module range, no name match; callers=0 callees=0 |
| 0x80003154 | 0x15c | __start | high | crt0 entry at DOL entry point 0x80003154 |
| 0x800032b0 | 0x90 | __init_registers | high | zeroes GPRs, sets r1/r2/r13 from table |
| 0x80003340 | 0xc0 | __init_data | medium | copies/clears data sections via table |
| 0x80003400 | 0x24 | __init_hardware | high | enable FP, calls __OSPSInit and __OSFPRInit |
| 0x80003424 | 0x34 | __flush_cache | high | dcbst/sync/icbi loop |
| 0x80003458 | 0x30 | CRT0_unk_80003458 | unk | in module range, no name match; callers=0 callees=0 |
| 0x80003488 | 0x24 | TRK_memcpy | low | byte-copy loop, 13 TRK callers |
| 0x800053e0 | 0x2c | CRT0_unk_800053e0 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8000540c | 0x30 | memset | high | 126 callers, calls __fill_mem |
| 0x8000543c | 0xb8 | __fill_mem | high | word/byte fill helper |
| 0x800054f4 | 0x2c | memcpy | high | overlap-aware copy, 71 callers |
| 0x80005520 | 0x24 | memcpy_backward | low | backward-copy half of memcpy |
| 0x80139344 | 0x20 | __init_cpp_wrapper | low | calls __init_cpp |
| 0x80139364 | 0x54 | __init_cpp | high | walks _ctors table 0x801b65e0 calling each |
| 0x801393b8 | 0x20 | _ExitProcess | medium | calls PPCHalt |
| 0x80159530 | 0xf8 | TRK_unk_80159530 | unk | in module range, no name match; callers=1 callees=9 |
| 0x80159628 | 0x24 | TRK_unk_80159628 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015964c | 0x18 | TRK_unk_8015964c | unk | in module range, no name match; callers=5 callees=0 |
| 0x80159664 | 0xe0 | TRK_unk_80159664 | unk | in module range, no name match; callers=5 callees=3 |
| 0x80159744 | 0xb4 | TRK_unk_80159744 | unk | in module range, no name match; callers=1 callees=3 |
| 0x801597f8 | 0x58 | TRK_unk_801597f8 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80159850 | 0x28 | TRK_unk_80159850 | unk | in module range, no name match; callers=1 callees=1 |
| 0x80159878 | 0x24 | TRK_unk_80159878 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015989c | 0x14c | TRK_unk_8015989c | unk | in module range, no name match; callers=1 callees=10 |
| 0x801599e8 | 0x44 | TRK_unk_801599e8 | unk | in module range, no name match; callers=6 callees=2 |
| 0x80159a2c | 0xf0 | TRK_unk_80159a2c | unk | in module range, no name match; callers=2 callees=1 |
| 0x80159b1c | 0x98 | TRK_unk_80159b1c | unk | in module range, no name match; callers=1 callees=1 |
| 0x80159bb4 | 0xe8 | TRK_unk_80159bb4 | unk | in module range, no name match; callers=2 callees=1 |
| 0x80159c9c | 0xfc | TRK_unk_80159c9c | unk | in module range, no name match; callers=2 callees=1 |
| 0x80159d98 | 0x68 | TRK_unk_80159d98 | unk | in module range, no name match; callers=11 callees=0 |
| 0x80159e00 | 0xfc | TRK_unk_80159e00 | unk | in module range, no name match; callers=2 callees=1 |
| 0x80159efc | 0x8c | TRK_unk_80159efc | unk | in module range, no name match; callers=2 callees=1 |
| 0x80159f88 | 0xa4 | TRK_unk_80159f88 | unk | in module range, no name match; callers=5 callees=1 |
| 0x8015a02c | 0x30 | TRK_unk_8015a02c | unk | in module range, no name match; callers=8 callees=0 |
| 0x8015a05c | 0x40 | TRK_unk_8015a05c | unk | in module range, no name match; callers=4 callees=1 |
| 0x8015a09c | 0x64 | TRK_unk_8015a09c | unk | in module range, no name match; callers=14 callees=2 |
| 0x8015a100 | 0x2c | TRK_unk_8015a100 | unk | in module range, no name match; callers=7 callees=0 |
| 0x8015a12c | 0xc8 | TRK_unk_8015a12c | unk | in module range, no name match; callers=6 callees=3 |
| 0x8015a1f4 | 0x74 | TRK_unk_8015a1f4 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015a268 | 0x8 | TRK_unk_8015a268 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015a270 | 0xc4 | TRK_unk_8015a270 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015a334 | 0x50 | TRK_unk_8015a334 | unk | in module range, no name match; callers=1 callees=2 |
| 0x8015a384 | 0x60 | TRK_unk_8015a384 | unk | in module range, no name match; callers=1 callees=4 |
| 0x8015a3e4 | 0x13c | TRK_unk_8015a3e4 | unk | in module range, no name match; callers=2 callees=7 |
| 0x8015a520 | 0x4 | TRK_unk_8015a520 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015a524 | 0x88 | TRK_unk_8015a524 | unk | in module range, no name match; callers=4 callees=3 |
| 0x8015a5ac | 0x170 | TRK_unk_8015a5ac | unk | in module range, no name match; callers=1 callees=16 |
| 0x8015a71c | 0x8 | TRK_unk_8015a71c | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015a724 | 0xa8 | TRK_unk_8015a724 | unk | in module range, no name match; callers=1 callees=4 |
| 0x8015a7cc | 0xa8 | TRK_unk_8015a7cc | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015a874 | 0x220 | TRK_unk_8015a874 | unk | in module range, no name match; callers=1 callees=7 |
| 0x8015aa94 | 0xb0 | TRK_unk_8015aa94 | unk | in module range, no name match; callers=1 callees=5 |
| 0x8015ab44 | 0x290 | TRK_unk_8015ab44 | unk | in module range, no name match; callers=1 callees=11 |
| 0x8015add4 | 0x2e0 | TRK_unk_8015add4 | unk | in module range, no name match; callers=1 callees=10 |
| 0x8015b0b4 | 0x23c | TRK_unk_8015b0b4 | unk | in module range, no name match; callers=1 callees=10 |
| 0x8015b2f0 | 0x244 | TRK_unk_8015b2f0 | unk | in module range, no name match; callers=1 callees=8 |
| 0x8015b534 | 0x8 | TRK_unk_8015b534 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015b53c | 0x8 | TRK_unk_8015b53c | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015b544 | 0x58 | TRK_unk_8015b544 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015b59c | 0x58 | TRK_unk_8015b59c | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015b5f4 | 0x78 | TRK_unk_8015b5f4 | unk | in module range, no name match; callers=1 callees=4 |
| 0x8015b66c | 0x64 | TRK_unk_8015b66c | unk | in module range, no name match; callers=1 callees=2 |
| 0x8015b6d0 | 0xc | TRK_unk_8015b6d0 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015b6dc | 0x10 | TRK_unk_8015b6dc | unk | in module range, no name match; callers=4 callees=0 |
| 0x8015b6ec | 0xa8 | TRK_unk_8015b6ec | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015b794 | 0x110 | TRK_unk_8015b794 | unk | in module range, no name match; callers=1 callees=6 |
| 0x8015b8a4 | 0xe8 | TRK_unk_8015b8a4 | unk | in module range, no name match; callers=1 callees=6 |
| 0x8015b98c | 0x11c | TRK_unk_8015b98c | unk | in module range, no name match; callers=1 callees=7 |
| 0x8015baa8 | 0x1e0 | TRK_unk_8015baa8 | unk | in module range, no name match; callers=5 callees=8 |
| 0x8015bc88 | 0x220 | TRK_unk_8015bc88 | unk | in module range, no name match; callers=1 callees=9 |
| 0x8015bea8 | 0x8 | TRK_unk_8015bea8 | unk | in module range, no name match; callers=6 callees=0 |
| 0x8015beb0 | 0x8 | TRK_unk_8015beb0 | unk | in module range, no name match; callers=6 callees=0 |
| 0x8015beb8 | 0x8 | TRK_unk_8015beb8 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015bec0 | 0x98 | TRK_unk_8015bec0 | unk | in module range, no name match; callers=1 callees=5 |
| 0x8015bf58 | 0x38 | TRK_unk_8015bf58 | unk | in module range, no name match; callers=13 callees=0 |
| 0x8015bf90 | 0xb8 | TRK_unk_8015bf90 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015c048 | 0x8 | TRK_unk_8015c048 | unk | in module range, no name match; callers=3 callees=0 |
| 0x8015c050 | 0x8 | TRK_unk_8015c050 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015c058 | 0x26c | TRK_unk_8015c058 | unk | in module range, no name match; callers=2 callees=2 |
| 0x8015c2c4 | 0x118 | TRK_unk_8015c2c4 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015c3dc | 0x24 | TRK_unk_8015c3dc | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015c400 | 0x24 | TRK_unk_8015c400 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015c424 | 0xc4 | TRK_unk_8015c424 | unk | in module range, no name match; callers=2 callees=2 |
| 0x8015c4e8 | 0x10 | TRK_unk_8015c4e8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015c4f8 | 0x18 | TRK_unk_8015c4f8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015c510 | 0x10 | TRK_unk_8015c510 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015c520 | 0x10 | TRK_unk_8015c520 | unk | in module range, no name match; callers=3 callees=0 |
| 0x8015c530 | 0x200 | TRK_unk_8015c530 | unk | in module range, no name match; callers=1 callees=7 |
| 0x8015c730 | 0x10 | TRK_unk_8015c730 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015c740 | 0xb8 | TRK_unk_8015c740 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015c7f8 | 0xac | TRK_unk_8015c7f8 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015c8a4 | 0x84 | TRK_unk_8015c8a4 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015c928 | 0x8c | TRK_unk_8015c928 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015c9b4 | 0x190 | TRK_unk_8015c9b4 | unk | in module range, no name match; callers=1 callees=2 |
| 0x8015cb44 | 0xac | TRK_unk_8015cb44 | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015cbf0 | 0x438 | TRK_unk_8015cbf0 | unk | in module range, no name match; callers=2 callees=3 |
| 0x8015d028 | 0x170 | TRK_unk_8015d028 | unk | in module range, no name match; callers=2 callees=2 |
| 0x8015d198 | 0x50c | TRK_unk_8015d198 | unk | in module range, no name match; callers=2 callees=7 |
| 0x8015d6a4 | 0xf4 | TRK_unk_8015d6a4 | unk | in module range, no name match; callers=2 callees=2 |
| 0x8015d798 | 0x4c | TRK_unk_8015d798 | unk | in module range, no name match; callers=3 callees=1 |
| 0x8015d7e4 | 0x14c | TRK_unk_8015d7e4 | unk | in module range, no name match; callers=3 callees=5 |
| 0x8015d930 | 0x2a4 | TRK_unk_8015d930 | unk | in module range, no name match; callers=5 callees=1 |
| 0x8015dbd4 | 0xc | TRK_unk_8015dbd4 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015dbe0 | 0x8 | TRK_unk_8015dbe0 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015dbe8 | 0x8 | TRK_unk_8015dbe8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015dbf0 | 0x8 | TRK_unk_8015dbf0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015dbf8 | 0x8 | TRK_unk_8015dbf8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015dc00 | 0x18c | TRK_unk_8015dc00 | unk | in module range, no name match; callers=3 callees=0 |
| 0x8015dd8c | 0x2c | TRK_unk_8015dd8c | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015ddb8 | 0x1b8 | TRK_unk_8015ddb8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015df70 | 0x98 | TRK_unk_8015df70 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015e008 | 0x94 | TRK_unk_8015e008 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015e09c | 0x1ec | TRK_unk_8015e09c | unk | in module range, no name match; callers=1 callees=5 |
| 0x8015e288 | 0x134 | TRK_unk_8015e288 | unk | in module range, no name match; callers=1 callees=4 |
| 0x8015e3bc | 0x4c | TRK_unk_8015e3bc | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015e408 | 0x12c | TRK_unk_8015e408 | unk | in module range, no name match; callers=1 callees=2 |
| 0x8015e534 | 0x58 | TRK_unk_8015e534 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e58c | 0x20 | TRK_unk_8015e58c | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015e5ac | 0x58 | TRK_unk_8015e5ac | unk | in module range, no name match; callers=0 callees=5 |
| 0x8015e604 | 0x88 | TRK_unk_8015e604 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e68c | 0x4 | TRK_unk_8015e68c | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e690 | 0x58 | TRK_unk_8015e690 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015e6e8 | 0x30 | TRK_unk_8015e6e8 | unk | in module range, no name match; callers=1 callees=1 |
| 0x8015e718 | 0x30 | TRK_unk_8015e718 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e748 | 0x30 | TRK_unk_8015e748 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e778 | 0x3c | TRK_unk_8015e778 | unk | in module range, no name match; callers=22 callees=0 |
| 0x8015e7b4 | 0x3c | TRK_unk_8015e7b4 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015e7f0 | 0x30 | TRK_unk_8015e7f0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e820 | 0x48 | TRK_unk_8015e820 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e868 | 0x50 | TRK_unk_8015e868 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015e8b8 | 0x26c | TRK_unk_8015e8b8 | unk | in module range, no name match; callers=2 callees=3 |
| 0x8015eb24 | 0x38 | TRK_unk_8015eb24 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015eb5c | 0x34 | TRK_unk_8015eb5c | unk | in module range, no name match; callers=2 callees=4 |
| 0x8015eb90 | 0x10 | TRK_unk_8015eb90 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015eba0 | 0xc | TRK_unk_8015eba0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015ebac | 0x84 | TRK_unk_8015ebac | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015ec30 | 0xbc | TRK_unk_8015ec30 | unk | in module range, no name match; callers=1 callees=3 |
| 0x8015ecec | 0xbc | TRK_unk_8015ecec | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015eda8 | 0x8 | TRK_unk_8015eda8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edb0 | 0x8 | TRK_unk_8015edb0 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edb8 | 0x8 | TRK_unk_8015edb8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edc0 | 0x8 | TRK_unk_8015edc0 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edc8 | 0x8 | TRK_unk_8015edc8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edd0 | 0x8 | TRK_unk_8015edd0 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edd8 | 0x8 | TRK_unk_8015edd8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015ede0 | 0x8 | TRK_unk_8015ede0 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015ede8 | 0x8 | TRK_unk_8015ede8 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015edf0 | 0x24 | TRK_unk_8015edf0 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015ee14 | 0x70 | TRK_unk_8015ee14 | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015ee84 | 0x24 | TRK_unk_8015ee84 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015eea8 | 0x24 | TRK_unk_8015eea8 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015eecc | 0xc0 | TRK_unk_8015eecc | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015ef8c | 0xec | TRK_unk_8015ef8c | unk | in module range, no name match; callers=0 callees=6 |
| 0x8015f078 | 0x8 | TRK_unk_8015f078 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f080 | 0x24 | TRK_unk_8015f080 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f0a4 | 0x8 | TRK_unk_8015f0a4 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f0ac | 0x88 | TRK_unk_8015f0ac | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015f134 | 0x108 | TRK_unk_8015f134 | unk | in module range, no name match; callers=2 callees=3 |
| 0x8015f23c | 0x108 | TRK_unk_8015f23c | unk | in module range, no name match; callers=4 callees=3 |
| 0x8015f344 | 0x50 | TRK_unk_8015f344 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8015f394 | 0x8 | TRK_unk_8015f394 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8015f39c | 0x24 | TRK_unk_8015f39c | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015f3c0 | 0x70 | TRK_unk_8015f3c0 | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015f430 | 0x24 | TRK_unk_8015f430 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015f454 | 0x24 | TRK_unk_8015f454 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8015f478 | 0xc0 | TRK_unk_8015f478 | unk | in module range, no name match; callers=0 callees=2 |
| 0x8015f538 | 0xf4 | TRK_unk_8015f538 | unk | in module range, no name match; callers=0 callees=6 |
| 0x8015f62c | 0x8 | TRK_unk_8015f62c | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f634 | 0x24 | TRK_unk_8015f634 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f658 | 0x8 | TRK_unk_8015f658 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015f660 | 0x88 | TRK_unk_8015f660 | unk | in module range, no name match; callers=0 callees=3 |
| 0x8015f6e8 | 0x50 | __va_arg_save | low | saves r3-r10/f1-f8 for variadic callers (61 callers); mwcc runtime |
| 0x8015f738 | 0x24 | RT_unk_8015f738 | unk | in module range, no name match; callers=2 callees=1 |
| 0x8015f75c | 0x30 | RT_unk_8015f75c | unk | in module range, no name match; callers=2 callees=1 |
| 0x8015f78c | 0x4 | RT_unk_8015f78c | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015f790 | 0xc8 | __va_arg | medium | va_list walk by type 1-4 (gpr/double/stack); 16 callers in game/MSL |
| 0x8015f858 | 0x48 | RT_unk_8015f858 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015f8a0 | 0x5c | __cvt_fp2unsigned | medium | 93 callers, compares f1 against 0.0 and 2^32 |
| 0x8015f8fc | 0x2c | __savefpr_14 | high | stfd f14.. -0x90(r11) save entries |
| 0x8015f928 | 0x8 | RT_unk_8015f928 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015f930 | 0x18 | RT_unk_8015f930 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f948 | 0x2c | __restfpr_14 | high | lfd f14.. restore entries |
| 0x8015f974 | 0x8 | RT_unk_8015f974 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015f97c | 0x18 | RT_unk_8015f97c | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f994 | 0xc | __savegpr_14 | high | stw r14.. -0x48(r11) save entries |
| 0x8015f9a0 | 0x4 | RT_unk_8015f9a0 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f9a4 | 0x4 | RT_unk_8015f9a4 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f9a8 | 0x4 | RT_unk_8015f9a8 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x8015f9ac | 0x4 | RT_unk_8015f9ac | unk | in module range, no name match; callers=5 ext=5 callees=0 |
| 0x8015f9b0 | 0x4 | RT_unk_8015f9b0 | unk | in module range, no name match; callers=5 ext=5 callees=0 |
| 0x8015f9b4 | 0x4 | RT_unk_8015f9b4 | unk | in module range, no name match; callers=10 ext=10 callees=0 |
| 0x8015f9b8 | 0x4 | RT_unk_8015f9b8 | unk | in module range, no name match; callers=10 ext=10 callees=0 |
| 0x8015f9bc | 0x4 | RT_unk_8015f9bc | unk | in module range, no name match; callers=13 ext=13 callees=0 |
| 0x8015f9c0 | 0x4 | RT_unk_8015f9c0 | unk | in module range, no name match; callers=14 ext=14 callees=0 |
| 0x8015f9c4 | 0x4 | RT_unk_8015f9c4 | unk | in module range, no name match; callers=26 ext=26 callees=0 |
| 0x8015f9c8 | 0x18 | RT_unk_8015f9c8 | unk | in module range, no name match; callers=37 ext=37 callees=0 |
| 0x8015f9e0 | 0xc | __restgpr_14 | high | lwz r14.. restore entries |
| 0x8015f9ec | 0x4 | RT_unk_8015f9ec | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f9f0 | 0x4 | RT_unk_8015f9f0 | unk | in module range, no name match; callers=1 ext=1 callees=0 |
| 0x8015f9f4 | 0x4 | RT_unk_8015f9f4 | unk | in module range, no name match; callers=3 ext=3 callees=0 |
| 0x8015f9f8 | 0x4 | RT_unk_8015f9f8 | unk | in module range, no name match; callers=5 ext=5 callees=0 |
| 0x8015f9fc | 0x4 | RT_unk_8015f9fc | unk | in module range, no name match; callers=5 ext=5 callees=0 |
| 0x8015fa00 | 0x4 | RT_unk_8015fa00 | unk | in module range, no name match; callers=10 ext=10 callees=0 |
| 0x8015fa04 | 0x4 | RT_unk_8015fa04 | unk | in module range, no name match; callers=10 ext=10 callees=0 |
| 0x8015fa08 | 0x4 | RT_unk_8015fa08 | unk | in module range, no name match; callers=13 ext=13 callees=0 |
| 0x8015fa0c | 0x4 | RT_unk_8015fa0c | unk | in module range, no name match; callers=14 ext=14 callees=0 |
| 0x8015fa10 | 0x4 | RT_unk_8015fa10 | unk | in module range, no name match; callers=26 ext=26 callees=0 |
| 0x8015fa14 | 0x18 | RT_unk_8015fa14 | unk | in module range, no name match; callers=37 ext=37 callees=0 |
| 0x8015fa2c | 0xec | __div2u | medium | MWCC runtime 64-bit unsigned divide |
| 0x8015fb18 | 0x138 | __div2i | high | used by OSTime/InsertAlarm 64-bit signed divide |
| 0x8015fc50 | 0xe0 | __mod2u | medium | runtime 64-bit unsigned modulo |
| 0x8015fd30 | 0x4 | RT_unk_8015fd30 | unk | in module range, no name match; callers=0 callees=0 |
| 0x8015fd34 | 0x10c | __mod2i | high | used with __div2i in OSTicksToCalendarTime |
| 0x8015fe40 | 0x24 | __shl2i | medium | 64-bit shift left |
| 0x8015fe64 | 0x24 | __shr2u | medium | 64-bit logical shift right |
| 0x8015fe88 | 0x28 | __shr2i | medium | 64-bit arithmetic shift right, 23 callers |
| 0x8015feb0 | 0xcc | RT_unk_8015feb0 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8015ff7c | 0xb8 | InitDefaultHeap | high | string 'GCN_Mem_Alloc.c : InitDefaultHeap. No Heap Available' |
| 0x80160034 | 0xf0 | MSL_unk_80160034 | unk | in module range, no name match; callers=0 callees=5 |
| 0x80160124 | 0x70 | MSL_unk_80160124 | unk | in module range, no name match; callers=2 callees=4 |
| 0x80160194 | 0x58 | MSL_unk_80160194 | unk | in module range, no name match; callers=1 callees=2 |
| 0x801601ec | 0x158 | MSL_unk_801601ec | unk | in module range, no name match; callers=1 callees=1 |
| 0x80160344 | 0x294 | MSL_unk_80160344 | unk | in module range, no name match; callers=2 callees=1 |
| 0x801605d8 | 0x70 | MSL_unk_801605d8 | unk | in module range, no name match; callers=2 callees=1 |
| 0x80160648 | 0xa8 | MSL_unk_80160648 | unk | in module range, no name match; callers=0 callees=4 |
| 0x801606f0 | 0x1a4 | MSL_unk_801606f0 | unk | in module range, no name match; callers=2 callees=1 |
| 0x80160894 | 0x3b4 | MSL_unk_80160894 | unk | in module range, no name match; callers=1 callees=8 |
| 0x80160c48 | 0x1784 | MSL_unk_80160c48 | unk | in module range, no name match; callers=2 callees=2 |
| 0x801623cc | 0x278 | MSL_unk_801623cc | unk | in module range, no name match; callers=3 callees=0 |
| 0x80162644 | 0xc4 | MSL_unk_80162644 | unk | in module range, no name match; callers=5 callees=0 |
| 0x80162708 | 0x34 | MSL_unk_80162708 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8016273c | 0x4 | MSL_unk_8016273c | unk | in module range, no name match; callers=8 callees=0 |
| 0x80162740 | 0x4 | MSL_unk_80162740 | unk | in module range, no name match; callers=8 callees=0 |
| 0x80162744 | 0x4 | MSL_unk_80162744 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80162748 | 0x24 | MSL_unk_80162748 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8016276c | 0x30c | MSL_unk_8016276c | unk | in module range, no name match; callers=1 callees=7 |
| 0x80162a78 | 0x7c | MSL_unk_80162a78 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80162af4 | 0x138 | MSL_unk_80162af4 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80162c2c | 0x1bc | MSL_unk_80162c2c | unk | in module range, no name match; callers=1 callees=4 |
| 0x80162de8 | 0x6c | MSL_unk_80162de8 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80162e54 | 0x270 | MSL_unk_80162e54 | unk | in module range, no name match; callers=1 callees=1 |
| 0x801630c4 | 0xe4 | MSL_unk_801630c4 | unk | in module range, no name match; callers=2 callees=2 |
| 0x801631a8 | 0x118 | MSL_unk_801631a8 | unk | in module range, no name match; callers=1 callees=1 |
| 0x801632c0 | 0x4c | MSL_unk_801632c0 | unk | in module range, no name match; callers=17 callees=0 |
| 0x8016330c | 0x2c | MSL_unk_8016330c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80163338 | 0x2c | MSL_unk_80163338 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80163364 | 0xcc | MSL_unk_80163364 | unk | in module range, no name match; callers=2 callees=4 |
| 0x80163430 | 0xac | MSL_unk_80163430 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801634dc | 0xc0 | MSL_unk_801634dc | unk | in module range, no name match; callers=1 callees=0 |
| 0x8016359c | 0xa8 | MSL_unk_8016359c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80163644 | 0xbc | MSL_unk_80163644 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80163700 | 0x10 | MSL_unk_80163700 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80163710 | 0xe0 | sprintf | medium | 132 callers, calls printf core 0x8016394c |
| 0x801637f0 | 0x98 | vprintf | high | called by OSReport |
| 0x80163888 | 0x6c | MSL_unk_80163888 | unk | in module range, no name match; callers=0 callees=1 |
| 0x801638f4 | 0x58 | MSL_unk_801638f4 | unk | in module range, no name match; callers=0 callees=1 |
| 0x8016394c | 0x774 | __pformatter | medium | MSL printf core (0x774 bytes) |
| 0x801640c0 | 0x71c | MSL_unk_801640c0 | unk | in module range, no name match; callers=1 callees=3 |
| 0x801647dc | 0x12c | MSL_unk_801647dc | unk | in module range, no name match; callers=3 callees=0 |
| 0x80164908 | 0x338 | MSL_unk_80164908 | unk | in module range, no name match; callers=1 callees=3 |
| 0x80164c40 | 0x314 | MSL_unk_80164c40 | unk | in module range, no name match; callers=2 callees=2 |
| 0x80164f54 | 0x258 | MSL_unk_80164f54 | unk | in module range, no name match; callers=3 callees=0 |
| 0x801651ac | 0x504 | MSL_unk_801651ac | unk | in module range, no name match; callers=1 callees=1 |
| 0x801656b0 | 0x30 | MSL_unk_801656b0 | unk | in module range, no name match; callers=3 ext=2 callees=0 |
| 0x801656e0 | 0x40 | MSL_unk_801656e0 | unk | in module range, no name match; callers=2 callees=0 |
| 0x80165720 | 0x128 | MSL_unk_80165720 | unk | in module range, no name match; callers=16 ext=16 callees=0 |
| 0x80165848 | 0x2c | MSL_unk_80165848 | unk | in module range, no name match; callers=14 ext=14 callees=0 |
| 0x80165874 | 0x44 | MSL_unk_80165874 | unk | in module range, no name match; callers=3 ext=1 callees=0 |
| 0x801658b8 | 0xb8 | MSL_unk_801658b8 | unk | in module range, no name match; callers=36 ext=20 callees=0 |
| 0x80165970 | 0x1c | MSL_unk_80165970 | unk | in module range, no name match; callers=16 ext=9 callees=0 |
| 0x8016598c | 0x88 | MSL_unk_8016598c | unk | in module range, no name match; callers=3 callees=0 |
| 0x80165a14 | 0xd0 | MSL_unk_80165a14 | unk | in module range, no name match; callers=0 callees=4 |
| 0x80165ae4 | 0x23c | MSL_unk_80165ae4 | unk | in module range, no name match; callers=1 callees=1 |
| 0x80165d20 | 0x238 | MSL_unk_80165d20 | unk | in module range, no name match; callers=1 callees=1 |
| 0x80165f58 | 0x290 | MSL_unk_80165f58 | unk | in module range, no name match; callers=1 callees=1 |
| 0x801661e8 | 0x224 | MSL_unk_801661e8 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8016640c | 0x33c | MSL_unk_8016640c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80166748 | 0x27c | MSL_unk_80166748 | unk | in module range, no name match; callers=1 callees=0 |
| 0x801669c4 | 0x830 | MSL_unk_801669c4 | unk | in module range, no name match; callers=1 callees=2 |
| 0x801671f4 | 0x3a0 | MSL_unk_801671f4 | unk | in module range, no name match; callers=3 callees=1 |
| 0x80167594 | 0xf4 | MSL_unk_80167594 | unk | in module range, no name match; callers=5 callees=0 |
| 0x80167688 | 0xe54 | MSL_unk_80167688 | unk | in module range, no name match; callers=1 callees=4 |
| 0x801684dc | 0xa0 | MSL_unk_801684dc | unk | in module range, no name match; callers=5 callees=0 |
| 0x8016857c | 0x214 | MSL_unk_8016857c | unk | in module range, no name match; callers=2 callees=0 |
| 0x80168790 | 0x218 | MSL_unk_80168790 | unk | in module range, no name match; callers=3 ext=1 callees=0 |
| 0x801689a8 | 0x28 | MSL_unk_801689a8 | unk | in module range, no name match; callers=3 callees=0 |
| 0x801689d0 | 0xd4 | sin | medium | calls __kernel_sin/__ieee754_rem_pio2, 55 callers |
| 0x80168aa4 | 0x148 | MSL_unk_80168aa4 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80168bec | 0x8c | MSL_unk_80168bec | unk | in module range, no name match; callers=1 callees=0 |
| 0x80168c78 | 0x1c4 | MSL_unk_80168c78 | unk | in module range, no name match; callers=6 callees=1 |
| 0x80168e3c | 0xfc | MSL_unk_80168e3c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80168f38 | 0xd8 | cos | medium | same, 52 callers |
| 0x80169010 | 0x78 | tan | medium | calls __kernel_tan, 12 callers |
| 0x80169088 | 0x20 | MSL_unk_80169088 | unk | in module range, no name match; callers=3 ext=3 callees=1 |
| 0x801690a8 | 0x20 | MSL_unk_801690a8 | unk | in module range, no name match; callers=2 ext=2 callees=1 |
| 0x801690c8 | 0x20 | atan2 | low | wrapper, 26 callers |
| 0x801690e8 | 0x20 | MSL_unk_801690e8 | unk | in module range, no name match; callers=12 ext=12 callees=1 |
| 0x80169108 | 0x20 | MSL_unk_80169108 | unk | in module range, no name match; callers=2 ext=2 callees=1 |
| 0x80169128 | 0x20 | MSL_unk_80169128 | unk | in module range, no name match; callers=5 ext=5 callees=1 |
| 0x80169148 | 0x20 | MSL_unk_80169148 | unk | in module range, no name match; callers=6 ext=5 callees=1 |
| 0x80169168 | 0x224 | sqrt | medium | 0x224 bytes, called by sqrtf |
| 0x8016938c | 0x8 | MSL_unk_8016938c | unk | in module range, no name match; callers=6 ext=6 callees=0 |
| 0x80169394 | 0x24 | MSL_unk_80169394 | unk | in module range, no name match; callers=2 callees=1 |
| 0x801693b8 | 0x24 | tanf | medium | calls tan; used by C_MTXPerspective |
| 0x801693dc | 0x24 | cosf | medium | calls cos; used by PSMTXRotRad |
| 0x80169400 | 0x24 | sinf | medium | calls sin; used by PSMTXRotRad |
| 0x80169424 | 0x20 | sqrtf | medium | 127 callers, calls sqrt 0x80169168 |
| 0x80169444 | 0x4 | MSL_unk_80169444 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80169448 | 0x4 | MSL_unk_80169448 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8016944c | 0x8 | MSL_unk_8016944c | unk | in module range, no name match; callers=2 callees=0 |
| 0x80169454 | 0x8 | MSL_unk_80169454 | unk | in module range, no name match; callers=2 callees=0 |
| 0x8016945c | 0x8 | MSL_unk_8016945c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80169464 | 0x4 | MSL_unk_80169464 | unk | in module range, no name match; callers=1 callees=0 |
| 0x80169468 | 0x4 | MSL_unk_80169468 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8016946c | 0x8 | MSL_unk_8016946c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80169474 | 0x8 | MSL_unk_80169474 | unk | in module range, no name match; callers=1 callees=0 |
| 0x8016947c | 0x4 | MSL_unk_8016947c | unk | in module range, no name match; callers=1 callees=0 |
| 0x80169480 | 0x4 | MSL_unk_80169480 | unk | in module range, no name match; callers=1 callees=0 |

## Open questions

- GX: 51 of 165 functions are still `GX_unk_`. The named set covers vertex descriptor/attr, TEV color/alpha in/op/order, pixel/blend/z, copy/display, channel, texobj and matrix loads. Remaining unknowns (GXTexture region/tlut helpers, bump/indirect, remaining GXLight getters, perf/stub getters) need BP/XF register-id decoding; most are tiny FIFO writers.
- AX: voice-parameter setters (AXSetVoice*, roughly 0x801470d8-0x801479dc, Disable/Restore pattern) and the AXOut/AXAux/AXCL internals are unnamed; their order in the SDK is known but individual identity was not proven. They have ext callers only from the game audio wrapper (0x8016f-0x801a5).
- dvd.c static state machine (stateReady, stateBusy, cbForState*, ...) 0x8013bd34-0x8013e9f4 is largely unnamed; only entry points and the DVDLow layer are named. All of it is replaced by a flat DVD shim in the runtime, so exact names matter less.
- PAD: PADClamp, PADRead, __PADDisableRecalibration and SPEC2_MakeStatus are identified; PADInit/PADReset/PADRecalibrate/PADControlMotor/PADSetSpec are among 0x801414dc/0x801415ec/0x80141a00/0x80141ab8 but their individual names are not proven (note: no PADInit callee `OSRegisterResetFunction` was found inside the PAD range, so PADInit may be inlined in game code).
- CARD: only CARDBios internals and CARDInit are named; the CARDMount/Open/Create/Read/Write/Delete/Stat layer (0x8014c1e4-0x80151604) is unnamed. The game uses it via an HSD memcard wrapper at 0x800ec-0x800ef.
- EXI 0x80169484-0x80169c64 (hw 0xCC006828/0xCC006838, r13 b940-b954) and several functions before EXIInit are unnamed; they may be channel-2 / debugger (EXI2) glue shared with MetroTRK.
- VI: 0x8013f760 (AdjustPosition) is a guess; VIGetNextField/VIGetCurrentLine etc. are position-based.
- OS: OSFont (0x80134ea0-0x80135a58) names are low confidence (font ROM decode); OSThread internals are medium (order-based). OSInitContext/OSCreateThread/OSSetAlarm users were not found as distinct functions; they appear unlinked or inlined.
- Function boundaries: the scan treats jump-table data as non-function. A few mwcc leaf routines that nothing calls by bl and that follow another blr without a prologue may be merged into their predecessor, so per-module totals can be off by a few. MetroTRK, MWCC runtime and MSL are only partially named; they are listed under `other` and are not Dolphin SDK.
- 0x80003100-0x800055e0 (.init) also contains data blobs (strings, ROM copy tables) at 0x800034ac and 0x80005544; those are excluded from the function tables.
- THP names are almost all unknown; the IDCT/Huffman functions (0x801afc7c, 0x801b1704, 0x801b3190) are paired-single code and relevant only if movies (seq_movie.c) are to be decoded natively.

