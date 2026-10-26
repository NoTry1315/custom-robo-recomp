# Recompiler unknowns

Things the translator does not decide. These are not guesses: each one is either
logged here with addresses or turned into a runtime call that reports it
(`recomp_bad`). Fix them against Dolphin, not by changing game logic.

## Semantics decisions (follow Dolphin's interpreter; to verify in phase 7)

- Single-precision ops round the double result to float (`fsgl`). The C operand of
  fmuls/fmadds/ps_mul* is first rounded to a 25-bit mantissa (`f25`), as Dolphin does.
- fmadd family uses C `fma()` then rounds to single (double rounding, same as
  Dolphin). It is slow on 3DS because VFPv2 has no fused multiply-add.
- The ARM FPSCR is already in RunFast mode: the boot log line `# fpscr:` reads 03000000 on the
  game thread and the GX thread in Citra (FZ + DN set, round to nearest, no traps; set by libctru /
  the kernel, this repo never wrote it). So denormals are flushed and NaN payloads are lost, unlike
  Gekko: the game never sets FPSCR[NI] (PPCSetFpNonIEEEMode 0x80131d14 and PPCMtfpscr are never
  called from translated code), so Dolphin keeps denormals. sdmc:/cr_runfast.txt ORs in FZ+DN at
  boot (a no-op while the boot value already has them). Citra battle 4.2 / campaign 8.3 game ms
  19.9 / 9.9 with and without it, logs and fb dumps identical (2026-10-22). The hardware boot value
  is not yet read: check the `# fpscr:` lines in the next hardware log.
- lfs/stfs use the C float<->double conversion. Dolphin uses bit-exact
  ConvertToDouble/ConvertToSingle, which differ for SNaN and for denormals on store.
- fres/frsqrte/ps_res/ps_rsqrte use placeholder `1/x` and `1/sqrt(x)` in runtime/cpu.c.
  Gekko returns table-based estimates; port Dolphin's tables (99 sites, listed below).
- The FPSCR rounding mode (mtfsf/mtfsb1) is stored but not applied. fctiw rounds
  to nearest, using the host default mode.
- psq_st quantization clamps then truncates toward zero, matching Dolphin. NaN input
  has no defined result.
- divw/divwu by zero (and 0x80000000/-1) follow Dolphin's results.
- `blr` is always a C return. Code that loads LR from somewhere other than the stack
  frame or mflr is listed below, because `blr` there may not be a return.
- A branch to another function's entry is a tail call (C call, then return). The
  exception is a backward branch inside the current function, which is a loop. Function
  bodies may overlap; shared code is duplicated.
- Hardware addresses (0xCC00xxxx) in translated code go to hw_read/hw_write. All other
  addresses are masked to 32 MB of RAM.
- Function-start discovery uses: bl targets, the SDK doc, code pointers in data,
  lis+addi/ori pairs, and prologues after terminators. An indirect call to anything
  else reaches `call_indirect`, which reports it.

## Stub runtime (phase 3; to verify against Dolphin)

- Boot info at 0x80000000 is written by runtime/dol.c from Dolphin's HLE BS2 values
  (Boot_BS2Emu.cpp, from memory of the source, not a Dolphin RAM dump yet). Console
  type is Dolphin's LatestDevkit 0x10000006, not retail 0x00000003. ArenaHi
  (0x34) and the FST pointer (0x38) are `(0x81800000 - fst_max) & ~31` as the
  apploader places the FST; the exact value Dolphin reaches is not checked. Phase 4 loads
  sys/fst.bin there. Still not compared with a Dolphin RAM dump of 0x80000000-0x80000100.
- SDK functions without a hand-written body only log; they leave r3 as it was, so a
  caller that uses the return value sees its own first argument. (The phase 3
  DVDOpen r3=0 came from DVDReadDir not filling its entry; fixed by phase 4 DVD.)
  Same trap fixed in phase 4: CARDProbeEx returned chan 0 = READY, OSGetResetButtonState
  returned non-zero (game latches a reset request).
- Interrupts are not asynchronous. VI retrace callbacks run inside VIWaitForRetrace,
  the GX draw-done callback inside GXDrawDone/GXWaitDrawDone. A game that polls a
  flag without calling one of those will hang (the log watchdog reports it).
- OSAlloc heap metadata (HeapDesc array) is kept host-side; cells use the SDK layout
  in guest memory. OSInitAlloc keeps at most 64 heaps.
- PADRead returns `PAD_CHAN0_BIT` (controller 1 only connected); whether this SDK
  version's PADRead returns a motor mask or BOOL is not confirmed.
- OSGetTime/OSGetTick/mftb follow the host clock (40.5 MHz scaled), not emulated
  cycle counts, so timing-dependent paths differ run to run.
- No threads: OSCreateThread/OSResumeThread etc. only log.

## DVD / PAD / ARAM (phase 4; to verify against Dolphin)

- DVD is replaced at the dvdfs level (runtime/dvd.c), not dvdlow: reads come from
  sdmc:/custom-robo/files/<FST path>, found by the FileInfo's startAddr. Async callbacks
  run at the next wait point (VIWaitForRetrace, DVDGetCommandBlockStatus,
  DVDGetDriveStatus); since 2026-10-21 reads run on a reader thread and a read slower than
  a real drive completes at a later wait point (see "DVD reader thread"). Otherwise read
  timing differs from a real drive (no seek/transfer delay).
- DVDGetDriveStatus always DVD_STATE_END, DVDCheckDisk TRUE, DVDCancel only drops a
  pending callback.
- ARAM is a 16 MB host buffer (runtime/ar.c), ARInit reserves 0x4000; ARQ DMA is
  instant, callbacks at the wait point. Bump ARAlloc without ARFree.
- AX frame callback (AXRegisterCallback 0x80145fb8) is called 10 times per 3 retraces
  (200 Hz) from VIWaitForRetrace; mixing since phase 6 (see Audio). Not required for the logo -> title path.
- PAD: 3DS controller -> port 0. Circle pad scaled +-155 -> +-100, C-stick likewise,
  L/R digital -> trigger 0xff, A/B analog 0xff when pressed, ZR -> Z. PADReset,
  PADRecalibrate, PADControlMotor are no-ops returning TRUE. Names of 0x801414dc,
  0x801415ec, 0x80141a00 come from Pad.c order and callees (medium).
- CARDProbeEx (0x8014ed5c) reports CARD_RESULT_NOCARD for both slots; the game polls it
  every other frame. Saving is not covered.
- 0x8015f6e8..0x8015f858 (__va_arg_save, __va_arg, critical-region helpers) were inside
  the MetroTRK range and skipped; the range now ends at 0x8015f6e8 (MWCC runtime).
- Scene state: struct 0x80321d88 (+0 scene, +8 next, +0x10 substate, +0x38 flags, +0x40
  fade); scenes 0 SEQ_LOGO, 1 SEQ_MOVIE, 2 SEQ_TITLE (name table 0x801deab4). The VI log
  line every 30 retraces prints it. Our boot goes LOGO -> TITLE at frame ~300 without
  SEQ_MOVIE (the code path after the second logo sets next = 2). Idling on the title goes to
  SEQ_MOVIE (thpdemo/opening_a.thp) and back: see "Attract movie (THP)" below.

## GX -> PICA200 (phase 5; to verify against Dolphin)

runtime/gx.c. Lines starting `# gx:` in the log are the approximations below, each
logged once when first hit.

- GX state is host-side, set by the hand-written GX calls; CP/XF/BP register writes
  in the command stream are not emulated (`raw GX command .. not emulated`). Draw
  commands reach one parser from three places: GXBegin (opcode + count), the game's
  own WGPIPE writes (hw_write 0xCC008000, inline GXPosition*), and GXCallDisplayList.
  A primitive is drawn when its last byte arrives.
- Transform, lighting and texgen run on the CPU; the PICA gets clip-space positions
  mapped EFB -> 400x240 (the GXSetDispCopySrc rect, 640x480 on the title, scaled 0.5
  and centred: 40 px bars) and rotated for the 3DS framebuffer. Vertex shader is
  pass-through (runtime/vshader.v.pica). Cost: the title's tunnel is ~15k triangles
  per frame on the CPU; real-time Citra drops well below 60 fps there (the log
  watchdog went from 4 s to 20 s for this).
- Depth: clip z is GX's own (both clip to [-w, 0]); the window depth uses GX's
  `far + (far - near) * z/w` via the PICA depth map. The title passes viewport
  near/far = 1 / 1000 (not 0..1), so depth saturates; Dolphin draws the same scene
  correctly, so hardware clamping is assumed, not checked.
- TEV: each GX stage becomes 1-2 PICA texenv stages (lerp, then +-d). Limits: 6 PICA
  stages, 3 texture units, one constant per stage plus the combiner-buffer colour as a
  second per-draw constant ("three different .. constants" when exceeded), no TEV
  output to REG0-2, no bias, no compare ops, no 1/2 scale. Swap mode: only the default
  swap tables, texture side only (R/G/B replicate via PICA operands; alpha unswapped).
  Raster swap ignored. COLOR1 channel not used (only COLOR0/ALPHA0 is rasterized).
- Texgen: MTX2x4/3x4 from POS/NRM/TEXn; projective 3x4 divided per vertex; post-
  transform (dual) texture matrices ignored (`post-transform texture matrix 64` on the
  title). Likely cause of the main title difference: Dolphin shows a circuit-board
  pattern on the tunnel, we show only its blue glow streaks.
- Culling off (`cull mode 2 drawn without culling`); GX winding vs the rotated PICA
  framebuffer not worked out. Lines/points not drawn. Alpha compare: one comparison
  (the one that is not ALWAYS). Logic-op blend drawn as no blend.
- Lighting: diffuse with GX_AF_SPOT distance/angle attenuation; GX_AF_SPEC is treated
  as no attenuation. Only 8 GXLoadLightObjImm calls seen up to the title.
- Textures: decoded from guest RAM to RGBA8, padded to a power of two (edge repeat)
  and cached by address/size/format with an FNV hash of the data, re-hashed once per
  frame per texture; TLUT contents are hashed at decode only (a palette changed in
  place at the same address mid-frame would be missed). GX texel row y is PICA
  memory row y, sampled at v = 1 - y/H. Min/mag filter: linear or nearest only, no
  mipmaps/LOD bias. GXInitTexObjCI with format 0 (the dialog font) decodes as I4, as
  the hardware does.
- GXTexObj / GXTlutObj / GXLightObj use the SDK's big-endian layouts in guest
  memory (the game copies texobjs word by word); texobj mode1 (LOD) is not filled.
- Frame: GXCopyDisp ends the citro3d frame; clear (copy-clear colour and z) happens
  at the start of the next frame when the copy asked for it. GXCopyTex, EFB peeks,
  fog, dither, z-compare location, dst alpha: not implemented (logging stubs).
- TPLGet / TPLGetGXTexObjFromPalette(CI) (TPL library, end of the GX range) are hand-
  written; they read the bound TPL in guest memory (the game binds it itself).

## Boot path to the title (phase 5)

- With no memory card (CARDProbeEx = NOCARD) the game shows "There is no Memory Card
  in Slot A" and further dialogs before the title; Dolphin, with a card, goes straight
  to it. Autotests press A at retraces 360/450/540/630:
  `autotest.ps1 -Pad '360 366 100; 450 456 100; 540 546 100; 630 636 100'`
  (sdmc:/cr_pad_script.txt, read by runtime/pad.c). Title (scene 2 sub 4) from ~600.
- The title starts sound voices: AXAcquireVoice / AXFreeVoice are hand-written over a
  64-voice pool at guest 0x81800000 (above the GameCube's 24 MB, unused by the game)
  because AXInit does not build the SDK pool and both stub results (its own argument,
  or NULL) hit the synth.c "entry" assert. Mixed since phase 6 (see Audio).
- Names fixed this phase in docs/sdk_functions.md: 0x80158534 is GXSetProjection (was
  GXSetViewport), 0x8015891c GXSetViewport, 0x80156b38 __GXSetTmemConfig (was
  GXSetTevOp), 0x80157100 GXSetTevOp, 0x801568ac GXInvalidateTexAll (was GXLoadTlut),
  0x801566e8 GXLoadTlut, 0x801584c4 GXCallDisplayList.

## Interrupt delivery at back-edges (2026-10-18)

- Fixes the pause hang above. recomp.py emits `RECOMP_POLL();` before every backward
  intra-function goto (3327 sites); it tests `recomp_event_pending` only.
- runtime/ar.c: each ARQ request is due at post time + 400 + len/2 timebase ticks
  (~10 us + ~80 MB/s). `recomp_poll_events` runs due callbacks in order when MSR[EE]
  is set; wait points (retrace, DVD poll) still run all pending ones at once, so the
  title path is as before. DVD completions are still wait-point only.
- Citra (user-driven): after the campaign intro, Start opens the pause menu (scene c);
  Return to Game etc. shown. Hardware not yet tested.

## Battle (2026-10-18, Citra, replay of a played log)

- P1 vs P2 reaches the battle. Spawn crash: libctru GPUCMD_AddInternal svcBreak, the
  GPU command buffer (1 MB) full; a battle frame is ~2200 draws x ~112 words. A
  C3D_FrameSplit does not help (it hands back the rest of the same buffer: the size
  shrank 262144 -> 912 -> 120 words). Now C3D_Init 2 MB (56% used) and a frame that
  still fills it skips its remaining draws ("# gx: GPU command buffer full").
- GX state dirty tracking (2026-10-18): setup_tev runs only when its inputs (stages,
  TEV/K colours, texmap->unit, COLOR1 state) changed; depth test/map, blend, alpha
  test, cull, scissor and texture binds are set only when their arguments changed
  (citro3d re-sends every state that was set at the next draw). Uniforms were
  already shadowed. Side effect: `# gx gaps` counts of TEV gaps now count TEV
  changes, not draws. Replays at r3000-3600 (Citra):
  battle 2.7 -> 3.1 fps, gx state 113.6 -> 64.9 ms, cmdbuf 55% -> 18% of 2 MB;
  campaign 4.3 -> 4.6 fps, gx state 56 -> 39 ms, cmdbuf 33% -> 11%.
  The rest of gx state is C3D_DrawElements (attribute/uniform upload per draw);
  gx vertex (~120 ms battle) and game (~118 ms) are now the larger costs.
- White robo skins, launch cannons, robo cubes (fixed): the robo TEV program is
  REG2 = tex0 * K2a (+ C0); REG2 *= COLOR0; PREV = tex1 * K1a (+ C1);
  PREV = PREV * COLOR1 + REG2 (diffuse texture + specular-lit second texture).
  setup_tev now allocates GX registers per side (rgb/alpha): a register is a
  per-draw constant until written, then lives in PICA previous; when another
  register is written while it is still read later, its last PICA stage sets the
  combiner-buffer update bit (stages 0-3, read from two stages later). Constants are
  placed per PICA stage (stage constant, then the buffer's initial colour while no
  update is scheduled); a lerp of two constants without a free buffer is split as
  b*c + a*(1-c) folded on the CPU. Constants equal to 0/255 simplify the ops.
- COLOR1/ALPHA1 are not lit (the shader computes COLOR0/ALPHA0 only): TEV reads of
  COLOR1 get the unlit part (amb*mat when enabled, else mat). The robo's specular
  term (COLOR1 = GX_AF_SPEC light 1) is therefore black: no highlights yet.
- Remaining gaps in battle: cull mode, lines/points, post-transform texmtx,
  projective texgen, EFB copy (TEXC without texture), two-sided alpha compare.
- `# gx gaps` (every 300 frames) counts hits per unemulated feature; `# gx tex miss`
  names each texture drawn without its image and why.
- Vertex buffer: 0x18000 vertices per half, u16 indices rebased past 0x10000.

## Hardware stalls and jaggies (2026-10-19, New 3DS)

- Multi-second single-frame stalls (4 s in campaign/battle, 11 s at boot) were SD
  writes: `# stall` lines showed log writes of up to 4.1 s for one fwrite+fflush.
  recomp_log now formats into a 1 MB memory ring and a writer thread (priority below
  the game) writes it every ~100 ms; crash, hang and exit drain it synchronously.
  `# time` lines report the time spent in recomp_log ("log writes").
- 2x supersampling (800x480 target, display-transfer downscale) was tried and
  reverted: on hardware startup and gameplay got clearly slower. It is not the fix
  for the jagged look; that is still open (other options: edge AA via the GX
  pipeline, mipmaps for minified textures).

## Vertex cache (2026-10-18, Citra replays)

- Profile before (battle r3600): gx vertex 124 ms = uniforms 24 (set_clip/lighting/
  mtx/texgen), decode 86, rest 14; 99% of it from GXCallDisplayList draws.
- Display-list draws go through `vc_draw` (runtime/gx.c): 16384-slot 2-way cache keyed
  by vertex data address + count + signature (trimmed plan, template vertex: current
  matrix, register material, texcoord defaults). A hit needs the hash of the draw's
  vertex bytes and of every indexed array to match. Each array base is hashed once per
  frame over the largest extent any cached draw read (assumption: the game does not
  rewrite a vertex array between two display-list draws of one frame that read it).
- Decoded vertices live in a 0x14000-vertex linear arena (5.4 MB) drawn from directly
  (attribute buffer switched per 32k-vertex window, cmdbuf +1%). Bump allocation; the
  arena is emptied at a frame start when 3/4 full or after it filled (C3D_FRAME_SYNCDRAW:
  the GPU is done with the previous frame). vbuf shrank to 0x6000 vertices per half
  (immediate draws, cache overflow): linear memory is tight (65 KB free in battle), a
  bigger arena made C3D_TexInit fail. Arena reset frames are slower (worst ~290 ms).
- decode_vtx: template vertex copy, fixed float destination per attribute, no switch.
- Battle ~48 misses/frame (vertex data that changes per frame). Results, gx vertex:
  battle 124 -> 63 ms, campaign 84 -> 39, title 28 -> 7 (30 fps). Battle, campaign and
  title logs identical to the pre-change build except cmdbuf %.
- Next: uniform setup per draw (24 ms battle), the hit path's list hash.

## Translator performance (2026-10-18, Citra replays)

- Guest registers are C locals inside each translated function (`Func`, `localize_line()` in
  recomp.py): r0-r31 as `rN`, FPRs as `PS fpN`, each CR field as `crN` (low 4 bits,
  compares write `cmp_s/cmp_u/fcmp_v | so`), CTR, XER[CA] (`ca`, helpers `addc_l`,
  `sraw_l`, `srawi_l`) and XER[SO]. Loaded at entry, stored to `cpu` / reloaded around
  calls, returns, polls and system calls. Before, every R[n] was a global access the
  compiler had to redo after each guest store (-fno-strict-aliasing, `ram` a pointer).
- Calls follow the PPC EABI (assumption, not checked per function): `bl` stores only
  r1, r0/r3-r12, f1-f8, cr1 and reloads only the volatile registers (r0, r3-r12,
  f0-f13, cr0/1/5-7, ctr, ca, so); returns store r0/r3-r12, f1-f4, ctr, ca. The `cpu`
  copies of r14-r31/f14-f31/cr2-4 can therefore be stale while a caller keeps them in
  locals; only MWCC's _savegpr/_restgpr stack slots see that. Tail branches, polls
  (interrupts), recomp_bad, sc/trap/rfi/mtmsr store and reload everything. Breaks if
  translated code reads a caller's non-volatile register through `cpu` (OSSaveContext /
  thread switches, setjmp-style code): if threads are emulated later, make the context
  switch go through a full spill. (Since 2026-10-19 the fixed sets are only upper bounds:
  see "Register spills per call site".)
- Single and paired madd (`fmadds`, `ps_madd`, `ps_madds0/1`, n/msub) use `a*c + b`
  (`fmadd_s`) instead of `fma()`: libm fma is software on VFPv2. Exact when `a` is a
  single (24-bit) and `c` went through f25 (25 bits): the product fits a double, so one
  rounding as with fma. Double `fmadd` still uses `fma()`.
- psq_ld/psq_st take and return PS by value, fast path for GQR type 0, scale 2^n built
  from float bits (was `ldexpf`, a libm call per element).
- r1-relative loads/stores use `rd32_ram`/`wr32_ram` (no IS_HW check). Guest RAM is a
  static array (`ram[]` in cpu.c), so its base is a constant.
- Profiling: `make FNPROF=1` (after `make clean`) makes gen/ store the running function
  in `recomp_cur_fn` and `# prof` lists functions (00000000 = SDK/runtime). Battle: 80%
  of samples outside game code, the game part flat (top function 2.4%, PSMTX-style code).
  Since 2026-10-21 SDK and native functions show under their names and 00000000 is the
  call-boundary glue only (see "Per-model cost (2026-10-21)").
- Results, `# time` at r3600 / r3000:
  battle game 118.4 -> 75.4 (locals) -> 69.5 (EABI calls) -> 58.7 ms (fmadd_s, stack);
  campaign 4.6 -> 5.4-5.7 fps (game 38-45 ms); title 15 fps, game 21.7 ms.
  Battle and campaign replays give the same scene/SDK-call log as their recordings.
- Not done: `| so` in every compare, HW check on non-stack accesses. (Dirty tracking of
  spills: done 2026-10-19, see "Register spills per call site".)

## Register spills per call site (2026-10-19)

recomp.py now translates every function to a per-instruction form (`Func`), computes
interprocedural register summaries and emits each call's stores/reloads separately
(`Summaries`, `analyze`, `dirty_sites`, `emit`):

- in(F): CALLU registers (r0, r1, r3-r12, f1-f8, cr1, so) whose `cpu` value at entry F
  (or its callees, transitively) may read before writing; clob(F): registers whose `cpu`
  value F may change (its stores plus its callees' clob). Least fixpoint over the call
  graph (worklist; recursion fine). call_indirect: in = CALLU, clob = all volatiles.
- SDK natives (runtime/, sdk_stubs.c): in = CALLU (LOG() prints r3-r6), clob = r3, r4, f1.
  Checked at generation time: `check_native_writes()` fails the run if runtime/*.c writes
  any other guest register (allowed: cpu.c boot and recomp_call_guest, which saves and
  restores all of cpu; mtx.c bench, same). memcpy/memset in r3-r5 clob none, sqrt f1/f1;
  MTX natives = the translated body's summary plus r3-r5/f1-f3 read, r3/r6/f1 written.
- Call site: store a register only if it is dirty (written since the last store or reload,
  forward may-analysis) and either the callee may read it or the callee may change it and
  it is still needed afterwards. Reload a volatile register only if the callee may change
  it and a later read or store needs it; otherwise its value stays in `cpu` and the stale
  local counts as clean, so it is never stored back (this keeps a callee's r3 return value
  on paths like `li r3,0; beq 1f; bl G; 1: blr`). Stores count as reads for that decision
  (iterated per function until stable). Exception (until 2026-10-22): ctr, ca, cr0, cr5-7
  were still reloaded after every call. Keeping them in locals when the callee cannot change
  them made the campaign replay rebuild 4 GX display lists every frame from r1500 (dl cache
  misses; SDK calls and scenes unchanged). Cause found 2026-10-22: no guest code reads them
  after a call; the misses were the build-dependent DL cache set conflict (see "Call-boundary
  glue (2026-10-22)").
- Returns store only dirty RETU registers; tail branches every dirty register (a tail
  target can be shared code of the same function, e.g. 0x8009d5ec, a common epilogue
  reached from several functions); recomp_bad/sc/trap/rfi/mtmsr store every dirty register
  and reload all. Polls (loop back-edges) store only r1 and reload
  nothing: recomp_poll_events runs callbacks through recomp_call_guest, which uses R[1] for
  the callback stack and restores all of cpu afterwards.
- The old fixed sets stay the universes, so a store is only ever skipped, never added, and
  every skipped store is of a value `cpu` already holds. New assumption (EABI, as for the
  non-volatile registers before): a volatile register the callee may change is dead after
  the call unless it is a value the caller reads; before, r0/r3-r12 were exact across calls
  (f0/f9-f13/cr0/cr5-7/ctr/ca were already trashed). Volatiles the callee cannot change are
  now kept in locals (machine-exact where the old code reloaded stale copies).
- MWCC's _savegpr_N/_restgpr_N/_savefpr_N/_restfpr_N (0x8015f8fc-0x8015fa28; ~170 + ~170
  call sites, mostly gpr) are emitted inline at each `bl` (same stw/lwz/stfd/lfd on r11, on
  the caller's locals, cpu.lr still set). The saved values are the same as before: the
  caller's locals for r14-r31 equal the (possibly stale, see above) `cpu` copies at that
  point in the prologue; _restgpr now loads into the caller's locals, which are dead after it.
- Static counts (13214 call sites): register stores 151596 -> 53779, reloads 183950 ->
  62361. custom-robo-recomp.3dsx 10.65 -> 6.73 MB.
- 3dsx relocations: with fewer reloads GCC knows more guest pointers' ranges, and IVOPTS
  turned `ram + (p & RAM_MASK)` in a loop of f_8019def4 into a literal `ram + 0x7fbb6160`.
  The 3dsx format keeps a relocation sub-type in the top 4 bits of the patched word, so
  Citra refused the ROM ("Error while loading ROM!", Load3DSXFile sub_type != 0) before
  any log line. The Makefile now builds gen/ (`recomp_%.o`) with `-fno-ivopts` and runs
  `tools/check_3dsx.py` after every link: it fails the build on any relocation a loader
  rejects. (runtime/ keeps IVOPTS; base build size was unchanged by the flag.)
- Found on the way, not changed: 5 functions start executing below their entry address
  (body has code under the entry and the C function begins at the lowest address):
  f_80020004 (at 8001fbb0), f_8009adc4, f_800b0004, f_80120010, f_80120030; printed by
  recomp.py as `note:`.
- Citra replays (2400 retraces, cr_card_off.txt, builds of main 4940496 + this branch, both
  with -fno-ivopts): title (A-press pad), campaign (pause-ok) and battle (rec-to-battle)
  give the same SDK calls with r3-r6, scenes, pad lines and call counts as the base build.
  Title and campaign logs are identical apart from timing; battle differs in one line,
  `# gx vcache frame 2100` arena 3339 KB (base, same in two runs) vs 3309 KB: some
  CPU-computed vertex data differs (open; FLOATOPS showed the same symptom, see below).
  `# time` game ms, average of the 40 lines, base -> new: title 6.53 -> 6.17, campaign
  11.92 -> 11.38, battle 15.07 -> 14.40 (-4.5 to -5.5%); frame 29.36 -> 28.75 ms in battle.
  Citra times are instruction-count based, not hardware; not yet run on a New 3DS.
- Test runner lesson: autotest.ps1 waits up to -Timeout for `# DONE` and cannot see Citra's
  error dialogs, so a ROM that does not load looks like a slow run. Check the dialog title
  (e.g. EnumWindows on citra-qt) or a log that stops growing (it grows every 30 frames).

## Perf wins 2026-10-19

- GX: C3D_FrameEnd(GX_CMDLIST_FLUSH). Every CPU-written linear buffer the GPU reads is
  flushed by gx.c: vbuf/ibuf (used part, gx_frame_end), vc_arena dirty range, each
  texture after decode (tex_upload). The command list is flushed by GX_ProcessCommandList.
  ndsp buffers are flushed by ax.c (DSP_FlushDataCache), not affected.
- log.c: sampler/watchdog/log_writer via aux_thread(): core 2 probed with a throwaway
  thread, else core 1 after APT_SetAppCpuTimeLimit(30), else core 0. Citra: core 2.
- Native libc (runtime/libc.c, recomp.py NATIVE, disassembly-checked): memcpy 0x800054f4
  (byte copy, backward when src < dst = memmove), memset 0x8000540c (via __fill_mem),
  sqrt 0x80169168 (+-0 -> x; x < 0 -> errno 0x8044fce8 = 33 and NaN from 0x8044eea8;
  inf/NaN -> x*x+x and errno 33, even for +inf). Open: if the 3DS FPSCR flushes
  denormals, sqrt of a denormal differs (guest normalizes). __fill_mem and
  "memcpy_backward" 0x80005520 (the backward half of memcpy, not a function) are now
  unreached words (459 -> 616).
- `# time r<n> split: before first draw X, from first draw Y` (PROF_PREDRAW). Citra,
  2400-retrace replays, before -> after (ms/frame):
  battle 125.1 -> 124.2 (split 3.2 / 121.1), campaign 134.1 -> 134.3 (1.9 / 132.4),
  title 33.7 -> 33.4 (0.7 / 32.7). Game code runs interleaved with draws, so "game"
  cannot be overlapped with GX by splitting the frame at the first draw.
- Title autotest with the A-press pad script ends in OSPanic before and after (card is
  now present, so the pad script no longer matches the dialogs; not caused by this).
  Later found to be the attract movie panic (seq_movie.c:184), fixed 2026-10-21.
- Hardware run 8 (2026-10-19, interactive, SD cr_autotest_2026-10-19_hw-run8-perf-wins.log):
  helper threads on core 2. Battle (scene 4 sub 2) 105-122 ms = 8.2-9.5 fps (run 4: 165 ms,
  6 fps): game 45-60, gx vertex 26-28, gx state 21, gx tex 10-12, end/wait 0.4-0.6; split
  2.3 ms before first draw. Title 60 fps (16.7 ms, game 12-13); scene 7 (menus/customize)
  20 fps (50 ms: game 28, vertex 8.5, state 7.8). Campaign not played. Boot stall: the
  first read of scenario_us/snr_map.BIN (0x800 bytes) took 3492 ms (DVD slow r44), like
  the 4 s title-jingle read of runs 6/7: first open of a file far into the disc tree?
## GX draw batching (2026-10-19)

- Consecutive draws with no PICA state change in between (same vertex buffer window,
  indices contiguous in ibuf) are merged into one C3D_DrawElements: every citro3d state
  call in gx.c is preceded by `pend_flush()` (texture bind/params/upload, TEV, pixel
  state, uniforms, light-loop bools, buffer switch, clear, frame end). Cap 0xfff0 indices.
- `# gx batch frame <n>` (every 300 frames): draws, mergeable, DrawElements issued.
  sdmc:/cr_batch_off.txt = batching off (counts still logged).
- Mergeable: battle 91% (2200 -> ~190 DrawElements), campaign 91% (1260 -> ~105),
  title 86% (185 -> 25).
- Citra `# time`, off -> on: battle r3600 4.6 -> 5.4 fps, gx state 64.3 -> 30.5 ms;
  campaign r3000 7.5 -> 8.5 fps, gx state 39.6 -> 18.5 ms; title 30 fps both, gx state
  5.6 -> 2.6 ms. gx vertex unchanged (uniforms ~25 ms in battle, decode ~25 ms).
  Replays (rec-to-battle, pause-ok, title pad; cr_card_off.txt) give identical scene/SDK
  logs; battle screenshots look the same. Not yet on hardware.
- Uniform setters memoized: set_clip, set_texgen and set_lighting return early when
  their inputs equal the previous call's (light_gen bumped by GXLoadLightObjImm, xf_gen
  for matrices; texgen not memoized with per-vertex TEXnMTXIDX). Citra uniforms: battle
  25.8 -> 19.7 ms (5.4 -> 5.5 fps), campaign 16.6 -> 11.2 ms (8.5 -> 8.8 fps).
- Index lists cached per vertex-cache entry (vc_ix, copied into ibuf on a hit): no
  measurable gain (gx vertex "rest" 14.3 -> 14.8 ms in battle); kept, cheap to drop.
- Replays still identical to the batching-off logs. Remaining gx vertex: decode ~25 ms
  and uniforms ~20 ms in battle.

## Float ops and native MTX (2026-10-19)

Profile (Citra battle replay r3600, `make FNPROF=1`): game-side top 0x801a4558 11.5% and
0x8006eef8 7.4% (mostly GX runtime time charged to the caller), 0x80090710 4.5%,
0x80060fa4 2.9%, PSMTXConcat 2.5%, PSMTXInvXpose 1.4%, gmtx_mul 0x8001e134 / keyframes
0x8001eb48 ~1% each, PSMTXMultVec 0.9%.

- Translator: single-precision ops are emitted as `SADD SSUB SDIV SMUL SMADD SMSUB(a, c[, b])`
  (ppc.h); the default definitions are the old `fsgl(..)` + `f25(c)` + `fmadd_s` code (gen/
  output is the same arithmetic as before).
- `make FLOATOPS=1` (RECOMP_FLOATOPS): SMUL/SMADD/SMSUB computed in float. Roundings that change
  vs Gekko/Dolphin:
  - c is no longer cut to 25 bits (f25); a and c are rounded to float first (only matters
    when an FPR holds a double, e.g. a double-op result or f1 argument);
  - madd/msub/nmadd/nmsub round twice (product to float, then sum) instead of once;
  - flush-to-zero is the same (RunFast applies to both).
  add/sub/div stay double + fsgl: a first version did them in float too and broke the
  int->float idiom (`fsubs` of a double magic constant): AISetStreamVolRight got 0 instead of
  255 at boot, and battle/campaign replays froze in scene 5 (r1710 / r1230).
- FLOATOPS replays (battle r3600, campaign r3000, title r2400): same SDK call sequence, call
  counts and scene path as the default build; only the battle vertex-cache arena differs
  (3325 vs 3309 KB at frame 2100), i.e. some CPU-computed vertex values change by ulps.
  Bench, float vs exact on random inputs in [-4, 4) (max ulp): Concat 1608, MultVec 2048,
  MultVecSR 444, InvXpose 1047, Inverse 7641, Dot 1280, Cross 303, Normalize 2, Mag 3,
  SquareMag 2 (large values = cancellation: absolute error stays ~1 ulp of the inputs).
- Native MTX (`runtime/mtx.c`, `mtx_impl.h`): 18 functions (Identity, Copy, Concat, Transpose,
  Inverse, InvXpose, Trans, Scale, MultVec, MultVecSR, VEC Add/Subtract/Scale/Normalize/
  SquareMag/Mag/DotProduct/CrossProduct). recomp.py MTX_NATIVE sends calls to sdk_<name>;
  the f_ bodies are still built (weak stubs call them; sdmc:/cr_mtx_off.txt routes all calls
  back). Same ppc.h ops in the guest order, so bit-identical in each mode (bench: 0 ulp vs
  translated for all 18, both modes). Concat sets r6 = 0x8044ed88 like the guest (it shows in
  SDK logs). Not native: RotRad/RotTrig/RotAxisRad, Quat, LookAt, C_MTX* (call libm, cold).
  Assumes GQR0 = 0; Copy/Identity/Transpose move raw words (NaN payloads kept).
- MTX replays: battle/campaign/title identical to cr_mtx_off.txt (only OSTicksToCalendarTime
  wall-clock arguments differ, as between any two runs).
- sdmc:/cr_bench.txt: `# bench` at startup (ulp checks, ns per call translated vs native,
  gmtx_mul as a translated scalar-float sample). Citra (x86 JIT, not representative):
  Concat 5.7 -> 3.1 us, MultVec 2.9 -> 1.4, InvXpose 4.4 -> 2.0.
- Hardware (New 3DS, interactive play, logs cr_autotest_2026-10-19_hw-run11-A-mtx-off / run12-B-mtx /
  run13-C-floatops on SD and in custom-robo-recomp-fpmtx-runs/). Bench ns per call, translated / native:
  default build: Concat 2181-2284 / 1577, MultVec 1102-1150 / 643, InvXpose 2066 / 1028, Normalize 833-871 /
  631, Dot 538-607 / 313, gmtx_mul (translated scalar) 2378-2385. FLOATOPS: Concat 1804 / 911, MultVec 984 /
  500, InvXpose 1411 / 599, Normalize 670 / 402, Dot 502 / 223, gmtx_mul 1574 (-34%).
  Game ms (average of the # time lines in the scene): battle A 37.0, B 33.9, C 32.8 (fps 19.9, 21.7, 21.9);
  campaign A 31.3, B 32.2, C 26.3 (fps 25.7, 25.6, 29.6). Each run was a different play-through (about 30 s
  of battle, campaign up to the Steel Hearts office), so campaign differences are within play noise;
  the battle gain from native MTX (-3 ms) matches the profile share.
- Decision (2026-10-19): FLOATOPS on by default (Makefile `FLOATOPS ?= 1`; `make FLOATOPS=0` for exact
  Gekko rounding). Assumption, not verified: the few-ulp differences in mul/madd results (vertex values,
  possibly collision/physics over long play) are not visible and do not change game outcomes. Citra
  replays support it (same SDK calls and scenes over 3600 retraces) but Dolphin framebuffer/RAM
  comparisons have not been done. If a replay or Dolphin comparison diverges, retest with FLOATOPS=0 first.

## Frame pacing: vblank wait (2026-10-19)

- Found in the hardware `# prof` lines of runs 11-13: lr 0x801a4728 (the GXBegin in 0x801a4558, the
  frame's first draw in battle) held 25-60% of samples. gx.c frame_begin runs at the first draw and
  called C3D_FrameBegin(C3D_FRAME_SYNCDRAW). In citro3d that is C3D_FrameSync(), which always waits
  until the next vblank on both screens, then waits for the GPU queue. So every frame idled up to
  16.7 ms. That time was counted as "game" in `# time` lines (and as "from first draw" in the split):
  every earlier game-ms figure, including hardware runs 4-13, includes it (estimated ~14 of 53.6 ms in
  run 13 battle from the lr share).
- New `# time` bucket "gx sync" = time in C3D_FrameBegin (vblank cap + GPU queue wait), not in "game".
- Pacing now: a frame waits for a vblank only when none has passed since the previous frame began
  (C3D_FrameCounter(0) unchanged), i.e. a 60 Hz cap, then C3D_FrameBegin(0) (GPU queue only).
  sdmc:/cr_vsync_on.txt = the old wait every frame. Logged once: "# gx: frame pacing ...".
- Citra replays, same build, cr_vsync_on.txt vs cap (logs in custom-robo-recomp-vsync-runs/):
  battle (replay of battle_float2, r3600) 12.8 -> 14.6 fps (78.2 -> 68.5 ms, gx sync 9.7 -> 0);
  campaign (replay of campaign_float2, r3000) 29.7 -> 43.0 fps (33.7 -> 23.2 ms, sync 10.5 -> 0);
  title (A-press pad, r1920) 29.9 -> 48.3 fps (sync 12.7 -> 0). Every other bucket is equal to
  0.1 ms. SDK/scene logs identical for battle and campaign; title differs in one line (a leftover r4
  at a VIGetRetraceCount call, wall-clock derived). Title still ends in the known OSPanic of that pad
  script (the attract movie, fixed 2026-10-21). Not yet on hardware.
- Open: the cap keys on frame begins, not on buffer swaps, so a frame that starts just before a vblank
  may be presented while the next one renders. The game runs one logic step per frame, so above 60 Hz
  is impossible and below it the game is slower than real time, as before.
- Hardware run 14 (New 3DS, interactive: title, menus, Vs. battle; SD and custom-robo-recomp-vsync-runs/
  cr_autotest_2026-10-19_hw-run14-vsync-pacing.log). Averages of `# time` lines per scene, run 13 -> 14:
  battle (4.2) 49.4 -> 47.3 ms (20.2 -> 21.1 fps), game 34.9 -> 21.8, gx sync 11.1; scene 4.5
  44.4 -> 34.6 ms (22.5 -> 28.9 fps); 4.0/4.1 ~30 fps both; scene 5.0 21.3 -> 17.5 ms (46.9 -> 57.0 fps);
  menus 7.0 28.7 -> 31.2 fps; title 2.4 45.2 -> 50.3 fps; title 2.1 at the 60 Hz cap (sync 11.5 = cap).
  Two short tear lines seen in the whole session (user report). Campaign not played.
- Finding: in battle the wait did not go away on hardware, it moved. At 21 fps the cap cannot fire, so
  the 11 ms of gx sync is C3D_FrameBegin waiting for the GPU queue (previous frame's drawing + display
  transfer). Citra does not model GPU time, which is why sync went to 0 there. Inference, not yet
  measured: battle is now bound by the GPU (~47 ms/frame) more than by the CPU (game 21.8 + gx 13 +
  audio ~1 = ~36 ms). Battle GPU load (frame 4500-5100): ~1590 draws merged into ~85 DrawElements,
  ~21300 triangles, 640x480 EFB. Next: log C3D_GetDrawingTime() per frame to confirm, then cut GPU cost
  (vertex shader length per lit vertex, overdraw/blending).
- GPU timer (2026-10-19): the `# time r<n> split:` line ends with "gpu X": the average GPU queue time per
  frame (C3D_GetDrawingTime of the previous frame, read in frame_begin after the queue wait; draws +
  display transfer). It runs in parallel with the CPU, so it is not part of the frame sum. gpu close to
  the frame time with gx sync > 0 = GPU-bound. Built, not yet run (Citra does not model GPU time).

## Mid-frame GPU submit (2026-10-21)

- Why the 2026-10-19 C3D_FrameSplit attempt (hw run 19) overlapped nothing (citro3d 1.7.1 /
  libctru 2.7.0, the versions in mkw3ds-build:2026-10-09): C3D_FrameBegin -> C3Di_WaitAndClearQueue
  does gxCmdQueueWait, gxCmdQueueStop, gxCmdQueueClear, so the GX queue is stopped during a frame.
  C3D_FrameSplit -> GX_ProcessCommandList -> gxCmdQueueAdd only submits when the queue is the running
  one, so mid-frame chunks waited for gxCmdQueueRun in C3D_FrameEnd. The splits only added GPU work
  (FRAMEBUFFER_FLUSH/INVALIDATE + EARLYDEPTH_CLEAR each).
- Now (runtime/gx.c gpu_kick, from cmdbuf_room before a draw): when the command buffer offset since
  the last split reaches KICK_WORDS (2048 words; battle/campaign frames are ~5-6k words, so ~2-3 submits), the pending draw is issued (pend_issue, so batching
  stats are unchanged), the vbuf/ibuf ranges written since the last submit and the vc_arena dirty
  range are flushed (watermarks vb_fl/ib_fl; frame_end flushes only the rest), then
  C3D_FrameSplit(GX_CMDLIST_FLUSH) and gxCmdQueueRun on citro3d's queue (every time: the queue may
  have drained and stopped). At most KICK_MAX (12) per frame; each takes one of the queue's 32
  entries (C3D_Init); the clear, the last list and the display transfer take ~3 more. Cap hits are
  logged once per frame and counted in the `# gx frame` line ("mid-frame submits N (cap skips N)").
  Textures need no extra flush: uploaded and flushed at tex_get, and an entry used this frame is never
  overwritten (reuse needs last < frames). The vertex cache arena is only reset at frame begin.
- Queue pointer assumption: libctru has no getter. citro3d source/base.c has the non-static global
  `C3D_Context __C3D_Context`, and its first member is `gxCmdQueue_s gxQueue` (source/internal.h),
  so `extern gxCmdQueue_s __C3D_Context` is the queue. Checked in the image: libcitro3d.a defines
  `B __C3D_Context`, and citro3d's own C3D_SyncTextureCopy passes `&__C3D_Context` (offset 0) to
  gxCmdQueueRun. Recheck if citro3d is updated. C3D_FrameSplit is void in 1.7.1.
- Mode: on by default; sdmc:/cr_gpukick_off.txt starts with it off (exactly the old single
  submit and flushes). ZL (not a game button) toggles it from the next frame and logs
  `# gx r<n>: mid-frame GPU submit on/off (ZL)`. Each `# time r<n> split:` line ends with
  `gpukick on|off|mixed` (mixed: a toggle inside that 60-retrace window, drop it from averages).
- Timers: `gpu` in the split line is now the tail only (C3D_FrameEnd -> queue done, since
  C3D_GetDrawingTime starts in C3D_FrameEnd). `gpu span` = first mid-frame submit (else
  C3D_FrameEnd) -> queue done, including any idle gaps when the GPU drained before the next chunk.
  The A/B compares frame and gx sync (time in C3D_FrameBegin).
- Expected (model from hw runs 17a-19a, not measured): battle 39.7 -> ~29 ms, campaign 42.1 -> ~28 ms.
  Part of gx sync may be the 60 Hz cap rather than GPU wait (title: sync ~12 with gpu ~4).
- Citra checks (logs/2026-10-21_gpukick, replays of rec-to-battle and pause-ok to ~r2250, flag on vs
  off): SDK call, pad and scene logs identical apart from the OSTicksToCalendarTime wall-clock value;
  framebuffer dumps of frames 1500/1980/2100 byte-identical; 1-4 mid-frame submits in draw-heavy
  frames, no cap hits. 6144 words gave at most 1 submit per frame (too late to overlap), hence 2048.
  Citra ignores CPU caches and GPU time, so a missing flush (garbage geometry) and the gain show
  only on hardware. ZL toggle not exercised in Citra.
- Hardware (New 3DS, logs/ cr_autotest_2026-10-21_hw-run20-gpukick (battle) and run21-gpukick-campaign,
  one build, ZL toggled mid-run; per-scene averages of # time lines, mixed windows dropped):
  battle (scene 4) on 26.7 ms = 37.5 fps (game 16.5, sync 1.0, gpu span 21.8; 46 windows) vs off
  33.6 ms = 29.8 fps (sync 9.1; 10 windows); run 19a was 35.1 ms. Campaign (scene 8) on 33.6 ms =
  29.8 fps (game 20.4, sync 2.0, span 25.3; 22 windows) vs off 50.0 ms (sync 20.1; only 3 windows),
  run 17a 39.8 ms. Title and scene 5 unchanged (60 Hz cap). gx sync is now ~1-2 ms: battle and
  campaign are CPU-bound. No garbage geometry reported. Campaign hits the 12-submit cap in bursts
  (frames ~1968-2315, safe: the rest of the frame goes in C3D_FrameEnd); a larger KICK_WORDS or cap
  is untested. Boot screen now shows build/flags (tools/buildinfo.sh, runtime/main.c).

## DVD reader thread (2026-10-21)

- Symptom (hw runs 15-21): single-frame freezes of 1-4 s, each one a single SD access. Run 21:
  cubog39.hps off 200c0 len 20 2844 ms (r736), cubog53.hps off 40100 len 20 3509 ms (r1227) and
  off 14f420 len 20 1914 ms (r2442), snr_map.BIN off 0 len 800 3529 ms (r44); run 20:
  v_cubog09.hps off 1d0420 len 20 3273 ms (r4130). The .hps reads come from the AX callback, so
  the `# stall` lines put them in "audio".
- Not the open or the seek: the stalled .hps files were first read 70-2000 frames earlier and
  streamed since (handles open in the 8-file LRU). Runs 17b and 19b stalled 4.0-4.1 s at r13 in
  "gx state", which is a failed fopen of a flag file (gx.c cr_batch_off.txt etc.), with no read
  at all. libctru 2.7.0 archive_dev.o: archive_seek calls FSFILE_GetSize only for SEEK_END
  (SEEK_SET is arithmetic), archive_read is one FSFILE_Read with the offset. So FSUSER_OpenFile/
  FSFILE_Read directly would send the same request. The SD/FS itself is busy for seconds.
- Likely cause (not yet confirmed): our own SD writes. Hw run 6 measured single log
  fwrite+fflush calls of up to 4.1 s; ax.c notes the WAV dump's small writes gave multi-second
  stalls. The log writer still appends + fflushes every ~100 ms from its thread, and a read
  queued behind a stalled write waits with it. Check: `# log write slow r<n>: <bytes> at <KB>
  took <ms>` (writes > 250 ms) and the log-writer part of each `# DVD slow` line.
- Fix (runtime/dvd.c): reads run on a reader thread (aux core, priority above the game), FIFO.
  The game thread still waits for each read at the next wait point, as before, but at most
  50 ms + len / 2.5 MB/s (about a GameCube drive) from the read's start; a read still running then
  completes in the background, its block stays BUSY (DVDGetDriveStatus says BUSY too) and its
  callback runs at a later wait point. Guest RAM is written by the reader directly (like DMA) and
  stamped at issue and again at delivery (write tracking). DVDCancel waits for the read to land,
  then drops the callback (the game never calls DVDCancel, DVDReadPrio or
  DVDGetCommandBlockStatus; all reads are DVDReadAsyncPrio + DVDGetDriveStatus).
- Logs: `# DVD slow` keeps its old text and appends `open X, seek Y, read Z; log writer: busy N ms
  at start, M writes done during, busy|idle at end`. `# DVD late r<issue>: ... game waited N ms,
  went on at r<a>, callback at r<b>` = a stall that no longer froze the game. `dvd wait` (time the
  game thread waited for the reader) is in the `# time` parenthetical and the `# stall` line.
- Flags: sdmc:/cr_dvdasync_off.txt = the game waits for each read at issue without limit (old
  timing). sdmc:/cr_dvd_stalltest.txt = every 64th .hps read sleeps 3 s on the reader (tests the
  late path).
- Citra (replays of hw run 21 to r2900 and hw run 20 to r4400, Citra save restored before each):
  SDK call, pad, scene and summary-count lines identical to the HEAD build apart from the
  OSTicksToCalendarTime wall-clock value; `# DVD` lines identical (Citra's SD delay model, 0.6 ms +
  183 ns/byte, is inside the budget: font.out 0x184480 takes 292 ms of a 686 ms budget); frame/game
  ms unchanged. Stall test on run 21: cubog49.hps 0x4000 read 3004 ms, game waited 57 ms, went on
  at r1279, callback at r1391; worst frame in that window 83 ms, no `# stall`; scenes as without
  the stall; the WAV keeps its level through the stall (the voice replays its ARAM buffer) and
  fades where the no-stall run fades. Not yet on hardware.
- Still on the game thread: lazy flag fopens in gx.c (the r13 stall), card.c saves, the WAV dump.

## Attract movie (THP) (2026-10-21)

- Symptom: idle on the title for ~37 s (scene 2 sub 4 counts down from ~0x7ff to 0, then sub 5)
  and the game enters SEQ_MOVIE (scene 1), opens thpdemo/opening_a.thp and ended in
  `OSPanic: seq_movie.c:184: Fail to decode video data` (retrace ~2520 idle, ~2250 with the
  title A-press script). THPVideoDecode (0x801aeb40) was an empty SDK stub.
- THP is a CPU-only library, so it is now translated from main.dol (recomp.py
  TRANSLATE_SDK_RANGES 0x801aeb40-0x801b6008 and 0x801b60a8-0x801b65d8, 22 functions). Native
  pieces it needs: THPInit (runtime/thp.c) points the decoders' locked-cache buffers at LC_BASE
  0x81E00000 (RAM above 24 MB) instead of 0xE0000000, which the RAM mask would fold onto
  0x80000000 (boot info); PPCMfhid2 reports HID2[LCE] after LCEnable; LCStoreData/LCStoreBlocks
  copy at once; DCZeroRange zeroes (it was a no-op stub, the decoder's work area needs it).
- The player then stalled after 3 frames: seq_movie mixes the movie audio from its own AI DMA
  callback (0x80017668, put in front of AX's __AXOutAiCallback 0x80145a00) and waits for it to
  consume each audio buffer. AI DMA is now modelled (runtime/ai.c): AIRegisterDMACallback swaps
  (starting value = AX's callback, as AXInit would register), AIInitDMA/AIGetDMAStartAddr keep the
  buffer. While another callback is registered, every 5 ms AX frame (ax.c) runs it; the native
  __AXOutAiCallback writes the AX mix as the next DMA buffer (big-endian R, L pairs, Dolphin's HLE
  AX order) and ax.c plays the buffer the chain leaves. When the movie restores AX's callback the
  old direct path is back. 0x80142088 is AIGetDMAStartAddr (was listed as AIStopDMA).
- OSGetSoundMode was a stub returning a stale r3 (0x4000), so the game chose mono at boot and the
  movie mixer averaged L and R. Now native, SRAM sound bit = stereo as in Dolphin's default SRAM.
  This affects all game audio: in the rec-to-battle replay the only SDK difference is the sound
  driver turning ITD on (AXSetVoiceItdOn/ItdTarget 0, 26 each; the mixer ignores ITD).
- Citra (private Citra copy, 20000-retrace idle run, no input): title -> movie (232 VI lines of
  scene 1 = ~6960 retraces, 3437 of the file's 3446 frames decoded) -> title -> movie -> title,
  no crash. Both cycles play opening_a.thp (opening_b.thp never opened; Dolphin not checked).
  Audio: smooth (lag-1 correlation ~0.94), L and R differ. rec-to-battle replay: same scenes and
  SDK call counts as the base build apart from the ITD calls above. Title A-press script no longer
  panics. Start pressed mid-movie (r4000): fade out, title again by r4080.
- Colours: the movie's YUV -> RGB is the THP SDK's 4-stage TEV program with signed
  intermediates (GXSetTevColorS10 REG1 = -90, 0, -114, 135; K0 0,0,226/88, K1 179,0,0/182, K2
  255,0,255 picks R/B from the colour side and G from the alpha side). setup_tev clamped REG1 to 0
  and the picture came out magenta. gx.c tev_yuv() now recognises that program (stage structure
  checked exactly, coefficients read from the registers, gx.tev_s10 keeps the signed values) and
  draws it as 6 PICA stages at half scale: 0.5*Y + positive offsets, + |c|/2 * X (or (1-X) for
  negative c), then minus the negative offsets with scale 2. Logged once as "TEV YUV -> RGB program
  (THP)". Check: GX frame 3000 = movie frame 457 (luma correlation 0.9996); against a PIL decode of
  that JPEG (full-range BT.601) the mean abs difference is 2.2 / 1.9 / 2.8 per R/G/B. Battle
  replay (rec-to-battle): SDK/pad/scene log identical, framebuffer dumps r1500/1980/2100
  byte-identical to the build without tev_yuv; one draw batch at r2100 splits differently (the
  signed registers are now part of the TEV change check).
- Not timed on hardware: 640x480 JPEG decode at 30 fps on the ARM11 is the likely cost.

## Hardware run 23 (2026-10-21, main a9cb4f7)

- Build main@a9cb4f7: mid-frame GPU submit on, DVD reader thread, THP movie. Interactive: boot,
  title, idle into the movie (scene 1, r2970-r3690, Start pressed), menus, Vs. battle, menus. Log:
  logs/cr_autotest_2026-10-21_hw-run23-dvd-thread-movie.log (same name on the SD card). Also kept:
  hw-run22-gpukick-long (13:00 gpukick build, ~24000 retraces, title/campaign/battle, ZL toggles).
- User report: no freezes; music sometimes repeats a 1-2 s segment 2-3 times; the movie plays and
  returns to the menu without a crash but is extremely laggy; audio is stereo.
- The repeats are the reader's late path. 9 reads finished late (`# DVD late`), 8 of them after
  4.6-5.7 s; each time the game went on after 33-57 ms. The .hps ones (cubog39.hps r1173, r1747,
  r3886; v_cubog09.hps r5170) are music stream refills: while the read is out, the voice loops its
  old ARAM buffer (as the Citra stall test showed), so a ~5.7 s gap over a ~2 s buffer replays it
  2-3 times. Before the reader thread the same reads froze the game instead.
- SD stalls: 9 `# log write slow` lines, each 5.6-6.0 s (median 5.8 s), the first one at r0 (105
  bytes at 0 KB). Every DVD read over 1 s (8 of 8) started while the log writer was inside one of
  these writes; reads with the writer idle took at most 160 ms. So the SD is unavailable for ~5.8 s
  at a time and a log write is in every such stall. Not yet shown whether the writes cause it
  (fflush, FAT/directory update) or only wait in it like the reads. Next check: one run with the
  log kept in RAM (no SD writes until exit); if the `# DVD slow` lines over 1 s go away, the log
  writer is the cause.
- Movie (scene 1): 9 fps while playing (r3480-r3660: frame 111 ms = game 98 + gx tex 12). Game is
  the translated THP decoder (one 640x480 JPEG per frame); gx tex is the Y/U/V upload each frame.
  Before that, r3060-r3420 ran at ~120 fps with game 0.2 ms: the player waited ~4 s for the late
  opening_a.thp read (r2981, 5.7 s), and with no GX draws the 60 Hz cap (in frame_begin) does not
  apply. ndsp dropped 480 + 286 AX frames in that span: AX frames are made per retrace (10/3 per
  retrace, ax.c), so above 60 Hz the output overruns and at 9 fps it runs short.
- Battle (scene 4.2, 32 windows): 28.6 ms = 34.9 fps (game 19.5, gx vertex 5.3, tex 1.0, state
  1.6, sync 0.2, audio 0.8). CPU-bound, as in runs 20-22.
- Boot: snr_map.BIN open took 5168 ms (log writer busy); the logo scene waited ~300 retraces for it.

## GX thread (2026-10-21)

Why: battle and campaign are CPU-bound and the GX host work is the one concentrated cost (hw run 23
battle: 7.9 of 28.6 ms in gx vertex/tex/state; the sampler puts 28-31% of samples on the return from
GXCallDisplayList), while core 2 only runs the helper threads (log writer, sampler, watchdog, DVD
reader). Design:

- Record and replay (runtime/gx.c "GX thread"). Each GX call becomes a record in a 1 MB ring (`gq`,
  single producer = game thread, single consumer = GX thread on core 2): the handler's address plus
  its inputs. The GX thread runs the same handler bodies, in the same order, on its own `gx` state;
  everything after them (setup_draw_state, uniforms, vertex/display-list/texture caches, citro3d) is
  unchanged and runs only there. Records: register arguments (r3..r10) for plain setters; for the
  calls that read guest memory at call time, a copy of what they read (GXLoad*MtxImm matrices,
  GXSetProjection(v) and GXSetViewport results, GXSetTevColor/S10/KColor, GXSetChanMat/AmbColor and
  GXSetCopyClear colours, GXLoadLightObjImm, GXLoadTexObj (the decoded TexMap), GXLoadTlut);
  GXCallDisplayList as (list, size, r1); write-gather pipe bytes as one record per run of complete
  commands (the FIFO is assembled on the game thread, which keeps the vertex-format state, desc/vat,
  to know where a primitive ends); GXCopyDisp as (clear flag).
- Stays on the game thread: everything that writes guest memory or returns a value (GXInit*,
  TPLGet*, GXGetProjectionv/GXGetViewportv from a shadow of projection/viewport, the region and
  draw-done callbacks), LOG() counts, the FIFO assembly.
- What the GX thread still reads later from guest memory: display-list bytes, vertex arrays,
  textures and TLUT data. The GameCube GPU also reads these asynchronously from the FIFO, so the game
  already keeps them intact until it syncs; the sync point is GXDrawDone/GXWaitDrawDone (called
  every frame, right after GXCopyDisp), which now waits until the GX thread has processed every
  record before it (then runs the draw-done callback as before). VIWaitForRetrace also waits before
  aptMainLoop, so citro3d's APT hooks never run while the GX thread uses citro3d. GXCopyDisp waits
  while more than one frame is queued (only matters if a scene never calls GXDrawDone).
- Pacing is unchanged: the 60 Hz cap and the GPU queue wait stay in frame_begin, now on the GX thread;
  the game thread meets them through the GXDrawDone wait.
- Write-tracking epochs (`recomp_wr_epoch`) are advanced by the game thread at GXCopyDisp (after a
  barrier), and every cache entry takes its epoch before it hashes (was after; same value when single
  threaded). A write stamped below an entry's epoch therefore happened before its hash.
  range_clean uses the r1 recorded with the draw instead of reading R[1].
- Wake-ups: records are published (barrier + index store) at draws, frame ends and waits, not per
  setter. The GX thread spins ~1 ms for more work before sleeping on a LightEvent, and the game
  thread only signals it when it announced sleeping (Dekker-style flags with barriers; both waits
  also time out, as a guard). The GX thread runs at the game's priority + 2, so the helper threads on
  core 2 (all higher) preempt its spin.
- libctru's linear allocator has no lock and both threads now allocate (ax.c ndsp buffers on the game
  thread, textures on the GX thread): linearAlloc/linearMemAlign/linearFree are wrapped (`--wrap`)
  with a LightLock.
- Only with core 2 (New 3DS); otherwise records run inline on the game thread at once (the old
  behaviour, same code). With core 2 the thread always exists and a bottom-screen tap (runtime/pad.c,
  KEY_TOUCH; ZR stays the Z button) switches between it and inline: applied in VIWaitForRetrace right
  after the sync, so every record so far has been handled and the other thread carries on with the
  same gx and citro3d state. sdmc:/cr_gxthread_off.txt = start inline. Each switch is logged
  (`# gx r<n>: GX thread on|off (bottom-screen tap)`, or the flag's name) and the `# time .. split:` line ends with
  `gx thread on|off|mixed` (mixed: a switch inside that 60-retrace window, drop it from averages).
  sdmc:/cr_gxthread_fliptest.txt = a switch every 300 retraces (at r150, 450, ..), to test the switch.
- Bottom screen: rows 0-28 scroll as before; row 29 is a fixed status line (main.c recomp_status,
  a second PrintConsole window) with both A/B toggles as they are now, e.g. `ZL gpukick ON   tap gx
  thread ON`, redrawn by the game thread when one changes. The ZL toggle's bottom-screen printf (it
  ran in frame_begin, now on the GX thread) is gone, so only the game thread writes to the console.
- Flag files are now all read once at start (recomp_gx_start), not at first use (a lazy fopen
  stalled 4 s at r13 in hw runs 17b/19b).
- Timing: in GX-thread mode the gx vertex/tex/state/end/sync buckets are GX-thread time (parallel);
  the `# time` line becomes `game X, gx wait W, audio A (...); gx thread busy B: vertex .., tex ..,
  state .., end/wait .., sync ..` with game = frame - gx wait - audio. `gx wait` = game thread
  blocked on the GX thread (draw-done sync, frame limit, ring full).
- Expected (not measured): battle 28.6 -> ~21 ms (the GX work overlaps the game code; the game still
  pays LOG, the setter calls and recording).
- Citra (logs/2026-10-21_gxthread; replays of rec-to-battle, hw runs 20, 21 and 23, cr_fbdump.txt;
  this build with the GX thread, the same with cr_gxthread_off.txt, and main@a9cb4f7): SDK call (r3-r6,
  lr), pad, scene and summary-count lines identical in all three (summary rows compared as a set:
  qsort orders equal counts differently per binary; the leftover r4 of one VIGetRetraceCount is
  wall-clock, as before) and every framebuffer dump byte-identical. Only other log difference: the
  frame-0 `# gx batch` line no longer says "(batching off)" (the old lazy flag read was still 2,
  i.e. true, at frame 0). Citra runs both threads in parallel: scene 5 menus 42-44 fps with the flag
  vs 60 (cap) without it (Citra time is instruction counts, not hardware). Coverage of those runs:
  boot, logo, title (display-list tunnel), title menu, Vs. menus only: the Citra memory card folder
  (sdmc:/custom-robo/save) was broken by the test script's save restore (a file instead of the
  folder), so the title waited on the card and no replay reached battle or campaign.
- Citra, no memory card (cr_card_off.txt, as the 2026-10-18 recordings were made; logs `*-nocard-*`):
  rec-to-battle reaches battle (scene 4.0 r1920, 4.2 r2400) and pause-ok the campaign (8.3 from
  r1470). GX thread on, cr_gxthread_off.txt, cr_gxthread_fliptest.txt (8 switches per run) and
  main@a9cb4f7: SDK/pad/scene/summary lines identical and framebuffer dumps r1500/1980/2100
  byte-identical, battle and campaign.
- Hardware run 24 (2026-10-21, build 76feb9e, logs/cr_autotest_2026-10-21_hw-run24-gx-thread.log, same
  name on the SD card; campaign r1320-7140, Vs. battle r8550-10650, GX thread switched by tap 12 times,
  gpukick toggled 4 times; averages of `# time` windows, mixed windows dropped):
  - Vs. battle (scene 4.2, GX thread on, 28 windows): 20.9 ms = 47.9 fps, game speed 80% (run 23,
    previous build: 28.6 ms = 35.0 fps, 58%). Game 19.1, gx wait 0.9, audio 0.9; GX thread busy 12.9
    (sync 3.2 of it), gpu tail 4.2, span 18.8. Same load as run 23 (~2200 draws, 29-31k tris, ~250 DL
    calls per frame). Side by side in the same battle: on 21.5-23.7 ms (r9300, 9360, 9600, 9780,
    10080) vs off 31.1-31.5 ms (r9480, r9900), i.e. 32 -> 44 fps there.
  - Campaign (scene 8.3, on, 81 windows): 23.6 ms = 42.4 fps, 71% (runs 21/22: 34-36 ms, a different
    play-through). GPU-bound now: next to the toggles at r2825-3374, on 22.8-24.4 ms vs off 24.1-24.9 ms;
    off, the game code is ~6 ms and gx sync ~15.5 ms (waiting for the GPU queue), gpu 16-21 ms per frame.
    The campaign gain needs GPU work (vertex shader length, vertex size, texture formats, mipmaps;
    quarter-resolution run 17b split campaign GPU into ~8 ms vertex + ~8.5 ms fill).
  - Menus: scene 5.0 58 fps, 6.0 52, 7.0 49, pause (c.0) 40; title 57-60 (cap).
  - No crash (the run ended through HOME, `# DONE APT exit`); every switch applied both ways in
    battle and campaign. User: results felt promising. SD stalls unchanged (log writes of 5.7-6.1 s,
    6 late DVD reads, music refills among them).

## Bottom-screen stats (2026-10-21)

- The bottom screen's last 8 rows are fixed status rows (`recomp_status_row`, main.c; the scrolling
  console is rows 0-21): 7 rows of stats, then the toggle line. Drawn by `stats_draw` (log.c) from
  `recomp_prof_frame` at every 60-retrace `# time` window, on the game thread; the GX thread never
  prints. A row is redrawn only when its text changed; 39 characters per row (a 40th wraps).
- Rows: fps, speed (= fps / 60) and worst frame; frame / game / audio ms; with the GX thread
  `gxthr wait / busy / sync` and `vtx / tex / state / gpu` (busy parts), inline `vtx / tex / state`
  and `end / sync / gpu`; scene/sub (0x80321d88 / 0x80321d98) and retrace; draws and triangles per
  frame (window average) and linear heap free; dvd wait ms, `# DVD late` / `# DVD slow` / `# stall`
  counts since boot; toggle line. Same numbers and formulas as the `# time` lines.
- Draws/tris: `frame_end` (gx.c, on whichever thread runs GX) adds st_draws/st_tris to running totals
  `recomp_gx_stat[]` and bumps the frame count after a barrier; the game thread diffs them per window
  (can be off by one frame). Linear free is read every 60 GX frames under lin_lock.
- Cost: `# status r<n>` lines (every 300 retraces) give the average and worst ms per update. Citra
  (-Fast, rec-to-battle): 0.8-1.4 ms per update, worst 1.5, once a second (~0.02 ms per frame on
  average, inside "game" of the next window). The console draws each glyph on the CPU. Not yet
  measured on hardware.
- Writing `PrintConsole.cursorX = 0` before a row lost its first character; setting only cursorY and
  printing "\r" first works.
- Citra replay of rec-to-battle (no card): SDK/pad/retrace lines and summary counts identical to the
  base build (only OSTicksToCalendarTime's wall-clock r4 differs, as before). Screenshot checked.

## Bottom-screen menu (2026-10-22)

**Buttons removed** (clean build 2026-10-22, user request): no touch input and no ZL toggle; the build id, flags, stats and messages layout stays.

- Replaces the scrolling console + "tap a half" toggles (`runtime/menu.c`; the "Bottom-screen stats"
  section above is the old layout). 40x30 cells of 8x8 px:
  row 0 build id; rows 1-2 sdmc flag files present at boot (cr_ dropped); rows 4-12 stats, one metric
  per row (fps+speed, frame ms+worst, game, gx wait+thread busy or gx inline+sync, vtx/tex/state parts,
  gpu+draws/tris, audio+dvd wait, scene/sub/retrace, io counts + linear free), from log.c every
  60-retrace `# time` window; rows 15-17 printf messages (libctru draws a console window one row above
  its y, so the window is set at y 15 to land on 14-16); rows 18-29 a 2x4 grid of buttons, 19x2 cells
  each: name, then ON (black on green) / OFF (white on red) / -- (unavailable or not bound).
- Touch: PADRead hands a new touch (KEY_TOUCH down only) to `recomp_menu_touch`; it acts only inside
  a button's cells, never in the gaps. ZL still toggles gpukick.
- Adding a button: one line in `btn[]` (menu.c). Either get/toggle functions (the toggle is a request
  the owner applies at a safe point: GX thread at recomp_gx_retrace, audio in recomp_ax_service,
  gpukick in frame_begin; their own `# gx`/`# AX` log lines and # time "mixed" fields as before), or a
  flag variable: the owner calls `recomp_menu_bind("cull", &cull_off, NULL)` once its flag file was
  read (e.g. in recomp_gx_start). The menu flips the int at recomp_menu_retrace, which recomp_gx_retrace
  calls after the GX-thread sync (no GX record in flight), logs `# menu r<n>: <name> on|off (touch)`,
  then calls the optional `changed` callback (e.g. to drop a texture cache). `inv` = 1 in the table for
  `_off` flags (button ON while the int is 0). Placeholders already in btn[]: cull (cr_cull_off),
  mipmaps (cr_mip_off), tex formats (cr_texfmt_off), vsh single (cr_vsh_single); main.c's flags[] lists
  those four flag files too. tools/time_avg.py drops a window with a `# menu` line in it.
- Cost (Citra -Fast, rec-to-battle): `# status` 0.8-1.25 ms per stats update (worst 1.31), once a
  second = ~0.02 ms per frame; buttons redraw only on a state change (a compare per button per
  retrace). Replay vs main 9719c55: SDK/pad/scene lines, # time line count and fb dumps
  (r2100/2700/3300) identical. Not yet on hardware.

## Codegen per guest instruction (2026-10-21)

Battle is CPU-bound on the game thread with a flat profile, so these change what every translated
function compiles to (recomp.py and ppc.h only; game semantics unchanged):

- ppc.h memory helpers (`rd*/wr*`, `_ram`, bit casts) are `always_inline` (`RI`): in gen/'s large
  functions GCC had stopped inlining them (37.5k out-of-line calls in 886 functions, e.g. rd16/wr16/wr8
  on f_80090710's flag bytes). Alone this grew gen/ from 1.52 M to 2.01 M instructions; the items below
  bring it back to 1.58 M.
- psq_l/psq_st with I = 0 (MWCC's f14-f31 saves/restores, PSMTX-style code; 4.7k sites) emit
  `psq_ld0/psq_st0`: float type without loading GQR0 or switching on its type; r1-relative ones skip
  IS_HW and WR_MARK like other stack accesses (they went through wr32 before). Assumes GQR0 = 0, as
  the native MTX functions already did. Guards: recomp.py records any translated mtspr GQR0 (there is
  none) and then defines RECOMP_GQR0_WRITTEN in funcs.h, which turns psq_ld0/psq_st0 back into the
  general psq_ld/psq_st; `check_gqr_writes()` fails the run if runtime/*.c writes cpu.gqr[0] other than 0
  or uses a computed GQR index outside GQR_INDEX_OK (thp.c sets GQR2-5). I != 0 sites keep psq_ld/psq_st.
- fctiw/fctiwz (558 sites, all fctiwz): `fctiw_bits` uses ARM `vcvt.s32.f64` (toward zero) /
  `vcvtr.s32.f64` (FPSCR mode, nearest like the old rint) instead of libm trunc/rint. VCVT saturates to
  0x7FFFFFFF/0x80000000 like the old clamps and gives 0 for NaN, which stays special-cased to
  0x80000000; the -0 upper-word quirk is unchanged. Non-ARM builds keep the C version.
- `so` is reloaded as `cpu.xer_so & 1u` (it is only ever 0 or 1: set_xer, mcrxr): GCC then knows
  `cmp_s(..) | so` cannot set LT/GT/EQ, and branches on those bits test the compare only (12.2k
  `tst #2/4/8` instructions -> 0.8k, -100k instructions).
- r1- and r11-relative accesses with a constant offset (r11 = r1 + N in the inlined _savegpr/_savefpr)
  are emitted as `rd32_sp(r1, k)` etc. (`stack_access()`): `ram + (r1 & RAM_MASK) + k`, so the mask is
  computed once per r1 value and k becomes the load/store offset. `rd32_ram(r1 + k)` masked every
  address, and GCC often precomputed those addresses and spilled them to the C stack. Update forms
  (stwu r1, psq_stu) and indexed ones keep `*_ram`. Assumes r1 & RAM_MASK is more than 32 KB from both
  ends of the mirror (else `ram + off` and the old wrapped mask differ); recomp_call_guest (cpu.c) logs
  `# r1 ... within 64 KB of the RAM mirror's end` once if r1 is ever there. The stack is at 0x8046xxxx.
- r2/r13 (SDA bases): `fold_base_regs()` checks that in all translated functions (Func write masks) the
  only writes are __start's lis/ori pair, giving the value cpu_init also sets, and then replaces their
  other reads by the constant (r13: 18221 reads, r2: 653). r13/r2-relative loads and stores get constant
  addresses: no IS_HW check, no base register (-180k instructions). If the check fails it prints
  `note: rN not folded` and nothing changes.
- fneg/fabs/fnabs and ps_neg/abs/nabs use `-d` / `fabs()` (`fneg_d` etc.): VNEG/VABS change only the
  sign bit (ARM: not arithmetic, no NaN processing, not flushed in RunFast mode), so they are exact for
  all inputs; the `d2u ^ sign` form went through core registers and a C-stack round trip.
- Stores to a compile-time constant WGPIPE address (0xCC008000, 705 sites in 22 functions) call
  `recomp_gx_fifo_write` directly (`HW_WRITE`), as hw_write would.
- Tried and dropped: `__attribute__((cold))` on hw_read/hw_write/recomp_poll_events (no hot/cold split
  on this target, +47k instructions); masking a store's offset once for both the store and its WR_MARK
  page through an empty asm (-17k static instructions, no measurable Citra change). For scale, removing
  WR_MARK altogether (experiment only, wrong results) gave battle game 22.2 -> 21.1 ms.
- Citra replays (`tools/citra/replay.ps1`, compare_logs.py exit 0 for every step: same SDK/pad/scene/
  summary lines, byte-identical framebuffer dumps) vs 40f52fc, game ms (time_avg.py; battle scene 4.2
  from r2460, campaign scene 8.3), f_ instructions (objdump) and .text, cumulative:

  | step | battle game | campaign game | f_ instructions | .text MB |
  |---|---|---|---|---|
  | base 40f52fc | 28.3 | 14.4 | 1,522,525 | 6.73 |
  | always_inline | 26.6 | 13.5 | 2,008,567 | 8.64 |
  | + psq GQR0 | 25.0 | 12.8 | 1,929,998 | 8.29 |
  | + fctiw vcvt | 24.9 | 12.8 | 1,921,845 | 8.25 |
  | + so & 1 | 24.3 | 12.5 | 1,822,008 | 7.85 |
  | + stack base+offset | 23.2 | 12.1 | 1,758,530 | 7.61 |
  | + r2/r13 constants | 22.5 | 11.9 | 1,580,026 | 6.90 |
  | + fneg/fabs, WGPIPE direct | 22.2 | 11.8 | 1,577,050 | 6.89 |

  Battle frame 30.7 -> 24.7 ms (GX thread busy 18.5 -> 18.7, it varies a little per build with the
  game/GX thread interplay); campaign frame 17.3 -> 17.2 ms (paced, now waiting more on GX/GPU).
  Static mix base -> final: C-stack ldr/str 12.2% -> 10.3%, vldr/vstr 1.7% -> 2.2%; out-of-line calls
  to rd*/wr*/psq_*/fctiw_bits/trunc 43.5k -> 720 (psq with GQR5/6: quant, psq_ld). The inlined IS_HW
  slow paths (36k hw_read + 18.8k hw_write calls, never taken in these replays) sit at function ends.
- Not yet on hardware: Citra time counts instructions, not I-cache misses; .text is 2% larger than base.

## Per-model cost (2026-10-21)

Question (user): the more models in a scene, the lower the frame rate. What grows per model, on
which thread, and what part of it the runtime can remove. Citra only (replay.ps1 battle /
campaign; Citra time is an instruction-count proxy, not hardware ms).

Profiling added (FNPROF builds only; normal builds and their logs are unchanged):
- `# prof` lists SDK and native functions by name: recomp_sdk_log (every LOG()), mtx.c and
  libc.c natives tag `recomp_cur_fn` with their name. 00000000 is then only the call-boundary
  glue of translated code: gen/ sets FNPROF(entry) after a function's entry reload and after
  each post-call reload, and FNPROF(0) at each spill, so those register loads/stores and
  call_indirect count as 0 (translator side). Top 60 entries instead of 25.
- `# sdk r<n>`: SDK and native calls per frame over the last 300 retraces (natives are counted
  in FNPROF builds only, and then also appear in the summary).

Per 300-retrace window (FNPROF build of the base code, logs/perf/fnprof4-*; ms = sampler share x
frame time, so the five parts add up to the frame; GX SDK = every GX/OS/PAD/VI call that LOGs):

| window | DL calls/f | draws/f | GX SDK calls/f | pos+nrm mtx loads/f | PSMTXConcat/f | game thread | game code | glue (0) | GX SDK | MTX natives | waits | GX thread busy |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| battle 4.2 r2700 | 102 | 1143 | 3974 | 130 | 51 | 25.0 | 18.6 | 5.3 | 1.5 | 0.6 | 1.9 | 13.1 |
| battle 4.2 r3300 | 247 | 2165 | 3409 | 594 | 508 | 32.5 | 24.8 | 4.8 | 1.4 | 1.8 | 1.5 | 21.1 |
| campaign 8.3 r2100 | 97 | 1257 | 1379 | 637 | 1087 | 15.7 | 9.3 | 2.2 | 1.1 | 3.0 | 2.1 | 13.0 |
| Vs. menu 7.0 r1800 | 0 | 199 | 2049 | 8 | 8 | 12.2 | 8.9 | 2.4 | 1.3 | 0.1 | 6.6 | 15.2 |
| menu 5.0 r1500 | 0 | 197 | 623 | 13 | 32 | 5.8 | 3.9 | 1.3 | 0.3 | 0.1 | 13.5 | 16.6 |

Unit costs (Citra):
- GX SDK call on the game thread (LOG + record into the ring): 0.3-0.5 us per call.
- PSMTXConcat native: 2.6 us per call (startup `# bench`; translated 7.0 us). Campaign: 1087 per
  frame, 3.4 per position-matrix load (envelope skinning), 2.5-2.8 ms. InvXpose 1.7 us x 315.
- GX thread: busy / DL calls = 76-130 us per display-list call in battle and campaign (all of the
  thread: draw setup, uniforms, plan, signature, validation, index copy).
- Battle r2700 -> r3300 (+145 DL calls, +464 matrix loads, +457 concats per frame): game thread
  +7.5 ms = ~52 us per added DL call (game code +6.2, MTX natives +1.2, GX SDK and glue flat;
  collision code grows too, so the play differs as well: an upper bound per model), GX thread
  +8.0 ms = ~55 us per added DL call.

What grows with models, largest first (battle r3300 / campaign 8.3, ms per frame):
1. Translated game code, 24.8 / 9.3: spread over many functions (none above 1.8 ms).
   Battle r3300: 80060fa4 1.8, 8006eef8 1.2, motion_eval_keyframes 8001eb48 1.1, 80023394 1.0,
   8001b544 1.0, 8019fc14 0.8, 80090710 0.8, coll_test_faces 8005e270 0.75, gmtx_mul 8001e134
   0.7; campaign: 8001eb48 1.0, 801958d0 0.8, 801a0098 0.7, gmtx_mul 0.55, 8019eb5c 0.5.
   gmtx_mul (game code, scalar fmuls, a 3x4 multiply like PSMTXConcat) is 8.3 us per call
   translated, PSMTXConcat native 1.6 us. MSL sin/cos (801689d0 / 80168f38): 0.5 / 0.1 ms.
   Roles (Sonnet sweep of gen/ and the disassembly; guesses unless noted): 80023394 is a byte-wise
   zero fill (r3 dst, r4 count; checked in the disassembly), 88 callers, 1.0 / 0.45 ms; 80060fa4
   a call-free float loop (physics/collision pass); 8006eef8 an immediate-mode GX draw (GXBegin
   x6, PSMTXMultVec x8; hit volumes or shadows); 8001b544 a vec3 distance check; 8019fc14 angles
   -> rotation (sin/cos x3); 80090710 a 2D/TLUT screen draw; 801958d0 HSD matrix setup before a
   draw (PSMTXConcat x4, InvXpose, Load Pos/Nrm/TexMtx); 801a0098 12-float scaled add (skinning
   blend); 8019eb5c local matrix from SRT; 80195ee4 PObj draw (SetArray/VtxDesc, CallDisplayList);
   80189058 TObj texture bind; 801aa74c MObj/TEV setup. gmtx_mul's 9 callers and
   motion_eval_keyframes' 2 are all in 0x80052000-0x8005b000 (motion/skeleton code).
2. Call-boundary glue (00000000), 4.8 / 2.2: grows with the number of calls.
3. MTX natives, 1.8 / 3.0 (PSMTXConcat 508 / 1087 calls).
4. GX SDK call path, 1.4 / 1.1: about flat in calls between battle windows (3400-4100).
5. GX thread, 21.1 / 13.0 busy: in parallel and below the game thread here and on hardware
   (run 24: busy 12.9 vs game 19.1); it decides the frame only once the game thread is faster.
6. A cliff, not a slope: the display-list cache (2-way, DL_N 4096) keyed on host pointers (the
   rd_ function pointer and ram + offset in plan_sig, the list pointer in the slot). Which keys
   share a set changed with every build; ~250 keys per battle frame in 2048 sets put 3 in one
   set in about every other build (expected triples 0.6), and those lists then missed every
   frame, re-decoded into the arena, filled it and forced the full flush (vc_gen++) about once
   per 60-frame window: a ~107 ms frame each time. Seen in this session's MTX-only build (5 lists
   "new" every frame from r3000, worst frame 107 ms in each window from r3000, GX vertex +2.5 ms)
   but not in the base build: this was the "battle vcache arena line varies by build" item.
   More models = more keys = likelier.

Fixes ranked by ms per frame (Citra game thread unless noted; estimates marked ~):
1. Translator (not this session, ppc.h / recomp.py): call-boundary glue, up to 4.8 / 2.2 ms;
   ~half of it if the callee's entry and the post-call reload load only what is read.
2. Translator (recomp.py NATIVE + a runtime sdk_ function, like memset): the byte-wise zero fill
   80023394 as a native, ~-0.9 / -0.4 ms (one wr8 with a write mark and a back-edge poll per byte
   today). It is game code, but only a memzero; polls in its loop go away as with memset.
3. Translator: float codegen of the hot game functions (gmtx_mul 8.3 us vs 1.6 us native for the
   same arithmetic suggests ~2-4x headroom in that kind of code); spread, size unknown.
4. Runtime, done: native MTX in float with flat pointers: campaign game -1.0 ms, battle -0.3/-0.5.
5. Runtime, done: display-list / vertex cache 4-way with build-independent keys: removes the
   per-build thrash (above); battle 4.2 vs the MTX-only build: frame 30.8 -> 30.1, worst 77 ->
   42 ms.
6. Runtime: cheaper SDK call path (a per-function static counter pointer in LOG() instead of
   count_of's hash probe; record writes inline): ~-0.3 to -0.5 ms of 1.4 (battle), ~-0.3 campaign.
7. Runtime: native MSL sin/cos, only if bit-identical: ~-0.3 ms battle, ~0 campaign.
8. Runtime: vertex-cache arena eviction (LRU or compaction) instead of the full flush at 3/4: the
   remaining flush (battle r2820 window, 113 ms frame) comes from slow growth of new keys. A
   hitch every ~600-900 frames in battle, not the average.
9. GX thread: memoise make_plan/trim_plan/plan_sig/texgen keys per display-list call on a GX
   state generation counter, and draw cached index lists without the memcpy into ibuf: ~30-50%
   of the ~55 us per DL call (~-3 to -5 ms GX busy in battle r3300). No frame-time gain until the
   game thread drops below the GX thread's busy time.

Implemented (runtime/mtx.c, mtx_impl.h, gx.c):
- MTX natives: mtx_impl.h is generic in the value type MT and loads LD / LDK. With FLOATOPS
  every single op is a float op whose double result is exact, so the native values stay float
  (F_ADD..F_MSUB; float add/sub equal the double op rounded to float, 53 >= 2 * 24 + 2 bits);
  without FLOATOPS nothing changes. A flat variant fp_ runs the same bodies on addresses masked
  once, used when every range lies inside ram[] (else nat_). One write mark per destination
  (MARK: first and last word, the same pages as one WR_MARK per store). Concat is fully
  unrolled (inputs stay in VFP registers). `# bench` per call: PSMTXConcat 2607 -> 1563 ns,
  MultVec 1280 -> 1052, InvXpose 1716 -> 1425; 0 ulp native vs translated for all 18.
- Caches: plan_sig hashes the array's guest offset and not rd (it follows from type and n); the
  DL and vertex cache slots use the list/data guest offset; both caches are CACHE_WAYS = 4-way
  (LRU way replaced). The arena lines are now the same in a normal and an FNPROF build
  (2100: 3887 KB in both; before: 3592 vs 2141 KB).
- Before -> after (logs/perf base-* / new-*, time_avg, Citra): battle 4.2 frame 30.5 -> 30.1 ms,
  game 28.2 -> 27.7, worst 48.2 -> 42.2, GX busy 18.3 -> 18.5; 4.0 frame 27.3 -> 26.8; 4.1 24.3
  -> 24.2. Campaign 8.3 game 14.4 -> 13.4 ms, frame 17.3 -> 17.2 (at the 60 Hz cap: gx wait
  1.7 -> 2.7). Menus unchanged. compare_logs.py exit 0 for both (SDK, pad, scene and summary
  lines; framebuffer dumps byte-identical). Not yet on hardware.

## GX thread per display-list cost (2026-10-22)

Why: after the translator work the GX thread was close to being the battle bottleneck (Citra battle
4.2: game thread 21.8 ms, GX thread busy 18.7; campaign 8.3: the game thread waited 5.2 ms per
frame for it). Almost all of its time is in dl_draw (247 GXCallDisplayList hits per battle frame).
Citra only (tools/citra/replay.ps1 battle / campaign; Citra time is an instruction-count proxy).

Where a hit's ~75 us went (temporary per-step tick marks in dl_draw, battle r3000-3300, ms per
frame; each mark adds 0.56 us): texture/TEV/pixel setup 4.6 (TEV rebuilds 0.8 for 23 per frame,
unchanged-TEV checks 1.0 for 225), index memcpy + emit 4.9, matrix uniforms 2.4, texgen 1.9, plan
(make_plan 1.5, trim/finish 1.0, plan_sig 1.0), lighting 0.7, clip 0.5, lookup/validation 0.7,
frame_begin/cmdbuf_room 0.7. C3D_DrawElements: 2.7 ms for 133 calls (~20 us each), spread over
the buckets that call pend_flush first (mostly the uniform setters); that is citro3d's own cost.

Changes, each measured alone (GX busy, ms per frame, time_avg windows; battle --from 2460,
campaign --from 1500):

| step | battle 4.2 busy | battle vertex / tex / state | campaign 8.3 vertex | campaign busy - sync |
|---|---|---|---|---|
| base (14d19e7) | 18.7 | 10.6 / 1.1 / 2.6 | 7.8 | 11.4 |
| 1. fewer tick reads | 17.9 | 10.6 / 0.7 / 3.4 | 7.8 | 11.1 |
| 2. texture cache hash chains | 17.4 | 10.6 / 0.2 / 3.4 | 7.8 | 11.1 |
| 3. index lists in place, rings with eviction | 13.5 | 6.7 / 0.2 / 3.4 | 3.9 | 7.0 |
| 4. plan memo, texgen key | 12.6 | 5.9 / 0.2 / 3.4 | 3.4 | 6.5 |

1. Timing: svcGetSystemTick is an SVC. Citra adds 150 ticks to every call (its kernel, as
   recalled from its source: "measured on an o3DS", i.e. ~0.56 us at 268 MHz); on hardware an SVC
   round trip is a few hundred cycles as well (not measured here). A display-list hit read it
   10-12 times (dl_draw 4, 2 per texture unit, 2 for state, 2 in emit_ix), ~2700 reads per battle
   frame. Now a chain of marks (t_start / t_mark / t_mark2: each read closes the interval since
   the previous one): 4 per hit, 6 per per-primitive draw. Bucket meanings changed slightly: gx tex
   is only the texture cache's slow path (timed alone and taken out of the chain), a hit through
   the per-frame index counts as state; emit_ix is the caller's vertex part (was state);
   dl_draw's make_plan now runs after the uniform setters (set_texgen does not read the plan
   there: per-vertex texture matrices take the per-primitive path).
2. tex_get: a texture's first use per frame scanned all 192 entries twice. Used entries are now
   chained by (image, size, format) in tcache index order (the same candidates in the same order
   as the scans); the LRU victim scan only runs for a new texture.
3. Index lists in place and rings:
   - Cached index lists (display-list and vertex cache) live in vc_ixa, a linear-memory ring of
     0x30000 indices, and are drawn from there (was: a memcpy into ibuf per hit, ~170 KB per battle
     frame). Lists of one frame are allocated in draw order, so consecutive hits stay adjacent
     and still merge into one C3D_DrawElements. ibuf (immediate draws, fallbacks) 0x48000 ->
     0x30000 indices per half, so linear memory use is unchanged; the static vc_ix is gone.
   - vc_arena and vc_ixa are rings (ring_alloc). Each allocation has a record; live records are
     queued in arena order from the ring's pointer, so the next allocation in the way is the
     queue front. Allocations in the way are evicted (owner entry dropped: it misses and decodes
     again) unless drawn in this GX frame (the GPU may not have read them; the previous frame's
     are done, C3D_FrameBegin waits for its queue). Second chance: allocations drawn in the last
     3 frames are passed over until a lap found no room. A plain ring evicted entries drawn every
     frame; they decoded again the next frame and evicted the next ones (a 76 ms frame at r2952
     in the test below, gone with second chance). An entry that gets a new allocation leaves the
     old one unowned (reused once the pointer passes it). Replaces the whole-arena flush at 3/4
     full. Flushes: each ring flushes its written range at kicks and frame end, and at once when
     a new allocation is not contiguous with it.
   - sdmc:/cr_vcache_small.txt: rings at 5/8, so that replays evict (battle 59 + 3 evictions,
     campaign 2 + 23). In a test build at 3/8 the battle working set no longer fits: 25k+ failed
     allocations, every frame falls back per primitive / to vbuf (~80 ms frames); logs and
     framebuffers still identical. Normal size, end of the battle replay: arena 4657 KB of 5440,
     index ring 317 KB of 384, no evictions yet.
   - "# gx vcache" line: live arena and index ring KB, evictions and failed allocations since
     start.
4. Plan memo and texgen key:
   - plan_get: dl_draw's trimmed, finished plan, template vertex and signature from a 256-entry
     memo keyed on everything they read (vertex format of the present attributes, base/stride of
     the indexed arrays, current matrix, material sources, texgen sources), the whole key
     compared on a hit. A current matrix that is not a PNMTXn is not memoised (plan_finish logs
     it). Battle: 245 of 247 calls per frame hit ("# gx dl" line: plan memo hits / misses).
   - set_texgen's key held xf_gen, bumped by every matrix load (~2.4 per call in battle), so it
     recomputed nearly every call; it now holds the write generation of the 3 XF rows each used
     texgen reads (xf_row_gen, set by x_load_mtx).
5. Tried and dropped: a TEV generation counter (every TEV/channel setter compares before it
   writes, tev_changed skips its compare while the generation is unchanged) plus a light reload
   compare: state 3.4 -> 3.0 ms but busy 12.6 -> 12.8 (the compares moved into the setters).

Results (Citra): battle 4.2 GX busy 18.7 -> 12.6 ms (-33%); the frame stays 24.1-24.2 ms (game
thread 21.8, gx wait 0.7 -> 0.6). Campaign 8.3 GX work (busy - sync) 11.4 -> 6.5 ms, frame
17.2 -> 17.0 (60 Hz cap; the GX thread now waits on the cap: sync 4.0 -> 8.8). Battle 4.2 window
worst 36.3 -> 35.0; the r2820 window 112.8 -> 87.6 ms (the arena flush was in it). The 87 ms
frame left there (r2772) is the GX thread uploading new textures (that window's gx tex is 1.4 ms
vs 0.2) and decoding new display lists when new content appears; next if hitches matter:
texture upload cost. compare_logs.py exit 0 at every step, battle and campaign (SDK, pad, scene,
summary lines; framebuffer dumps r2100/2700/3300 and r1500/1980/2300 byte-identical), also with
cr_vcache_small. Not yet on hardware.

Remaining per hit (battle, after): uniforms 4.5 ms per frame (set_mtx uploads, and the pending
draw's C3D_DrawElements through pend_flush), state 3.4 (TEV compare, pixel state, binds), vertex
rest 2.7 (lookup, validation, emit). Next for the GX thread: fewer C3D_DrawElements (~20 us each
in citro3d) or a cheaper path to it.

## Call-boundary glue (2026-10-22)

Task: the game thread's cost at translated call boundaries ("Per-model cost" item 1, FNPROF glue
4.8 / 2.2 ms) and the zero fill 0x80023394 (item 2). recomp.py, runtime/libc.c and runtime/vi.c
only. Every step was checked with Citra replays (replay.ps1 battle / campaign) against the base
14d19e7: compare_logs.py exit 0 (SDK calls with r3-r6 and lr, pad, scenes, summary counts,
byte-identical framebuffer dumps), and no `# call_indirect` guard line (step 6) in either log.

Measuring:
- `RECOMP_SITEPROF=1 python tools/recomp/recomp.py` (then a normal make): every call site,
  return and function entry counts its executions (`SITEC(k)`), gen/sites.tsv lists each site's
  caller, callee and store/reload masks, and vi.c logs `# site <retrace> <k> <n>` (counts since
  the last dump) every 300 retraces. A normal gen/ has none of it.
- Per frame (battle 4.2 r2460 (after: r2400) to r3600, campaign 8.3 r1470 (after: r1500) to
  r2400; one register = one u32 or one PS; entry loads = registers live at entry, the others are
  dead loads GCC drops):

  | per frame | battle before | battle after | campaign before | campaign after |
  |---|---|---|---|---|
  | calls (to SDK / natives / call_indirect) | 16553 (6693) | 14899 (6856) | 7450 (3474) | 6849 (3652) |
  | translated function entries | 11140 | 9308 | 4368 | 3593 |
  | call_indirect | 1272 | 1257 | 386 | 390 |
  | registers stored before calls | 65047 | 57682 | 25565 | 23025 |
  | registers reloaded after calls | 62146 | 34843 | 27395 | 14640 |
  | registers stored at returns | 38844 | 26549 | 13915 | 8923 |
  | registers loaded at entry | 84658 | 72920 | 31507 | 26140 |

- Calibration: the old call_indirect (binary search, ~12 probes over 3975 entries) cost ~180
  instructions per call; 1.3k calls were 0.84 ms of battle game time, so Citra charges ~270
  instructions per us on the game thread (an instruction count at 268 MHz).
- FNPROF of the final code: 00000000 is 15.2% of battle r3300 (~3.6 ms of the FNPROF frame) and
  11.9% of campaign r2100 (~2.0 ms). That bucket is now mostly the C call itself (prologue and
  epilogue, the caller's live values spilled around the call, the call and return) rather than
  guest-register traffic. No FNPROF build of the base was made, so there is no before/after for it.

Steps, game-thread ms (time_avg windows, battle 4.2 from r2460, campaign 8.3), cumulative:

| step | battle game | campaign game |
|---|---|---|
| base 14d19e7 | 21.80 | 10.79 |
| 1 0x80023394 native | 21.06 | 10.36 |
| 2 so = 0 | 21.04 | 10.33 |
| 3 use/def per statement | 21.00 | 10.31 |
| 4 ctr/ca/cr0/cr5-7 kept in locals | 21.04 | 10.31 |
| 5 call_indirect hash table | 20.20 | 10.07 |
| 6 out(): return stores | 20.08 | 10.02 |
| 7 native and call_indirect in-sets | 20.06 | 9.97 |
| 8 leaf inlining | 19.86 | 9.93 |

Battle frame 24.17 -> 22.61 ms (GX thread busy 18.7, unchanged); campaign frame 17.16 (paced,
unchanged).

1. 0x80023394 (game code, 88 callers) as `sdk_mem_zero` (recomp.py NATIVE, runtime/libc.c): `stb
   0` per byte while i < n (cmplw, so n is unsigned), r3/r4 unchanged, r0 = 0 when n > 0
   (`dol.py dis 0x80023394 0x10`). NATIVE_REGS: reads r3/r4, writes r0 (NATIVE_WRITE_EXTRA lets
   libc.c write R[0]); write tracking through recomp_mark_ram. Not reproduced, as for memset: its
   16-byte frame below r1 (back chain, saved r30/r31) and the back-edge polls. Its body now shows
   in the generated "valid code not reached" list.
2. XER[SO] is never set by translated code (OE forms are not modelled and none is listed; no
   mtxer) and runtime/ never writes cpu.xer_* (check_native_writes), so `fold_so()` drops `| so`
   from 21054 compares and the `so` local disappears (its entry load and reloads: ~11k per battle
   frame). A translated mtxer (set_xer) would turn it off with a note.
3. usedef() works statement by statement: a record form (`rlwinm. r0,..` = `r0 = ..; cr0 =
   cmp_s(r0..)`) no longer reads the old r0. Liveness shrinks only where the old value was dead
   (r0 looked live after 774 calls through this alone).
4. ctr, ca, cr0, cr5-7 are kept across a call unless the callee may change them, like the other
   registers. Cause of the 2026-10-19 symptom ("Register spills per call site"): with
   pure-read liveness over all translated code, no call is followed by a read of ctr, ca or cr0
   before a write. cr5-7 are read after 357 calls (270 after step 8), all by an mfcr whose value
   feeds a cr0 single-bit extract (srwi/rlwinm .., 31, 31), in 5 functions (8003dd38, 8003fe98,
   80041358, 8004b0a0, 801a5f10; the first four run in the battle replay); only f_801ae9b4, an
   assert-path register dump that does not run, reads them at entry. Keeping them changes no
   guest value. With the DL cache keyed on guest offsets ("Per-model cost", 4-way) the campaign
   replay shows `cache hits 97 misses 0` in every window and both replays are identical, so the
   2026-10-19 misses were most likely the host-pointer-keyed DL cache conflict that changed with
   every build. No time change: the forced reloads were dead loads GCC already removed.
   `check_call_reads()` prints the mfcr count and lists any call followed by a ctr/ca/cr0 read
   (none).
5. call_indirect looks the address up in a generated open-addressing hash table (8192 slots,
   multiplicative hash, linear probing; 1.5 probes on average, longest 15) instead of a binary
   search. MTX bodies are no longer in the table: indirect calls reach their natives, as direct
   calls do (before, the table had both and the binary search picked one).
6. out(F) (`solve_out()`): a return stores only the dirty RETU registers some reader may see
   (was all of RETU). Least fixpoint: a direct call s in C adds (live after s | reaches C's return
   unwritten & out(C)) & RETU, where live includes later callees' in-sets (so SDK calls keep their
   logged r3-r6) and later stores; a tail call adds out(C); every call_indirect site adds the same
   to all address-taken functions; functions runtime/ calls directly (MTX bodies for cr_mtx_off,
   the mtx.c bench) keep all of RETU, __start none (main.c reads nothing after it). New
   assumption, guarded: a function that is not address-taken (`Program.taken`: data pointers,
   lis/addi constants, prologue-scan starts that no bl reaches, EXTRA_STARTS, the DOL entry, doc
   names no bl reaches) is entered only by translated bl/b, so its callers are known. functable.c
   sends call_indirect to any other start through a wrapper that logs `# call_indirect <addr>: no
   pointer to it seen by recomp.py` (16 at most): none in either replay. Static: 39042 of 49374
   return registers kept; r0 (mflr r0 in every prologue) is now rarely stored at a return.
7. In-sets: SDK natives were assumed to read all of CALLU; `native_reads()` scans runtime/*.c for
   R[k]/F[k]/cpu.gpr/fpr/cr reads, plus r1 (recomp_call_guest's stack) and r3-r6 (LOG()):
   r1, r3-r10, f1-f8, so not r0, r11, r12, cr1. A computed index must be listed in INDEX_READS
   (os.c R[gpr] gpr <= 10 and F[fpr] fpr <= 8, gx.c F[1 + i] taken as f1-f8) or the run fails.
   Callbacks that natives run through recomp_call_guest get r1/r3/r4 and are assumed to read no
   other register at entry, as for callbacks delivered at polls (which store only r1).
   call_indirect's in-set is the native in-set plus the in-sets of all address-taken functions
   (grown inside Summaries.solve; was all of CALLU).
8. Leaf inlining (`inline_leaves()`, a first translation pass picks them): straight-line leaves
   (one blr, no other event, no `cpu.` access, no ctr/ca write, at most 16 instructions, or 64 with
   at most 4 call sites, not MTX bodies) whose entry reads only CALLU registers are emitted at each
   bl to them. Their RETU writes go to the caller's locals, which the caller's own events store
   where something reads them (same EABI assumption as for calls); their other writes (f0, cr0,
   r1 ..) go to block-local `pv_` copies, which the caller never saw before either. `cpu.lr` is set
   only when lr can be read before the next write (`lr_read_after()`: mflr, blrl, return, tail,
   recomp_bad/skipped): 939 of 985 sites drop it, so the normal-build `# prof` (cpu.lr samples)
   counts an inlined leaf under the caller's previous call site, FNPROF under the caller. 985 calls
   to 258 functions; the hottest is 0x8001e5e4 (vec3 dot product, 508 calls per battle frame, 73
   sites). Code: f_ functions 6.55 -> 6.73 MB (+2.7%; the inlined copies +189 KB, the other steps
   -12 KB), most copies at sites that are cold in these replays. I-cache effect on hardware unknown.

Not done (by the counts above):
- Per battle frame there remain 58k stores before calls (mostly r3, r4, r5, r1, r6: arguments and
  the stack pointer), 73k entry loads (r1, r3, and r14-r31 / f14-f31 loaded only to save stale
  copies in prologues, see "Translator performance") and 27k return stores (r3, r4, f1-f4 that
  callers read, often through SDK in-sets). Removing most of that means passing argument registers
  as C arguments instead of through `cpu`, which is a redesign.
- MTX summaries: callers still store/reload what the translated MTX body may change (r5, r7, r11,
  r12, f2-f4, ctr: ~8k reloads per battle frame), because cr_mtx_off runs that body. Narrowing it
  to the natives' effect (MTX_REGS) changes which stale values later SDK log lines print (r5, r6).
- Per-native in-sets instead of the union over runtime/, no r1 store before natives that never
  call back, no cpu.lr store before calls to translated leaves: ~0.05 ms or less each.
- Inlining leaves with branches or loops (labels inside one Func instruction are not modelled).
- Not yet on hardware.

## Hardware run 25 (2026-10-22, perf-test bcd754a)

- Build task/2026-10-22-perf-test@bcd754a: the four perf branches above (codegen per guest instruction,
  per-model cost, GX thread per display-list cost, call-boundary glue) on the GX-thread base. Interactive
  run; log logs/cr_autotest_2026-10-22_hw-run25-perf-branches.log (sd: cr_autotest.log). Baseline: run 24
  (GX thread). Per-scene averages from tools/time_avg.py, GX thread on, gpukick on; different play each
  run, so window counts and content differ.

| scene | run 24 fps | run 25 fps | speed | game ms | gx wait ms | gpu ms | windows |
|---|---|---|---|---|---|---|---|
| battle 4.2 | 47.9 | 54.2 | 80 -> 90% | 19.1 -> 16.1 | 0.9 -> 1.4 | 4.2 -> 6.3 | 28 / 49 |
| campaign 8.3 | 42.4 | 44.4 | 71 -> 74% | 12.3 -> 5.5 | 10.7 -> 16.5 | 12.8 -> 18.9 | 81 / 53 |
| pause c.0 | 40.1 | 55.8 | 67 -> 93% | 14.4 -> 5.2 | 9.9 -> 12.0 | 3.3 -> 5.5 | 3 / 4 |
| scene 7 | 49.3 | 56.4 | 82 -> 94% | 11.7 -> 7.5 | 7.9 -> 9.7 | 4.7 -> 4.7 | 2 / 7 |
| movie 1.0 | ~9 (run 23) | 13.9 | 23% | 65.5 | 6.0 | 2.3 | - / 2 |

- Battle: game CPU -16%, still CPU-bound (gx wait ~1 ms).
- Campaign: game CPU more than halved, but the frame now waits on the GX thread / GPU (gpu ~19 ms per
  frame); further campaign gains need GPU work (fill vs vertex not yet measured on this build).
  Whether the gpu increase vs run 24 is the build or the play is not checked.
- Title/menus unchanged at ~60 fps.
- Not covered: the main merge with audio pacing (8edc577) has not been run on hardware.

## Vertex shader variants (2026-10-22, reverted)

**Reverted:** on New 3DS hardware (run 26) the variants hang the GPU at the first draw that uses them (boot froze at frame 12; switching to them in campaign froze too). Citra did not show it. Cause not found; the single shader is the only path again. Notes kept for a later retry.

- One PICA program ran every path (`runtime/vshader.v.pica`: lighting behind `ifu blit`, three
  projective texgens always). Now 14 variants (`tools/gen_vsh.py` -> `runtime/vsh/*.v.pica`, generated,
  committed): lit or unlit x 0-3 texture units, all affine (`dp4` s/t only) or all projective. picasso
  links them with the single shader into one SHBIN (Makefile rule for vshader.shbin), so a switch resends
  operand descriptors / entry point, not the code. Chosen per draw in gx.c (`vsh_variant`: lit from
  set_lighting, units from setup_draw_state, projective = a texm q row other than (0, 0, 0, 1), from
  set_texgen); a switch flushes the pending draw, so batching never merges across programs.
- picasso 2.7.2 limits: uniform and `.constf` registers are allocated across all linked files (past 96 it
  fails, or segfaults), names stay per file, procs are global. So variants declare no uniforms: they read
  the single shader's registers (`.alias u c0` + offsets, `k` = c95, loop count i0); gx.c checks the
  locations at init (mismatch -> logged, single shader only) and keeps k loaded at c95 from the single
  DVLE's constant bits. libctru returns int uniform locations as register - 0x10 (iloop = 0x60).
- Executed instructions per vertex (single -> variant): position 11 -> 8 (`dph` instead of mov w=1 + dp4);
  colour unlit 4+2 (mov amb, ifu) -> 2 (clamp of amb dropped: amb is in [0, 1]); per affine texture unit
  5 -> 2, unused units 5 -> 0. Totals: unlit no texture 33 -> 11, unlit 1 unit 33 -> 13, unlit 2 units
  projective 33 -> 21; lit with n lights 40 + 23n -> 22 + 23n plus 2 per affine unit (1 light, 1 unit:
  63 -> 47). Lit stays dominated by the light loop (23 per light).
- Draw mix (draws / indices per frame, Citra replays): campaign 8.3 (pause-ok r1500-2100): l1t1 924/47844,
  l0t0 233/10233, l0t1 100/4482 = 77% of indices lit + 1 affine unit; 7 program switches per frame,
  81 draw calls (82 single). Battle (r3000-3300): l0t0 1123/41556, l1t2 938/21744, l1t2p 7/8862, l0t1
  78/2550, l1t0 16/3600, others small; 15 switches per frame. Light counts per draw not logged.
- Attributes: still 68 bytes and 6 attributes per vertex for every program (pos f32x4, colour u8x4, normal
  f32x3, 3 x tc f32x3); unlit/untextured programs ignore normal/tc but the loader still fetches them. No
  smaller type is exact for positions/normals/tc. A per-program loader (BufInfo padding) is not done.
- Check (Citra, `tools/citra/replay.ps1`, variants vs `cr_vsh_single.txt`): SDK lines (minus
  OSTicksToCalendarTime) and scene/pad lines identical for campaign (2232 / 546 lines) and battle
  (2371 / 152); framebuffer dumps campaign r1500/1980/2300 and battle r2100/2700/3300 byte-identical.
  Citra fps unchanged (host GPU: no signal). Logs not in git (logs/vsh/ in the worktree). Not yet on
  hardware; hardware rcp(1.0) may not be exactly 1, so affine texcoords can differ from the single
  shader's by ~1 ulp there (GX does not divide 2x4 texgen either).
- Flag: sdmc:/cr_vsh_single.txt = start value; `recomp_vsh_single_req` (0/1, any thread) switches at the
  next GX frame, logged "# gx r<n>: vsh single on|off"; `# time .. split` says "vsh variants|single|mixed,
  N program switches per frame"; `# gx vsh frame N` (every 300) gives draws/indices per program. Runtime
  toggle not exercised yet.

## Cull mode (2026-10-22)

**Now always on** (clean build 2026-10-22): cr_cull_off.txt and the runtime toggle were removed after hardware run 26.

GXSetCullMode was ignored (C3D_CullFace(GPU_CULL_NONE) always, "cull mode 2 drawn without
culling" in every run). `setup_pixel` now maps GX 2 (back) -> GPU_CULL_FRONT_CCW and GX 1 (front)
-> GPU_CULL_BACK_CCW. The opposite pairing (the first try) culled the visible faces, which blanked
most of the battle arena in the framebuffer dumps. That happens because set_clip mirrors window
space. GX 3 (all) has no PICA mode, so it is drawn unculled with a ONCE log line; it has not shown up in
the replays. sdmc:/cr_cull_off.txt = the old behaviour, as the start value. `recomp_gx_cull_toggle()`
(ppc.h) flips it at the next frame start and logs "# gx r<n>: cull on|off"; the `# time .. split` line
says "cull on|off|mixed".

Citra replays (logs/cull, not committed), cull on vs cr_cull_off, and vs the pre-fix build: SDK, pad
and scene lines are identical. The campaign fb dumps (r1500/1980/2300) are pixel-identical. The battle
dumps (r2100/2700/3300) differ in 1.2k-8.5k pixels. With culling off, the P1/P2 marker hexagons are
drawn as solid blue/red back faces over their emblem, and the back faces of the translucent arena
cubes are blended in. With culling on, the emblems show and those back faces are gone, as GX
culling would do. This has not yet been checked against Dolphin. Battle draw calls go up from 123 to
165 per frame (indices are unchanged at 89.5k), because cull state changes split the merged batches.
Campaign draw calls are about the same (82 -> 83). Not yet on hardware.

## Texture mipmaps and lossless formats (2026-10-22)

**Now always on** (clean build 2026-10-22): cr_mip_off.txt, cr_texfmt_off.txt, cr_tex_fliptest.txt and the toggles were removed.

`runtime/gx.c` texture path: mip chains are uploaded when the GX texmap asks for them (min filter
NEAR/LIN_MIP_*, levels = (max_lod + 15) / 16 + 1 as Dolphin, capped at 1x1 on the GX side and 8x8 on
the PICA side; min/max LOD to whole PICA levels, bias as is). TexObj now keeps LOD fields like the SDK
(mode1 min/max LOD u4.4, mode0 bits 9-16 bias): GXInitTexObj with mipmap sets max LOD = log2 of the
larger side and LIN_MIP_LIN (LIN_MIP_NEAR for CI), as the SDK (0x801560c8); GXInitTexObjLOD and the TPL
helpers pass min/max LOD and bias (bias clamp, edge LOD, anisotropy not kept). Formats decoded to RGBA8
(RGB5A3, RGBA8, CI, CMPR) are put in the smallest PICA format that holds every texel exactly: LA4,
RGB565, RGBA5551, LA8, RGBA4, else RGBA8. ETC1 is lossy and not used; its size is only reported.
Also fixed: RGB5A3 expands by bit replication (Dolphin Convert5To8/3To8; was x*255/31, 1 LSB off for
some values), min filter texel/mip bits read from the GXTexFilter value (LIN_MIP_NEAR was nearest).

Flags (start value; `recomp_tex_mip_toggle()` / `recomp_tex_fmt_toggle()` flip at the next frame
boundary, logged "# gx r<n>: texture mipmaps|lossless 16-bit texture formats on|off", cache invalidated
by a generation in the entry key): sdmc:/cr_mip_off.txt, sdmc:/cr_texfmt_off.txt; test
sdmc:/cr_tex_fliptest.txt flips formats every 300 retraces. Shown in "# gx: texture mipmaps" at start,
"# time .. split" (mipmaps/tex 16-bit on|off|mixed), "# gx tex frame" every 300 frames (resident KB,
ETC1 estimate) and "# gx tex new:" once per texture (inventory).

Citra results (pause-ok campaign and rec-to-battle replays, fb dumps 1500/1980/2100):
- Inventory: the game uses **no mipmaps**. Every texture (campaign 57, battle 45 distinct) has min
  LINEAR (4 RGBA8 are NEAR), LOD 0..0. So the mip path is inert in this game and min filters are already
  plain LINEAR; the "minified textures thrash the cache" idea has nothing to act on here. (Assumes no
  game code edits TexObj bits without the SDK calls, which are hand-written.)
- Formats (campaign): CI8 46, CMPR 24, RGBA8 23, RGB5A3 8, I4 2. Lossless pick: LA4 3, LA8 11,
  RGBA5551 4, RGBA8 39. CMPR almost never fits 565 (interpolated colours are not 5/6/5 values).
- Resident texture memory at frame 2100: campaign 9995 -> 9809 KB (-1.9%), battle 7751 -> 7547 KB
  (-2.6%). As ETC1/ETC1A4 it would be ~2201 / 1872 KB (-78% / -76%), lossy.
- SDK/scene/pad lines identical to main in all runs; fb dumps flags on vs off identical (campaign and
  battle), on vs flip test identical. Main vs this branch: campaign identical, battle 9-1101 pixels
  differ by 1-2 (the RGB5A3 expansion fix).
- Citra gpu ms is not a signal (0.3 everywhere). Expected hardware gain is small (~2% fewer texture
  bytes); not yet on hardware. The remaining campaign fill lever would be ETC1 (lossy, needs a decision).

## Hardware runs 26-27 (2026-10-22, campaign GPU)

- Run 26 (combined test build with vsh variants; boot froze at the first draw, so it was played with
  cr_vsh_single.txt; switching the variants on in campaign froze again). Campaign 8.3, per flag state
  (time_avg.py): all on 52.1 fps / gpu 15.6 ms (55 windows); cull off 46.6 / 19.9 (2); mipmaps off 49.8 /
  18.4 (11, inert in this game, so noise); tex formats off 47.8 / 19.1 (2). Log
  logs/cr_autotest_2026-10-22_hw-run26-campaign-gpu-vsh-single.log.
- Run 27 (main 8575bae, clean build: cull + texture formats always on, no toggles; campaign and
  several Vs. battles; HOME exit, no crash; log logs/cr_autotest_2026-10-22_hw-run27-clean-main.log):

| scene | run 25 fps | run 27 fps | speed | frame | game | gx wait | GX busy | gpu | windows |
|---|---|---|---|---|---|---|---|---|---|
| campaign 8.3 | 44.4 | 51.2 | 85% | 19.5 | 8.7 | 10.1 | 18.0 | 12.2 | 210 |
| Vs. battle 4.2 | 54.2 | 45.3 | 76% | 22.1 | 18.1 | 2.6 | 15.8 | 9.3 | 103 |
| battle 4.0/4.1/4.3/4.5 | - | 53.4/59.8/54.6/59.0 | 89-100% | | | | | | 7-13 |
| menus 5.0 | - | 59.6 | 99% | 16.8 | 3.4 | 13.1 | | 3.9 | 43 |
| pause c.0 | 55.8 | 59.1 | 99% | 16.9 | 3.6 | 12.8 | | 2.7 | 7 |
| scene 6 / 7 | - / 56.4 | 53.9 / 57.4 | 90 / 96% | | | | | | 11 / 9 |

- Campaign: GPU 18.9 -> 12.2 ms, so campaign is no longer clearly GPU-bound. The GX thread is now busy 18.0
  of a 19.5 ms frame (sync 13.2 of it = waiting in C3D_FrameBegin), so the frame is bound by the GPU queue
  plus the GX thread. Game CPU 8.7 (run 25: 5.5; different areas played).
- Battle 4.2: the drop from 54.2 to 45.3 fps (game CPU 16.1 -> 18.1 ms) comes from what was played. Run 27's
  battle windows were mostly campaign battles, where the opponent fights back with more models, pods,
  explosions and effects; run 25 was a 1-v-1 Vs. arena match, which the user saw run better in run 27
  too. Still CPU-bound by the game thread (gx wait 2.6).
- Draw calls per frame (split line): campaign ~130 with 75-94k indices at peak windows.
- Stalls: 30 DVD-slow / log-write-slow lines, mostly at boot (font.out 180 ms, logo.tpl 64 ms).

## VRAM textures (2026-10-22, not yet on hardware)

Cached GX textures move from linear FCRAM to VRAM (`runtime/gx.c`, `tex_to_vram`; comment above it has
the ordering argument). Reason: campaign 8.3 is GPU-side limited (run 27 gpu 12.2 ms, GX sync 13.2) and
partly fill-bound (run 17b quarter resolution: gpu 16.6 -> 10.2), and the PICA reads VRAM faster than
FCRAM. MKW-3DS and the Simpsons Hit & Run O3DS port also put textures in VRAM (no numbers published).

- Decode is unchanged (into linear memory). Once a texture's contents stayed the same for 2 frames, on its
  first use in a frame it is copied to VRAM with GX_TextureCopy added to citro3d's GPU queue: the queue runs
  in order, so the copy finishes before this frame's command list and after all earlier work. The linear
  block is freed at the next frame_begin (C3D_FrameBegin waited for the queue). citro3d's own VRAM upload
  (C3D_TexLoadImage -> C3D_SyncTextureCopy) is not used: outside a frame it waits for and clears the queue.
- At most 8 moves per frame, and only while the 32-entry queue keeps room for 12 mid-frame submits + 8;
  textures over the cap stay linear and move on a later frame.
- VRAM budget: 5394 KB free after the 400x240 RGBA8 + D24S8 render target; a 256 KB reserve is kept. The
  working set is bigger than VRAM (battle replay: ~5 MB in VRAM, the rest linear), so first come, first
  served, plus: when VRAM is full, a VRAM texture not drawn for 60 frames is copied back to linear memory
  (queued before the move that takes its VRAM; that frame its linear block is not reused, rewritten or
  freed: `dmf`). First version deleted those textures: they all decoded again at battle r3000 (one 337 ms
  frame in Citra, both runs); copying them back removed it.
- VRAM textures are never rewritten in place: changed contents go to a new linear entry.
- sdmc:/cr_vramtex_off.txt = linear only. Bottom-screen button "VRAM textures" (the touch button grid in
  `runtime/menu.c` is back, with this one button) toggles it at the next frame start, invalidating the
  texture cache (tex_gen), logged "# gx r<n>: VRAM textures on|off". Logs: boot "# gx: VRAM textures";
  every 300 GX frames "# gx vram tex frame": counts/KB per pool, VRAM free, moves, over cap, VRAM full
  (fallbacks), moved back; every `# time .. split` line ends with "vram tex on|off|mixed (..)", grouped by
  tools/time_avg.py as vram=on/off (mixed windows dropped).

Citra replays (logs/perf dbase*/dvram*, flag file vs none, 2 runs each): campaign and battle SDK/scene/pad
logs identical, framebuffer dumps identical (campaign 1500/1980/2300, battle 2100/2700/3300), 1 texture
miss each (same as base). gx tex: campaign 0.6 both, battle 0.2 -> 0.3 ms (copy bookkeeping); worst frames
the same. Linear free at the low point: battle 769 -> 5462 KB, campaign 9118 -> 13636 KB. Citra does not
model FCRAM vs VRAM bandwidth (gpu ms unchanged there), so the gain can only be measured on hardware.

Hardware A/B to run: campaign 8.3 with and without the flag (or tap the button; windows with a tap are
dropped), compare gpu ms (run 27: 12.2) and GX sync (13.2) / fps (51.2) per vram=on/off with
tools/time_avg.py; check the "VRAM full" counts and that no texture looks wrong after a scene change.
## -O3 and LTO (2026-10-22, off by default)

Other 3DS ports build with -O3/-O4 and LTO; this one builds everything at -O2 without LTO. Makefile options,
both off by default: `GENOPT=-O3` (extra flags for gen/recomp_*.c only, after -fno-ivopts, which still
wins), `LTO=1` (-flto in CFLAGS, -flto=20 at link). LTO needed `__attribute__((used))` on the
`__assert_func` / `abort` overrides in log.c (libc members that call them are pulled in after LTO had dropped
them: undefined __assert_func). With LTO the per-file -fno-ivopts is kept (it is in the objects' LTO option
records, GCC keeps per-function optimisation options) and check_3dsx.py passes for every variant.
Citra battle replay (scene 4.2, mean game ms of the 20 windows r2460-3600; Citra time is deterministic, the two
runs of each variant agree to 0.01 ms), full build with -j20:

| variant | game ms | vs -O2 | .text | .3dsx | build |
|---|---|---|---|---|---|
| -O2 (default) | 19.96 / 19.96 | | 7.27 MB | 7.36 MB | 57 s |
| GENOPT=-O3 | 19.94 / 19.94 | -0.1% | 7.93 MB (+9%) | 8.03 MB | 68 s |
| LTO=1 (-O2) | 19.66 / 19.66 | -1.5% | 7.42 MB (+2%) | 7.53 MB | 51 s |
| LTO=1 GENOPT=-O3 | 19.66 / 19.67 | -1.5% | 9.81 MB (+35%) | 9.93 MB | 87 s |
| LTO=1 + -fipa-pta at link | 19.76 / 19.75 | -1.0% | 7.43 MB | 7.54 MB | 61 s |

- SDK call / pad / scene lines and the framebuffer dumps (r2100/2700/3300) identical to -O2 in every variant.
- -O3 does nothing for the translated code (goto-heavy, memory accessors; little to vectorise or unroll
  usefully) and grows it 9%; with LTO it grows it 35% for no gain. Not worth it on a 16 KB I-cache.
- LTO -1.5% (below the 2% bar for a default change) with +2% code: probably cross-file inlining of runtime
  helpers into gen/. Citra does not model the ARM11 caches, so the hardware effect is unknown either way;
  LTO=1 is the one worth a hardware A/B if battle CPU gets tight. Not yet on hardware.
- -O3 on runtime/ hot files (gx.c) not tried: -O3 on gen/ gave nothing and battle is bound by game code.

## Vertex packing (2026-10-22, not yet on hardware)

Every vertex the PICA read was a 68-byte `PVtx` (float pos[4], u8 mat[4], float nrm[3], 3 x float tc[3]),
whatever the draw used. Campaign 8.3 is GPU-side limited on hardware (run 27: gpu 12.2 ms, 55.7k indices
per frame). Other 3DS ports shrink vertices for this: mw3ds uses 16-byte s16/u8 vertices, the Simpsons
Hit & Run O3DS port drops normals from static meshes, REGTA packs normals into bytes.

What the draws use (`# gx vattr frame` lines, per stats frame, weighted by vertices drawn; nearly all
vertices come through the display-list cache):

| | campaign 8.3 (28k verts) | battle 4.2 (35-51k verts) |
|---|---|---|
| texcoord sets fed 0/1/2/3 | 14 / 86 / 0 / 0 % | ~45 / 15-24 / 26-40 / 2 % |
| light loop on (normal read) | 80 % | 50-57 % |
| position | float 72 %, s16 frac 11 28 % | float 60-90 %, s16 frac 11-14 the rest |
| normal (lit draws) | float 82 %, s16 frac 14 18 % | mostly float, s16 frac 14 0-40 % |
| texcoords | float 72 %, u16 frac 15 14 % (of all vertices) | float and u16 frac 15; units fed from the normal (env map) 15-30 % |

So float sources dominate: most of the saving is leaving out unused texcoord sets and normals, and storing
2-component texcoords. s16/s8 packing helps the minority.

Design (`runtime/gx.c` comment at `VLay`, `plan_finish`, `decode_pk`, `use_buf`):
- Display-list draws (vertex cache and display-list cache) get a per-draw layout. Only the shader inputs
  the draw reads are stored, in the GX source format where the PICA has one: s16 -> GPU_SHORT, s8 ->
  GPU_BYTE, u8 -> GPU_UNSIGNED_BYTE, f32 -> GPU_FLOAT (byte-swapped as they are). u16 has no PICA format,
  so it is converted to float as before. Immediate draws (vbuf) stay PVtx.
- Every attribute is a multiple of 4 bytes, so offsets stay aligned and the attribute sizes add up to the
  stride (no padding attributes needed): position s16x4 / s8x4 / f32x4, with the matrix row in w; material
  u8x4; normal s16x4 / s8x4 / f32x3; texcoord s16x2 or x4, s8x4, f32x2 or x3.
- The integer scales are exact. The PICA converts an s16 to float24 exactly, and a power-of-2 scale
  commutes with float24 rounding. The position and normal scales are a new `vscale` uniform: the shader
  does `mul r0.xyz, vscale.x, inpos` and `mul r13.xyz, vscale.y, innrm`. In the 68-byte layout vscale is 1,
  so that path computes what it did before. A texcoord's scale is folded into its texm columns (only
  set_vtx_unif uploads texm now, from set_texgen's `tex_m`).
- The normal is left out when the light loop is off and it feeds no texgen. A unit nothing feeds reads a
  fixed attribute (0, 0, 1, 1), the value PVtx had there. All 12 fixed-attribute slots are set once at init
  and never change, so stereo replay and mid-frame submits are not affected.
- A texcoord from TEXn has 2 components (the loader's z = 0) only when that unit's texm column 2 is zero.
  Otherwise z = 1 is stored, as in PVtx: the dp4 then has the same terms.
- Still one shader program. Each layout is a C3D_AttrInfo: loaders in register order, then fixed slots.
  use_buf compares a 30-bit layout id. Battle has ~20-30 layout switches per frame; batching is unchanged
  (190 DrawElements for 2200 draws, the same as before).
- The vertex arena is now in 4-byte units. Each allocation starts at a multiple of the vertex size
  (ring_alloc's `al`), and the u16 index window is a multiple of 0x8000 vertices of that size, so indices
  stay below 0x10000.
- Cache keys: the layout (VLay, which includes the scales) is hashed into plan_sig. PlanKey also holds
  pk / lit / texm column-2 mask, so a mode change only misses and nothing has to be flushed. That is why
  the bottom-screen button can switch modes between frames.
- The plan memo is now 2-way (512 entries, key hash checked first). With the new key fields, 256 entries
  direct-mapped collided on 10 of ~250 battle calls a frame (misses 2 -> 10-12; now 0).
- Flag sdmc:/cr_vtxpack_off.txt = every vertex 68 bytes. The bottom-screen button "Vertex pack" is bound
  to `recomp_vtxpack_off`; the menu flips it at the retrace sync. Logs: boot "# gx: vertex packing";
  every 300 GX frames "# gx vpack frame" (vertices/bytes decoded that frame, layout switches, vscale/texm
  uploads) and "# gx vattr frame" (the distribution above); every `# time .. split` line ends with
  "vtx pack on|off (B per index, KB vertex fetch per frame)", indices x vertex size, with no
  post-transform cache. tools/time_avg.py groups it as vpack=on/off.

Citra replays (logs/perf base1/2 = main, p9/p9b = pack, p9off = flag file, p9st/basest = cr_stereo_force):
campaign and battle SDK/scene/pad logs identical, and framebuffer dumps identical (campaign 1500/1980/2300,
battle 2100/2700/3300, and both eyes in stereo, 0 fallbacks). Vertex fetch per frame: campaign 4154 ->
1992 KB (68 -> 32.6 B per index), battle 5815 -> 2513 KB (68 -> 29.4 B). Vertex arena in use: campaign
3881 -> 1749 KB, battle 4657 -> 2047 KB. GX-thread vertex time (Citra ticks, scene average): campaign
3.1 -> 3.2 ms, battle 5.7 -> 6.1 ms (6.0 with the flag: most of it is the restructure, i.e. the
per-draw texm/vscale check and the layout bookkeeping, not packing itself). Decode in the load windows is
~+20 % (generic per-attribute byte copies vs the specialised float readers), e.g. battle r1800
1.5 -> 1.8 ms. fps and game ms unchanged; battle is game-CPU-bound.

Citra does not model PICA vertex-fetch bandwidth, so the GPU gain can only be measured on hardware.
Hardware A/B to run: campaign 8.3 gpu ms (run 27: 12.2) and fps, vpack on vs off (button or flag file),
with tools/time_avg.py. Also watch the GX thread busy ms, which was 18.0 of 19.5 in run 27: packing adds
a little GX-thread CPU.

## Stereo 3D (2026-10-22, not yet on hardware)

**What it does.** When the 3D slider is above 0 (`osGet3DSliderState`, read every frame on the GX thread),
gx.c records the command words of the left eye. At frame end it binds a second render target, set as the
output for GFX_TOP/GFX_RIGHT, and appends the same words again with GPUCMD_AddRawCommands. This is the mw3ds
technique: the right eye costs GPU time and a memcpy, with no second pass of game or GX CPU work.
`gfxSet3D(true)` is called the first time stereo is used, and the right target is created then too
(VRAM: 240x400 RGBA8 + D24S8). After that, a frame that does not use the right target is shown in 2D,
with the left framebuffer sent to both eyes (libctru `gfxScreenSwapBuffers(..., hasStereo)`).

**How the replay is kept correct** (citro3d 1.7.1 source read: base.c, renderqueue.c, uniforms.c, internal.h):
- citro3d only emits dirty state, so the left eye's first draws normally rely on state left over from the previous
  frame. The right-eye replay would start from the left eye's end state instead. To avoid that, `stereo_begin`
  calls `C3Di_UpdateContext()` right after `C3D_FrameDrawOn(left)`. This emits the left framebuffer outside the
  recording. It then sets every other dirty flag (`C3DiF_RESEND`: attr/buf info, effect, viewport, scissor,
  program without code, texenv buffer, light enable, all tex units, all texenvs). It also calls
  `C3Di_DirtyUniforms` and dirties the 4 int uniforms, which DirtyUniforms misses (it clears their EverDirty
  flag). The first draw therefore re-emits the full state inside the recording. `__C3D_Context` is declared in
  gx.c with only the head of citro3d's struct (gxQueue, cmdBuf, cmdBufSize, cmdBufUsage, flags).
- `eye` (vshader.v.pica, the 92nd of 96 float uniform slots including the constant `k`) is written raw
  (GPUREG_VSH_FLOATUNIFORM_CONFIG/DATA) before each eye's words and is never set through citro3d. So no
  recorded word writes it, and citro3d never re-uploads it. eye = 0 is written once at the first frame and
  whenever stereo turns off.
- Mid-frame submits (gpu_kick, up to 12 per frame) split the words into ranges. The FINALIZE / framebuffer
  flush words that C3D_FrameSplit adds between chunks are not recorded. The replay goes into the frame's
  last chunk.
- Right eye: `C3D_FrameDrawOn(right)` + `C3Di_UpdateContext()` (left framebuffer flush, right framebuffer,
  viewport), eye, the ranges. Then citro3d's DrawUsed flag is set again (the replayed draws did not set it),
  so C3D_FrameEnd flushes the right framebuffer before its display transfer. The right target is cleared
  together with the left one, and also when stereo turns on.
- Fallback (counted, the frame is shown in 2D): the left eye hit the command-buffer limit, the ranges
  overflowed, or the replay would not fit in the rest of the 2 MB buffer. 0 fallbacks so far: battle frames
  use about 2% of the buffer, so the buffer was not raised.

**Shader.** Clip-space xy += eye.xy * max(w - eye.z, -w) * (1 - clipm[3].w). With eye = (0, +-sep * slider,
conv, 0), the per-eye shift in NDC is sep * (1 - conv/w), clamped to at least -sep. The display's horizontal
axis is clip y, because the top screen is rotated (set_clip). The left eye gets +, the right eye gets -; the
signs were checked with fb dumps. Distant objects get up to 2 * sep * 200 = 10 px of uncrossed disparity at
full slider. Objects nearer than conv cross, by at most the same 10 px. Orthographic draws have
clipm[3] = (0,0,0,1), so the factor is 0 and the HUD stays at screen depth. Default sep 0.025, conv 3.0.
Battle eye-space w is about 3 to 60: conv 40 put most of the arena 24+ px in front of the screen, and conv
15 clamped almost everything. With conv 3 the arena sits 0-10 px behind the HUD (Citra fb dumps r2100/2700/
3300, measured by best horizontal shift per band; script in the session notes). This is a single shader with
no variants (see "Vertex shader variants").

**Cost (Citra, logs/perf in the worktree, not committed).** Battle 4.2 (r2460-3600): mono vs forced stereo:
game 20.0 / 20.0 ms, GX thread busy 13.6 / 13.9, 44.9 / 44.7 fps. 8-16k replayed words per frame (32-64 KB
memcpy), cmdbuf 2% either way, 0 fallbacks. Campaign 8.3: busy 15.5 / 15.7, 0 fallbacks, ~11k words. The
gpu column is meaningless in Citra (about 1 ms in both modes). The real cost is GPU time, roughly double the
top-screen draw time (battle gpu 9.3 ms and campaign 12.2 ms in hw run 27, so expect about 19 / 24 ms). That
would make campaign GPU-bound below 60 fps. If fps drops to about 30, game speed may halve, because the game
appears frame-locked (unconfirmed). Not yet on hardware.

**Mono path.** Slider 0 (no flag file): the battle and campaign replays are identical to main 342a766 (SDK,
pad and scene lines, and fb dumps; tools/citra/compare_logs.py). The only extra GPU words are one eye = 0
write at frame 0. The shader adds 0 to xy.

**Open.**
- Hardware: real GPU cost, how the slider feels, whether 2D fallback frames flicker the parallax barrier,
  and whether conv/sep suit the campaign. The campaign replay's top screen is black in Citra from r1100 on
  (fb dumps and window shots, main too), so campaign stereo has not been checked visually.
- Split-screen battle views: the shift is in full-screen NDC, so it applies across the viewport split,
  where the scissor cuts it.
- The slider's position only scales the separation. conv is fixed and has no runtime control.
- EFB-to-texture copies are not emulated, so render-to-texture effects get no stereo treatment.

## 2v2 game CPU: native sin/cos, inline fmadd, non-volatile saves (2026-10-22, not yet on hardware)

Task: hardware run 28's 2v2 battle runs 30.5 fps with game CPU 29.8 ms of a 32.8 ms frame. Find where the
game thread's time goes in the `2v2` replay (scene 4.2 r4200-8610) and cut it without hand-porting game logic.
Citra only (`replay.ps1 -Name 2v2`, `time_avg.py --from 4260`; Citra time counts instructions).

**Profile** (`make FNPROF=1` `# prof`, main@9dc614c, 2v2 r4500-8400 vs 1v1 `battle` r2700-3600; ms = share x
FNPROF frame, 64.6 ms in the 2v2, 23.9 in the 1v1; `RECOMP_SITEPROF=1` for calls per frame):

| what | 2v2 ms | 1v1 ms | class | note |
|---|---|---|---|---|
| call-boundary glue (00000000) | 11.6 | 4.0 | translator | 35.7k calls, 23.9k entries per frame; 139k register stores before calls, 92k reloads, 212k entry loads, 82k return stores |
| MSL sin/cos (0x801689d0 cos 0.51, 0x80168f38 sin 0.48, __kernel_cos 0.73, __kernel_sin 0.64, __ieee754_rem_pio2 0.78) | 3.1 | ~0.9 | SDK (MSL) | ~2400 sin/cos calls per frame, each 2-3 more translated calls; native now |
| 0x801829d0 | 3.1 | 0.26 | game | 186 calls per frame, cos/sin x4 per call, 16 FPR saves; with 0x8017b4a4 (1.0), 0x801835b0 (0.9), 0x801822d0 (0.5), 0x8018144c (0.45): a code family at 0x8017b000-0x80185000 that is almost idle in the 1v1 (effects or particles: a guess). Corrected 2026-10-25: it is the HSD particle system, and the sin/cos in 0x801829d0 are static call sites not taken at run time; see "2v2 particle family" |
| 0x80060fa4 | 2.9 | 0.87 | game | 346 calls per frame, call-free float loop (physics/collision), 15 FPR + 9 GPR saves |
| PSMTXConcat (native) | 1.8 | 0.6 | SDK | 991 calls per frame |
| 0x8019fc14 (angles -> rotation) | 1.55 | 0.38 | game | 273 calls, 11 FPR saves |
| 0x8001b544 (vec3 distance check) | 1.33 | 0.35 | game | 904 calls per frame |
| GX SDK calls on the game thread (GXBegin, GXLoadPos/NrmMtxImm, GXSetVtxDesc..) | ~2.5 | ~1.2 | runtime | |
| waits (GXDrawDone 1.9, VIWaitForRetrace 1.1) | 3.0 | 2.0 | runtime | |

What grows with four robots: the glue (+7.5 ms, more calls), the 0x8017b000-0x80185000 family (+5 ms), sin/cos
(+2.2), 0x80060fa4 (+2.0), PSMTXConcat (+1.2), 0x8019fc14 and 0x8001b544 (+1 each). Prologue saves of r14-r31
and f14-f31 alone ran ~37k stw, 33k stfd and 33k psq_st per 2v2 frame.

**1. Native MSL sin/cos** (`runtime/libm.c`, recomp.py `MSL_NATIVE`, handled like `MTX_NATIVE`: calls go to
sdk_sin/cos/sinf/cosf, the translated bodies stay for the rare paths and `cr_libm_off`):
- Names: `docs/sdk_functions.md` had sin and cos swapped. 0x801689d0 calls __kernel_cos for |x| <= pi/4 and
  takes quadrants kcos, -ksin, -kcos, ksin, so it is **cos**; 0x80168f38 is **sin**; 0x801693dc = frsp(sin) =
  sinf and 0x80169400 = cosf (were swapped too). 0x801684dc = __kernel_sin(x, y, iy r3), 0x80167594 =
  __kernel_cos, 0x801671f4 = __ieee754_rem_pio2 (npio2_hw at 0x801c4c58), all checked in the disassembly.
- The native follows the guest instruction order with the same operations as gen/, so results are those of
  the translated code (not Gekko's: see 2). It leaves what the translated calls leave in `cpu` for a later
  reader: r3, r4, r6, f1-f4 (ps0; lfd and double ops keep ps1) and XER[CA] (srawi in rem_pio2's careful path),
  and not r0, f0, f5-f7, cr0 or the guest stack frames (no translated return stores those). |x| > 2^19 pi/2,
  inf and NaN run the translated body (rem_pio2's large path calls __kernel_rem_pio2). recomp.py allows the
  extra writes (NATIVE_WRITE_EXTRA: libm.c r6, f2-f4, cpu xer_ca).
- `sdmc:/cr_bench.txt` adds `# bench libm` lines: 20000 inputs per function (small, medium, up to and past
  2^19 pi/2, float arguments, near n pi/2, tiny, the pi/4 boundary, inf/NaN/0), every compared register poisoned
  first, ps1 included: 0 differ for all four. Citra per call: sin 3443 -> 1085 ns.
- `sqrtf` (0x80169424, `bl sqrt` with no frsp, 596 calls per 2v2 frame) is a plain native (`sdk_sqrtf`).
- Live toggle: bottom-screen button "Native sin/cos" (bound to `recomp_libm_off`); the `# time .. split` line
  ends with `libm on|off|mixed` and `time_avg.py` splits by it.

**2. Double fmadd inline.** devkitARM's newlib `fma()` is `vmla.f64 d2, d0, d1; vmov; bx lr` (libm.a,
armv6k/fpu): VFPv2 has no fused multiply-add, so it rounds twice, unlike Gekko's fused fmadd/fmsub/fnmadd/
fnmsub. gen/ called it at 460 sites (MSL math, a few HSD functions). recomp.py now emits `fmadd_d(a, c, b)` =
`a * c + b` (ppc.h): the same two roundings without the libm call (logs, framebuffers and the libm bench are
unchanged). Open: double fmadd is not fused, so double math (MSL sin/cos/atan2/pow..) can differ from Dolphin
in the last bit; a correct fused multiply-add in software would cost much more. The old ppc.h comment calling
fma() "a software libm call" was wrong and is corrected.

**3. Non-volatile saves dropped** (recomp.py `drop_nv_saves()`). Translated callers keep r14-r31/f14-f31 in C
locals and never reload them from `cpu` after a call ("Translator performance"), so an MWCC prologue saved the
(possibly stale) `cpu` copy it loaded at entry, and the epilogue loaded it back into a local that is dead at the
return. The pass drops a save and its restores when: the frame is one `stwu` at the entry; r1 is otherwise only
written by `addi r1, r1, size` with no stack access after it before the return; no address is derived from r1
(indexed or update forms, `mr rX, r1`, r1 arithmetic other than `addi rX, r1, k`, or such a pointer at or
above the lowest save slot); the register still holds its entry value at the save (prologue = straight-line
code from the entry); every other access to the slot in the function (via r1 or r11 = r1 + N for the inlined
_savegpr/_restgpr) is a restore of the same register at the same offset; and the restored register is not read
before the function exits. Result: 1886 functions, 9981 stw, 1874 stfd, 1837 psq_st and 13665 restores dropped;
5 functions keep theirs (other r1 use); no slot was kept for another access. 3dsx 7.37 -> 7.05 MB.
`RECOMP_KEEP_NV_SAVES=1 python tools/recomp/recomp.py` keeps every save (A/B builds).
- Semantics: the saved slots in guest stack memory keep older contents. Before, they held the stale `cpu`
  copies, not the caller's real registers, so a reader of them was already off; a reader of uninitialized stack
  (or a back-chain walker reading saved registers) would now see other values. Tail calls after a dropped
  restore store the function's own register value instead of the reloaded stale copy (the tail target saves
  nothing from it, or only into its own dropped slot). Identity checks below found no difference.
- Bench (`cr_bench`): translated gmtx_mul (0x8001e134, game code) 3131 -> 2161 ns per call, translated
  PSMTXConcat body 3189 -> 2992 ns.

**Results** (Citra game ms, time_avg: 2v2 scene 4.2 `--from 4260`, battle 4.2 `--from 2460`, campaign 8.3; base
main@9dc614c built in a separate worktree):

| step | 2v2 game | 2v2 frame | battle game | campaign game |
|---|---|---|---|---|
| base 9dc614c | 52.9 | 60.0 | 20.0 | 9.9 |
| + native sin/cos, sqrtf (libm.c, fma() calls) | 51.2 | 58.4 | | |
| + inline multiply-add in libm.c | 50.6 | 57.7 | | |
| + fmadd_d in gen/ (commit d1e6d03) | 50.4 | 57.5 | 19.2 | 9.8 |
| same build, `cr_libm_off` | 52.3 | 59.5 | | |
| + non-volatile saves dropped (3f87081) | 43.2 | 50.4 | 17.1 | 8.9 |

2v2 game -18.3%, battle -14.5%, campaign -10% (campaign is at the 60 Hz cap: frame 17.2 unchanged). GX thread
busy unchanged (37.7 in the 2v2). Identity vs base (compare_logs.py exit 0: SDK calls with r3-r6 and lr, pad,
scene, summary lines; framebuffer dumps byte-identical): 2v2 (5269 lines, fb r5000/6500/8000), battle (2690,
r2100/2700/3300), campaign (2939, r1500/1980/2300) and boot to the title (1601 lines, 3000 retraces), at each
step; also the 2v2 with `cr_libm_off`. FNPROF after: frame 64.6 -> 54.6 ms; glue 11.6 -> 10.6 ms (19%),
0x801829d0 3.1 -> 2.4, 0x80060fa4 2.9 -> 1.8, 0x8019fc14 1.55 -> 0.54, 0x8001b544 1.33 -> 0.63, sin + cos
natives 1.3 ms.

**Not done / next:**
- The call glue is still the largest bucket (10.6 ms FNPROF, 19%): passing argument registers as C arguments
  is the redesign named in "Call-boundary glue".
- The 0x8017b000-0x80185000 family (0x801829d0 and neighbours, ~4 ms after) only runs in the 2v2: identify it
  (decomp names) and look at its codegen (float stack round trips: stfs/lfs through r1 with bswap, see
  0x80060fa4).
- Hardware: the hardware gain of dropping the saves is unknown (stores to the guest stack are cheaper on ARM11
  than in Citra's instruction count, but the FPR saves also converted ps0/ps1 to float). A/B: the libm button is
  live; for the saves, build once with `RECOMP_KEEP_NV_SAVES=1`.

## 2v2 game CPU 2 (2026-10-23, not yet on hardware)

Task: make the 2v2 battle (scene 4.2, hardware run 28: 30.5 fps, game CPU 29.7 ms) faster past the previous
session. Citra only: `replay.ps1 -Name 2v2`, `time_avg.py --from 4260` (battle 4.2 `--from 2460`, campaign 8.3).
Base main@8fc8d98 built in a separate worktree; every step run twice on the 2v2 (the two runs gave the same
numbers each time) and once on battle, campaign and boot. Logs: `logs/perf/2026-10-23_*` in the session
worktree (not committed).

**Research (web, ideas only, no code taken).**
- XenonRecomp (hedge-dev, PPC -> C++ for Xbox 360; README "Optimizations",
  https://cdn.jsdelivr.net/gh/hedge-dev/XenonRecomp@main/README.md): every function takes (context, base);
  options keep ctr, xer, cr, the reserved register, non-argument and non-volatile registers as C++ locals (ABI
  assumed). Argument registers r3-r10 stay in the context struct. Its largest reported gain is from the locals
  (savegpr/restgpr calls and redundant context stores gone). No arguments as C parameters, no stack-slot
  promotion. Its `skip_lr` option drops LR handling when the game uses no exceptions.
- ReXGlue (https://mintlify.wiki/rexglue/rexglue-sdk/concepts/recompilation): same (ctx, base) signature,
  locals for the registers a function uses, MMIO base-register tracking (accesses through a register known to
  hold a hardware base use MMIO helpers): the same idea as checking the base register below, the other way
  round. N64Recomp (https://github.com/N64Recomp/N64Recomp): (rdram, ctx), ctx->rN directly.
  So this translator already does more than the published PPC recompilers (per-site dirty spills,
  interprocedural summaries, dropped saves); C-argument passing would be new ground.
- VFP11 (ARM11 FPU), FFmpeg's dsputil_vfp.S comments (LGPL, Siarhei Siamashka;
  https://fuchsia.googlesource.com/third_party/ffmpeg/+/refs/tags/n0.7.7/libavcodec/arm/dsputil_vfp.S): 1-cycle
  throughput for most single-precision ops but not double arithmetic, latency 4 (loads) / 8 (arithmetic),
  separate load/store and arithmetic pipelines; scheduling decides speed. Short-vector mode (FPSCR LEN) queues
  LEN operations into the same pipeline one after another: fewer instructions, not more arithmetic per cycle
  (https://ffmpeg.org/pipermail/ffmpeg-devel/2008-April/040786.html; their vector_fmul ~1.9 cycles per element,
  load/store bound). For the MTX natives a better schedule matters more than vector mode, and Citra cannot show
  either (it counts instructions).
- ARM11 `ldr` + `rev` (big-endian load): a dependency chain, ~6 cycles reported for the unaligned case, `rev`
  needs its operand early (https://ffmpeg.org/pipermail/ffmpeg-devel/2008-July/048658.html). Every guest access
  avoided saves both.
- New 3DS: 4 cores at 804 MHz and the 2 MB L2, enabled by osSetSpeedupEnable (https://3dbrew.org/wiki/Hardware;
  already on here).
- PGO on a bare-metal ARM target needs a way to dump .gcda without constructors or a filesystem
  (https://gcc.gnu.org/pipermail/gcc/2020-November/234164.html); on the 3DS the sdmc exists, but the profile
  would bias the one test build to one scene. Function ordering: lld --symbol-ordering-file
  (https://reviews.llvm.org/D44969), with GNU ld a linker script listing hot `.text.f_*` sections first. Both
  need hardware to measure (I-cache), so not tried.
- -ffast-math sub-flags: only -fno-math-errno leaves values unchanged; -fno-signed-zeros changes x + 0.0 and
  0.0 * x, so it breaks the identity rule.

**Profile** (FNPROF build of the base, 2v2 r4500-8400, 53.2 ms FNPROF frame; `RECOMP_SITEPROF=1` build for
counts): call glue 19.3% (10.3 ms), 0x801829d0 4.5%, GXDrawDone 3.7%, 0x80060fa4 3.3%, PSMTXConcat 3.2%,
0x8006eef8 2.5%, VIWaitForRetrace 2.0%, GXBegin 1.6%, sin + cos + sinf + cosf 3.4%. Per frame: 31.6k calls, 17.4k
translated function entries, 2.5k call_indirect; register traffic at call boundaries (stores before calls 129k,
entry loads ~100k, return stores 62k, reloads at most 82k, many dead and dropped by GCC) is ~1.2 ms of Citra time
at most, so most of the glue is the C call itself (prologue/epilogue, the caller's spills around the bl), not
`cpu` traffic. Half of the entries (8.7k) go to functions of at most 64 instructions, almost all of them in
another gen/ file than the caller (60 functions per file). The SDK natives' union in-set (r1, r3-r10, f1-f8)
costs at most ~15k stores per frame (<0.1 ms). Two runtime paths stood out in the ARM output: every WGPIPE
store (722 sites, immediate-mode vertices in 0x801829d0 / 0x8006eef8 ..) called `recomp_gx_fifo_write` out of
line, and every non-stack access checked and masked its own effective address.

**Steps kept** (Citra game ms; frame = time_avg frame; all identical to the base, see below):

| step | commit | 2v2 game | 2v2 frame | battle game | campaign game |
|---|---|---|---|---|---|
| base main@8fc8d98 | | 43.2 | 50.5 | 17.1 | 8.9 |
| 1 base-register accesses | c27c2f2 | 40.9 | 48.3 | 16.3 | 8.4 |
| 2 WGPIPE stores inline | 0c8c024 | 39.0 | 47.0 | 15.5 | 8.4 |
| 3 SDK call path | 136d224 | 37.4 | 46.3 | 14.6 | 8.2 |
| 4 stack words in locals | 40381bd | 36.0 | 45.9 | 14.1 | 8.0 |

2v2 game -16.7%, battle -17.5%, campaign -10% (campaign stays at the 60 Hz cap). The 2v2 frame moves less: in
Citra the GX thread (busy 38.6 ms, unchanged) is now longer than the game thread (gx wait 0.9 -> 3.6 ms). On
hardware run 28 the GX thread was 18.6 ms against game 29.7, so the game thread still decides there. Estimate,
not measured: scaling run 28's 29.7 ms by Citra's 52.9 -> 36.0 (run 28's build had neither this work nor the
2026-10-22 2v2 work) puts the game thread near 20 ms, close to the GX thread. .text 6.96 -> 6.66 MB.

1. **Base-register accesses** (recomp.py `base_access`, ppc.h `rd*_b/wr*_b/psq_*0_b`): a D-form access
   `rA + k` (rA not r1) checks IS_HW and masks the base register and uses k as the load/store offset, like the
   r1 `_sp` accesses; GCC then checks and masks a pointer once for all the fields read through it (one check per
   struct pointer in a loop body instead of one per field). Differs from `rd32(rA + k)` only if rA + k crosses
   into or out of 0xCC000000-0xCDFFFFFF or rA & RAM_MASK is within 32 KB of either end of the 32 MB mirror:
   ram[] and ram_wr[] now sit inside larger blocks with 32 KB / 32 pages of guard space on both sides
   (cpu.c: `.set ram, recomp_ram_block + 0x8000`), so such an access lands in the guard instead of wrapping.
   Indexed and update forms keep the old helpers. `RECOMP_BASE_ACCESS=0` = old code. On by default.
2. **WGPIPE stores inline** (ppc.h `gx_fifo_put`, gx.c `fifo_lim`): a store to 0xCC008000 appends its bytes
   to the game thread's FIFO in place while fifo_n + size < recomp_fifo_lim (the primitive GXBegin started is
   still incomplete after it, and the buffer has room); the completing store, and every store while the
   primitive size is unknown, still calls `recomp_gx_fifo_write`, which flushes as before. Always on (the same
   bytes reach the same records; no flag).
3. **SDK call path** (ppc.h `recomp_sdk_log_at` / `RECOMP_LOG`, log.c, gx.c): `LOG()` is inline, with the
   name's count slot looked up once per function (a static pointer) instead of count_of's hash probe per call;
   the first 20 calls per name still write the same log line, the ring of the last 32 calls is the same.
   `vtx_size_g` (21 attr_size_in per GXBegin) is cached per vertex format until GXSetVtxDesc /
   GXClearVtxDesc / GXSetVtxAttrFmt change gs.desc / gs.vat. Always on.
4. **Stack words in locals** (recomp.py `promote_stack_slots`, `frame_pointers`; ppc.h `psq_ld0_v`): words
   r1 + k of a frame that only the function itself uses become `u32 sp_s<k>` locals holding the word's bits
   (stw/lwz, stfs/lfs, stfd/lfd as two words, psq with GQR0, the stw/stw/lfd int->double idiom), so MWCC's
   spills and reloads lose the address mask, the byte swaps and the RAM round trip, and GCC can forward them.
   A word qualifies when: the frame is one stwu in the straight-line prologue (after `mflr`/`stw r0, 4(r1)`),
   r1 is otherwise only written by the epilogue `addi r1, r1, size` with no stack access after it, and r1 is
   not used in any other way; 8 <= k and k + 4 <= size; k lies below every frame offset a register derived from
   r1 may point to (forward may-analysis over the function: r1 + k, pointer +/- constants, + an index taken as
   non-negative, update forms; masks and other arithmetic get k - 4096; a register that keeps going down, or
   minus a register, gets no bound; loads are data; after a call r3/r4 may be a pointer argument returned; a
   definition counts as a use, so a pointer passed to a callee or stored counts); no callee reaches that word of
   its caller's frame (stack accesses above a callee's own frame or before its stwu, pointers it derives there,
   through frameless functions transitively, per call target; call_indirect = any frameless function;
   natives in `NATIVE_STACK_READS`, today GXInitTexObjCI's 9th argument at r1 + 8, and recomp.py fails if
   runtime/ reads `R[1] +/-` anywhere else); and every access to it is a whole-word 32-bit, 64-bit or psq access
   of promoted words (byte and halfword accesses keep the word in memory). The local starts as the word's memory
   value right after the stwu (GCC drops the load where a write comes first). 3439 words in 483 functions,
   11085 accesses. Assumption (new): code reaches a local array or struct only from its address upward, as C
   does. The frame memory of promoted words keeps older contents: only a reader of dead stack below r1, or a
   pointer path these rules miss, would see a difference. `RECOMP_STACK_SLOTS=0` = old code. On by default.
   A first version applied the callee exclusion globally and matched no callee at all (a `\b` lost to shell
   escaping): the campaign replay then asserted in AX (`pvoices[...] == NULL`) because 0x8016d5dc kept a
   12-argument call's 10th-12th arguments (its words 12-20, read by 0x80173f64 at its r1 + 172..180) in locals.
   The identity runs caught it. Lesson: a regex written through a shell heredoc can lose its `\b` (it became a
   backspace character); check new translator code for control characters before trusting a zero count.

**Tried and not kept:**
- Inline copies (`static inline fi_` copies of every translated function of at most 32 instructions in each
  gen/ file that calls it, so GCC can inline across files: 756 copies, 3509 call sites; GCC inlined ~2580 of
  them, including the hot 0x8018758c / 0x8018a28c / 0x801690c8): 2v2 game 37.4 -> 37.2 (-0.5%) for +5.5%
  ARM instructions. Small callees are cheap to call already; the glue is in larger functions' calls.
- Per-native in-sets instead of the union (r1, r3-r10, f1-f8): sized from the site counts at < 0.1 ms, not built.
- LR save/restore removal (XenonRecomp's skip_lr): ~6 instructions per framed call, ~12k per 2v2 frame,
  estimated 0.3 ms (0.7%), and SDK log lines show lr after tail calls; not built.

**Identity** (`compare_logs.py` exit 0: SDK calls with r3-r6 and lr, pad, scene, summary lines; framebuffer
dumps byte-identical) against base 8fc8d98 after every kept step: 2v2 (5269 lines, fb r5000/6500/8000), battle
(2690, r2100/2700/3300), campaign (2939, r1500/1980/2300) and boot (1581 lines).

**Profile after** (FNPROF build of 40381bd, same windows): FNPROF frame 53.2 -> 46.9 ms, game 46.0 -> 38.9. Call
glue 21.8% (10.2 ms; was 10.3), GXDrawDone 6.1% (2.9 ms: waiting for the GX thread), PSMTXConcat 3.7%,
0x80060fa4 3.3 -> 2.3%, VIWaitForRetrace 2.3%, 0x801829d0 4.5 -> 2.1% (WGPIPE path), 0x8005d08c 1.8%, cos 1.5%,
PSMTXMultVec 1.4%, GXLoadPosMtxImm 1.2%, GXBegin 1.6 -> 1.2%, sin 1.2%. Game code is flat (no function above
2.3%); the glue is now a larger share of a smaller frame.

**Not done / next (ranked):**
1. Hardware run: 2v2 game ms against run 28 (29.7) with this branch plus the 2026-10-22 2v2 work; also check the
   GX thread (busy 18.6 ms in run 28): with the game thread near 20 ms it is the next limit.
2. Stack words between address-taken locals: most hot frames (0x8017b4a4, 0x801835b0, 0x8018144c) keep their
   spills because a local struct sits at r1 + 8..24 below them. Promoting those needs the extent each callee
   reads through a pointer argument (an interprocedural "bytes reached through r3-r10" summary, plus a table for
   the natives that take pointers), then words outside every passed object qualify.
3. Call glue (still the largest bucket): C arguments for r3-r10/f1-f8 would remove the `cpu` traffic (~1.2 ms
   Citra at most) but not the C call itself; inlining larger hot callees (64-200 instructions, 4.8k entries per
   frame) needs a size/benefit choice GCC's inliner does not make here (see inline copies).
4. MTX natives (PSMTXConcat 991 calls, ~1.7 ms FNPROF): ldm + rev + `vmov d, r, r` pairs and an interleaved
   schedule; bit-identical by construction, measurable only on hardware (`cr_bench`).
5. I-cache layout (hot functions first) and PGO: hardware only; PGO would need to be the main build.

## Run 30 text hang: not the audio skip (2026-10-25, open)

Hardware run 30 (`logs/cr_autotest_2026-10-25_hw-run30-t06-text-hang.log`, `logs/hw-run30-shots/`, build of
2v2-perf-2): story event `snr_t_06` (read r14274), the "Awex!" box after A at r14975 shows no text, A/B do
nothing, no voice is started again (`voices started 572` from ~r14660 to the end). A Rosalina pause at r14951
(4274 ms, game thread waiting on the GX thread, GPU held) made real-time pacing skip 807 AX frames. The guess was
that the skipped frames lost AX callbacks or voice state the event waits on. **That is not it:**

- Repro (Citra, this branch = main 1a78d4d + `sdmc:/cr_ax_stall.txt`): `tools/citra/replay.ps1 -Name boot -Save
  data\saves\2026-10-25_hw-freeze -RecLog <rec> -RunLen 3600 -Flags cr_audio_rt_on`, stall file written to
  Citra's sdmc by hand. The route loads `snr_t_06` at r1222; the same boxes as on hardware: the "???" box
  ("Rrrriiinnnggg!", portrait texture 80ac2640) at r1615 (hw r14636), "Awex!" (80b64ee0) one frame after the next
  A (hw r14977). Rec logs: `rec_t06.log` (hw pad lines to r1200, then A every 60) and `rec_t06_hwpad.log` (no input
  until r1928, then hw's Rosalina pad lines: L on p1, L+Down on p2 while SELECT was held, stall at r1930, A at r1954
  = 24 frames later, as on hw). Logs + framebuffer dumps in `logs/perf/2026-10-25_t06-stall/` (main checkout).
- All of these play through, "Awex!" types "...Hm? Who could be calling me this early in the morning?" with blips
  and the event goes on (voices started 95-101 at AX frame 12000, same as without a stall):
  4300 ms added to the pacing clock at r1700 (815 frames skipped); a real 4300 ms sleep of the game thread at r1700
  (823 skipped, OS time jumps too); the same at r1930 with hardware pad timing; and with the Rosalina pad lines.
  Skipped AX frames are frames not mixed and callbacks not run, nothing is half-done: voices do not advance in
  them, so the sound driver sees time stand still, as if the DSP had paused.
- The run 30 screenshot taken *during* the stall (19-19-51.339_top) already shows a broken box: "?" instead of
  "???", and "iiinnnggg!" with even ~10 px glyph advances, the first four glyphs ("Rrrr") missing and the right
  end where Citra's "Rrrriiinnnggg!" ends. Citra (r1625-1925 dumps) shows the box static and left-aligned in every
  frame, so this is not a text effect. On hardware the voice count also stopped near r14660 (estimated from where the
  `# AX: frame 51000` line falls; ~4 of Citra's 7 blips for that box), about 300 frames before the stall. So the fault was there before the Rosalina pause; the user
  most likely paused to take the screenshot because of it.
- Not reproduced in Citra with or without a stall, so it depends on hardware timing or on the 14000-frame session
  before it (2v2 battles, a story save at ~r13900). Candidates not yet checked: guest RAM written late or not
  tracked on hardware only (the session had seven ~5 s SD stalls on `cubog21.hps` 0x20-byte reads whose data
  landed 300 frames after the game went on: `DVD late` lines; RAM a read lands in may have been reused by then),
  and the texture/display-list caches keeping a stale glyph sheet or text list (write tracking). Next: a hardware
  run that dumps guest RAM (font/text buffers) or the framebuffer when the box breaks.

`sdmc:/cr_ax_stall.txt` = "<retrace> <ms> [sleep]" (Citra test flag, no button): injects such a stall
(`runtime/ax.c`). Logs "# AX: injected stall at r<n>" at boot and "# AX r<n>: injected stall" when it fires.
Without the file, 2v2 (5359 key lines), battle (2690), campaign (2939) and boot (1581) replays and their fb dumps
are identical to main 1a78d4d (`compare_logs.py` exit 0).

**Memory dumps** (`runtime/memdump.c`), to catch the next broken box on hardware without a PC attached: the
bottom-screen button "Dump RAM" (action button: TAP, "<n> done") or `sdmc:/cr_memdump.txt` (retraces, ascending,
one per line: the Citra side of a comparison) writes, at the retrace after the GX thread synced:
`sdmc:/cr_dump_r<n>_ram.bin` (guest 0x80000000 + 0x1808000: 24 MB and the AX voices above it), `_aram.bin`
(16 MB), `cr_dump_r<n>.txt` (build, retrace, GX frame, scene struct, MSR, DVD reads still in flight with file,
offset, length, destination and whether they are late) and the next GX frame as `cr_fb_<frame>.bmp`; log line
"# memdump r<n> (button|cr_memdump.txt): ... <ms>". Citra check (t06 route, r1900): files written, RAM at
0x80003100 equals main.dol's text0. `tools/hw_dump.sh` (gdb through Rosalina's debugger at 192.168.1.129:4000)
is the attached alternative: thread registers and backtraces, but needs a PC online at that moment (not yet run).

## 2v2 particle family: HSD particle system profile (2026-10-25, investigation only)

Task: identify the 0x8017b000-0x80185000 code family that the earlier 2v2 FNPROF passes flagged, measure it on the
current code, and find which of its costs are repeated work and which are translation cost. No optimisation here.
Base: main@ad99a97 (all of "2v2 game CPU" and "2v2 game CPU 2" in). Citra only: `replay.ps1 -Name 2v2` (window
r4500-8400, scene 4.2) and `-Name battle` (1v1 with idle robots, r2700-3600). Logs (not committed) and the
temporary probe sources are in the session notes folder `2v2-family-probe/` (probe-runtime-hooks.patch, probe.c,
wrap.py, pcnt-ppc-hooks.patch, pcnt.c).

**What it is.** The family is HAL's sysdolphin particle system. The DOL has its source file names: "particle.c",
"psinterpret.c", "generator.c", "psdisp.c" and "pslist.c" (0x8024b068-0x8024b490), "Particle:setBlendMode:Unknown
mode" (printed by 0x8018144c) and the pslist asserts "activeParticle[pp->linkNo] == pp" and "linkNo >= 0 && linkNo <
PS_NUM_LINK". 16 links, active lists at 0x80449c58. Names are in `tools/decomp_names.tsv`:
- update, once per frame from ps_update_frame (0x80075c14): ps_interpret_all (0x8017e79c, "psinterpret.c") runs
  ps_interpret (0x8017b4a4, 3020 instructions, recursive) for every active particle, and ps_generators_update
  (0x80180728) runs ps_generator_update (0x8017f4b8, "generator.c") for every generator.
- draw: ps_draw_frame (0x80075c54) calls ps_disp (0x8018144c, psdisp.c) for passes 0-2. Pass 0 only ticks a
  frame-counter byte; passes 1 and 2 walk the two lists that ps_list_get (0x80186158) returns for each link. For
  each particle it calls ps_disp_tev_setup (0x80185310), ps_disp_tev_color (0x80184b44), ps_disp_chan_color
  (0x80184f60), then ps_disp_particle_a (0x801822d0) or ps_disp_particle_b (0x801835b0, particles with an
  attached object at pp+0x90). Both end in ps_disp_quad (0x801829d0), which draws the particle as its own
  GX_QUADS primitive: GXClearVtxDesc, 2-3 GXSetVtxDesc, GXBegin and 4 vertices (f32 x3, rgba8, u8 texture index)
  written to WGPIPE.
- spawn: 0x8017ee20, reached through 0x800750f8 from pod_update, bomb_update, gun_bullets_update_hit,
  gun_fire_volleys and robot_physics_update.

**Why the 2v2 runs it more.** It scales with live particles, not with robots or cameras: 186 particles are drawn
per frame in the 2v2 replay against 10 in the 1v1 replay, where the robots stand idle and nobody fires. The 2v2 is a
real fight with four robots shooting. Per frame, the 2v2 interprets 188 particles at the top level (201 ps_interpret
entries) and draws 186, so it processes no inactive particles. Any battle with as much shooting would cost the same.
Correction to "2v2 game CPU": the "cos/sin x4 per call" there for 0x801829d0 came from its static call list. At
run time its sin/cos/tan paths (pp+0x8c / pp+0x90 set) are not taken in either replay (RECOMP_SITEPROF shows no
call), and the "effects or particles" guess is now confirmed as particles.

**Measured** (2v2 vs 1v1). Self ms from `make FNPROF=1` (PROF_TOP raised to 300 for the run): FNPROF frame
48.1 ms (game 39.9) in the 2v2, 18.7 ms (16.0) in the 1v1. Calls per frame from `RECOMP_SITEPROF=1`. In the
brief, the stale figures were 3.1 / 1.0 / 0.9 / 0.5 / 0.45 ms (before the 2026-10-22/23 work).

| function | 2v2 self ms | share | 1v1 ms | calls/frame 2v2 | 1v1 | stack / base accesses per call (2v2) |
|---|---|---|---|---|---|---|
| 0x801829d0 ps_disp_quad | 1.09 | 2.26% | 0.05 | 186 | 10 | 16 / 11 |
| 0x801835b0 ps_disp_particle_b | 0.60 | 1.24% | 0.06 | 102 | 8.3 | 46 / 39 |
| 0x8017b4a4 ps_interpret | 0.55 | 1.14% | 0.04 | 201 | 8.9 | 6 / 47 |
| 0x8018144c ps_disp | 0.44 | 0.92% | 0.06 | 3 | 3.4 | 401 / 1656 (per pass) |
| 0x80184b44 ps_disp_tev_color | 0.32 | 0.66% | 0.01 | 186 | 10 | 16 / 23 |
| 0x801822d0 ps_disp_particle_a | 0.24 | 0.49% | 0.01 | 84 | 1.7 | 17 / 17 |
| ps_generators_update, ps_list_get, ps_interpret_all, ps_disp_chan_color, 0x8017a248, ps_generator_update, ps_disp_tev_setup | 0.04-0.07 each, 0.38 together | | <0.02 | 1-186 | | |
| family self total | ~3.6 | 7.5% | ~0.27 | | | |

Inclusive (a timing-only build: svcGetSystemTick around the entry points and nothing else; Citra 2v2 game
37.6 ms in the window, 1v1 15.0): **ps_draw_frame 5.56 ms (14.8% of the game thread), ps_generators_update 0.86,
ps_interpret_all 0.58, spawn 0.01: the particle system is 7.0 ms of the 2v2 game thread (18.6%)**, against 0.47 ms
in the 1v1. Where it goes (FNPROF ms, SDK and glue shares apportioned by call counts, so an estimate): self 3.6;
SDK work on the family's behalf ~1.9 (GXBegin 0.31, PSMTXMultVec 0.32, sinf/cosf inside PSMTXRotAxisRad 0.32,
GXSetVtxDesc 0.21, PSMTXCopy 0.15, GXClearVtxDesc 0.10, sin/cos 0.10, GXSetTevColor 0.09, texture init/load 0.14,
atan2 + PSMTXRotAxisRad bodies 0.14); call glue ~1.0 (3755 of the 31.6k calls per frame and 1302 of the 17.4k
entries come from the family). Assumption: the game thread does not block on a full GX queue inside ps_disp (1 MB
queue). Hardware share not measured: if it is the same 18.6% of run 29's 22.2 ms, that is ~4 ms.

**Repeated inputs** (temporary probe build: per frame, the calls from code at lr 0x8017a000-0x80187000 whose inputs
had already been seen in the same frame; GX state was compared with the runtime's current state):

| what | calls/frame | repeats | inputs / what would invalidate a cached result |
|---|---|---|---|
| GXBegin (ps_disp_quad) | 209 | 183 (87%) had the same vertex descriptor and format as the previous GXBegin | gs.desc + format; the GXClearVtxDesc (209) and GXSetVtxDesc (477) calls before them change nothing in net, but each one sets `vsz_g_ok = 0` and queues a GX-thread record |
| PSMTXMultVec | 424 | 211 identical (matrix, vector) pairs, 21 distinct matrices | 194 of the repeats come from the two calls at 0x801838cc / 0x801838f8 in ps_disp_particle_b (97 of 100 each). Their inputs are the matrix at 0x80449bf4, the translation at 0x80449b78 + 0x0c/0x1c/0x2c and pp+0x90's +0x24/+0x28, so they are the same for every particle of one attached object. Invalidated by the camera matrix or that object changing |
| PSMTXRotAxisRad | 158 | 22 (14%) | axis (12 bytes at r4) + angle |
| atan2 | 90 | 17 | f1, f2 |
| sin/cos (mostly ps_generator_update) | 132 | 64 | the argument |
| GXSetTevColor | 121 | 0 (the colour differs from the register's previous value every time) | |
| GXLoadTexObj / GXInitTexObj | 51 / 51 | 0 same object already on the map / 38 same arguments | |
| PSMTXConcat | 12 | | no per-child concatenation of a parent matrix |

Outside the family (found by the same probe, game-wide): 2384 sin/cos/sinf/cosf per frame, of which **1501 (63%)
repeat an argument of the same function already computed in that frame**. ~1130 of the repeats are in 0x8019fc14
(angles -> rotation: 6 sin/cos call sites, 273 calls each per frame; 263 of its 273 calls come from 0x80196630, not identified), 183 in PSMTXRotRad
(0x80139800, 115 calls) and 66 in PSMTXRotAxisRad.

**Translation cost** (FNPROF build with temporary counters in ppc.h `sp_at`/`b_at`/plain rd/wr, keyed on the
running function): per 2v2 frame the family runs 16.4k stack accesses that stayed in guest memory, 30.6k
base-register accesses and 1.5k others. The whole game runs 126.3k / 320.2k / 22.5k. With `RECOMP_STACK_SLOTS=0`
the game runs 153.6k stack accesses, so stack-slot promotion removed 27.3k executed accesses per frame for its
measured 1.4 ms (37.4 -> 36.0), ~0.05 us of Citra game time each (GCC's forwarding included). In the family
promotion gains nothing: 15.5k accesses without it, 16.4k with it (entry loads of promoted words). Almost every
accessed frame word lies above an address-taken local, which the current rule excludes: 57 of 61 offsets in
ps_disp_particle_b sit at or above r1+0x18 (vec3 buffers passed to PSMTXMultVec), 32 of 38 in ps_disp_quad (r1+0x14
passed to 0x8019fc14), 33 of 37 in ps_disp_particle_a, 34 of 37 in ps_disp, 73 of 78 in ps_generator_update.
ps_disp_tev_color passes a GXColor in r1+0x24-0x2f by address and also accesses it by byte. ARM code is 4-8 times the
PPC size (ps_interpret 3020 -> 24632 words, ps_disp_quad 760 -> 4230).

**Opportunities, ranked** (estimated Citra 2v2 game-thread savings; none built):
1. *Pointer-extent stack promotion* (translator; next session as planned): per callee, how many bytes it reaches
   through r3-r10, plus a table for the natives (PSMTXMultVec reads 12 bytes at r4 and writes 12 at r5, PSMTXCopy
   48, GXSetTevColor's colour 4). In the family, 16.4k accesses per frame; promoting 50-80% of them is ~0.4-0.65 ms
   (1-2%). The same rule applies to the 126k accesses left game-wide, which is the larger prize (unmeasured: what
   share qualifies).
2. *Lazy vertex descriptor* (runtime gx.c, SDK layer): only invalidate the vertex size and queue a descriptor
   record when the descriptor at GXBegin differs from the last one sent. 87% of particle GXBegins would skip it,
   and so would part of the 775 GXSetVtxDesc calls game-wide. ~0.3-0.5 ms game thread plus ~690 fewer GX-queue
   records per frame. Exact (same records reach the GX thread whenever the state differs). Not in the planned
   sequence.
3. *Memo for repeated sin/cos* (runtime natives, exact since sin/cos are pure; but a hit must also reproduce the
   registers the native leaves, see "2v2 game CPU" 1): 1501 repeats per frame at ~0.75 us FNPROF each, minus the
   lookup, ~0.5-0.9 ms. Mostly outside this family (0x8019fc14 via 0x80196630).
4. *Selective inlining* (session 4): the per-particle helpers ps_disp_tev_setup / tev_color / chan_color (3 x 186
   entries) and ps_disp_quad (186): ~750 calls per frame, ~0.25 ms of glue at the measured ~0.33 us FNPROF per call.
5. *Redundant math inside the family* (planned session 3): the confirmed repeats are the 194 PSMTXMultVec at
   0x801838cc/f8 (~0.15-0.2 ms with glue) and <0.1 ms of rotaxis/atan2/sin/cos. Removing them would mean changing
   game code (hoisting per-object work out of the per-particle loop), which the rules forbid. A memo inside the
   PSMTXMultVec native costs about what the multiply costs. Not worth a session.
6. *GX thread, not the game thread*: in the 2v2 the GX thread's immediate-mode draws take 6.2-6.3 ms against 1.2 ms
   in the 1v1, and draw calls are 403-449 against 180. Each particle quad has its own TEV colour, so it looks like
   one C3D draw per particle (an inference, not split out). Once the game thread drops below the GX thread, merging
   particle quads that differ only in TEV colour (colour as a vertex attribute) is the matching win there.

**Recommendation:** do the pointer-extent stack promotion (session 2) first. It is the largest item that only the
translator can deliver, and it applies game-wide. Replace session 3 (redundant math in this family) with the
lazy vertex descriptor (2) and the sin/cos memo study (3): the family's own confirmed redundancies are small and
are game logic.

## Pointer-extent stack promotion (2026-10-26, off by default, not on hardware)

Task: let `promote_stack_slots` ("2v2 game CPU 2", step 4) promote frame words that sit above an address-taken
local, by knowing how many bytes each pointer can reach. Base: branch 2v2-family-probe @39c4989. Citra only, same
replays as before (`replay.ps1 -Name 2v2|battle|campaign|boot`, `time_avg.py --from 4260 / 2460 / 1500`).

**What it does** (recomp.py `ptr_flow`, `ptr_summaries`, `native_arg_reads`, `native_ptr_ext`; on with
`RECOMP_PTR_EXTENT=1` at generation, default off). The old rule excluded every word at or above the lowest
r1-derived address the function computes (a definition counted as a use). The new one is a forward may-analysis
per function. A register's value is a set of offset intervals per origin: origin 0 is the frame (r1 after the
stwu, or at the entry when frameless), and origins 3-12 are the entry values of r3-r12 (r11/r12 because MWCC
helpers such as the context save at 0x801ae9b4 take them). Uses give byte ranges:
- `rN + k` / ea accesses give their bytes.
- An indexed access, a store of the pointer, a `call_indirect` or any other use gives [lo + min(k, 0), INF): the
  old upward assumption.
- A call gives the callee's per-register reach. A frame word is promoted when no range covers it (plus every
  earlier rule).

Callee summaries (bytes reached through each of r3-r12, and the argument-derived values left in r3/r4) come from a
fixpoint over the call graph: Tarjan SCCs, callees first; summaries only grow and are widened after 8 rounds.
Natives:
- A native reaches only the registers its runtime/ code reads (`native_arg_reads` parses runtime/*.c; LOG/printing
  excluded, `gq_regs(fn, n)` = r3..r3+n-1, computed index = INDEX_READS or all, calling guest code = all).
- `tools/recomp/native_ptr_ext.tsv` classifies those registers: value / ptr N / unknown, with file:line evidence.
  It covers 136 natives, 354 registers (228 value, 76 ptr N, 50 unknown). A Sonnet subagent wrote it and I
  spot-checked it (GXLoadNrmMtxImm 44, GXSetProjection 48, the colour setters 4). Natives with a ret row (memcpy,
  memset, mem_zero, OSInitAlloc) keep the default rule.
- MTX/MSL natives use their translated body's summary.

After a call registers keep their values (a callee may leave them), so a stale frame pointer in r4 still counts
at a later native that reads r4. New assumption: a function never returns the address of its own frame (C:
locals are dead after the return). 197 functions do leave a frame pointer in r3/r4 at a return, all stale
arguments. For frameless functions it is an escape into the caller's frame. `RECOMP_PTR_REPORT=addr,..` prints
per-frame detail, and `RECOMP_PTR_RANGE=lo-hi` applies the new rule only to functions in [lo, hi) (bisecting).

**Measured.**

| | old rule (= default) | RECOMP_PTR_EXTENT=1 |
|---|---|---|
| promoted words / functions / accesses (static) | 3439 / 483 / 11085 | 4578 / 584 / 14333 |
| words free of every other rule but kept for frame pointers | 3806 | 3277 |
| frames with words a callee reaches | 533 | 429 |
| ps_interpret 0x8017b4a4 / ps_disp_particle_b 0x801835b0 / ps_disp 0x8018144c words | 0 / 0 / 0 | 0 / 4 / 2 |
| .text | 6,534,856 | 6,486,888 (-0.7%) |
| Citra 2v2 game ms (2 runs) | 36.0 | 35.6, 35.6 (-1.1%) |
| Citra battle 4.2 / campaign 8.3 game ms | 14.0 / 8.0 | 13.9 / 7.9 |

Frame ms did not change (2v2 46.0 -> 45.9; in Citra the GX thread decides it). **Identity**: `compare_logs.py`
exit 0 for 2v2, battle, campaign and boot, with the SDK/scene/pad lines and all fb dumps identical to 39c4989.
With the switch off (the default), gen/ is byte-identical to 39c4989's.

**Why the gain is small.** In the particle family the words left in memory are the address-taken buffers
themselves, not words beside them.
- In ps_disp_particle_b the vec3/matrix buffers at r1+24..216 really are passed to PSMTXMultVec/Copy/Concat. Their
  ranges touch and merge, and the 30 remaining words all lie inside them.
- ps_disp reaches OSReport (varargs: every register it reads is unknown) and the texture/TLUT natives from r1+24.
- ps_interpret has a `call_indirect` (unknown from r1+16) and callee reach into its frame.

Game-wide, the largest remaining causes of an unbounded range are:
- indexed accesses to local arrays (102 functions);
- the assert handler 0x801aea4c (73 functions; it calls OSReport and the context save with every register);
- stores of a frame pointer (37);
- `call_indirect` (36);
- sprintf 0x80163710 (28).

Making those precise (a format-aware OSReport/sprintf extent, bounding a local array's index) would add static
words. Static counts did not predict much time, though: +33% words gave -1.1%. Below the 2% bar (as with LTO and
the inline copies), so it is off by default. `RECOMP_PTR_EXTENT=1` keeps it available; it also shrinks .text.

**Bugs found on the way** (each caught by the boot replay or a differential check, all fixed before the numbers
above):
1. A `\b` in the callee regex was lost to a shell heredoc and became a backspace, so the call graph was empty.
   Summaries were then computed in arbitrary order and were stale: 0x8019cc64 promoted a struct it passes to
   0x8019def4, and boot hung loading files. A function-range bisection (`RECOMP_PTR_RANGE`, 8 steps) found it.
   recomp.py now refuses to run if its own source contains a control character. The same trap was noted in
   "2v2 game CPU 2", so the guard is overdue.
2. Inlined leaves start with a `/* inlined */` comment, and their first assignment was not parsed. Unparsed
   assignments now stop the translator.
3. `rD = rS + rX` kept only rS's origins (found by checking that every register the old analysis tracks as a frame
   pointer is tracked by the new one at the same address: 0 mismatches after the fix).
4. Blanking an indexed access hid its index register.
5. The fixpoint oscillated on the recursive 0x80160c48: a recomputed summary can be smaller than the widened one.

<!-- generated -->
## Generated by tools/recomp/recomp.py

Functions translated: 2742. SDK stubs: 1053. MetroTRK entries skipped: 184. Jump tables as switch: 52.

### flow into non-translated code that is not a known entry (1)

0x801aea48 -> 0x801346c8

### fres/frsqrte estimate (needs Dolphin table) (100)

0x800082ec frsqrte, 0x8001b5dc frsqrte, 0x8001b824 frsqrte, 0x8001c03c frsqrte, 0x8001c678 frsqrte, 0x8001c79c frsqrte, 0x8001cbb4 frsqrte, 0x80043848 frsqrte, 0x80043980 frsqrte, 0x80048c4c frsqrte, 0x8004a69c frsqrte, 0x8004cbc4 frsqrte, 0x8005fdcc frsqrte, 0x8006bab4 frsqrte, 0x8006c2c8 frsqrte, 0x8006c468 frsqrte, 0x8006d2b8 frsqrte, 0x8006d778 frsqrte, 0x80076d00 frsqrte, 0x80076e00 frsqrte, 0x800979fc frsqrte, 0x80097e58 frsqrte, 0x800e5604 frsqrte, 0x800e5734 frsqrte, 0x801396b4 fres, 0x801397ac fres, 0x80139950 frsqrte, 0x80139ac8 fres, 0x8013a1e0 frsqrte, 0x8013a244 frsqrte, 0x80140a88 frsqrte, 0x8016d8bc frsqrte, 0x8016d924 frsqrte, 0x8016d97c frsqrte, 0x8016d9e8 frsqrte, 0x8016ddc8 frsqrte, 0x8016de30 frsqrte, 0x8016de88 frsqrte, 0x8016def4 frsqrte, 0x8016f700 frsqrte, 0x8016f768 frsqrte, 0x8016f7c0 frsqrte, 0x8016f82c frsqrte, 0x801707f8 frsqrte, 0x801708b8 frsqrte, 0x8017094c frsqrte, 0x80170a10 frsqrte, 0x80179a14 frsqrte, 0x8017ae10 frsqrte, 0x8017b080 frsqrte, 0x8017b26c frsqrte, 0x8017b2d8 frsqrte, 0x8017e664 frsqrte, 0x8017f56c frsqrte, 0x8017fb84 frsqrte, 0x80180404 frsqrte, 0x80181094 frsqrte, 0x80181104 frsqrte, 0x80183060 frsqrte, 0x801830e8 frsqrte, 0x801909f0 frsqrte, 0x80190a8c frsqrte, 0x80191140 frsqrte, 0x801911dc frsqrte, 0x80199ec4 frsqrte, 0x80199f44 frsqrte, 0x80199ffc frsqrte, 0x8019a068 frsqrte, 0x8019a144 frsqrte, 0x8019a3d0 frsqrte, 0x8019a4c0 frsqrte, 0x8019a6d0 frsqrte, 0x8019a900 frsqrte, 0x8019b578 frsqrte, 0x8019b5f0 frsqrte, 0x8019b860 frsqrte, 0x8019b8d8 frsqrte, 0x8019b944 frsqrte, 0x8019bb4c frsqrte, 0x8019bbc4 frsqrte, 0x8019bd24 frsqrte, 0x8019becc frsqrte, 0x8019bf44 frsqrte, 0x8019bfbc frsqrte, 0x8019e694 frsqrte, 0x8019e748 frsqrte, 0x8019e804 frsqrte, 0x8019e8ac frsqrte, 0x8019efe4 frsqrte, 0x8019f068 frsqrte, 0x8019f0ec frsqrte, 0x8019f65c frsqrte, 0x8019f728 frsqrte, 0x8019f814 frsqrte, 0x8019f938 frsqrte, 0x8019f9e0 frsqrte, 0x801a1290 frsqrte, 0x801a1310 frsqrte, 0x801a19bc frsqrte, 0x801a66ac frsqrte

### invalid instruction reached (2)

0x8000554c 000024d0, 0x800055b4 001fe890

### jump table base not lis/addi (2)

0x8009af38, 0x800a7cec

### jump table entry outside translated text (2)

0x8000e6cc table 0x801b6748, 0x80010774 table 0x80329b10

### jump table without cmplwi bound (not translated as switch) (1)

0x8000f414 table 0x801e64fc

### mtlr from a non-stack value (blr treated as return) (3)

0x800031f0, 0x8000341c, 0x8013938c

### system SPR access in translated code (4)

0x801ae9b4 mtspr 272, 0x801ae9c4 mfspr 272, 0x801aea2c mfspr 26, 0x801aea34 mfspr 27

### system instruction in translated code (1)

0x80003408 mtmsr

### valid code not reached from any function start (628)

0x800034b0, 0x800034b4, 0x800034b8, 0x800034bc, 0x800034c0, 0x800034c4, 0x800034c8, 0x800034cc, 0x800034d0, 0x800034d4, 0x800034d8, 0x800035ac, 0x800036ac, 0x800036b0, 0x800036b4, 0x800036b8, 0x800036bc, 0x800036c0, 0x800036c4, 0x800036c8, 0x800036cc, 0x800036d0, 0x800036d4, 0x800036d8, 0x800036dc, 0x800036e0, 0x800036e4, 0x800036e8, 0x800036ec, 0x800036f0, 0x800036f4, 0x800037ac, 0x800037b0, 0x800037b4, 0x800037b8, 0x800037bc, 0x800037c0, 0x800037c4, 0x800037c8, 0x800037cc, 0x800037d0, 0x800037d4, 0x800037d8, 0x800037dc, 0x800038ac, 0x800038b0, 0x800038b4, 0x800038b8, 0x800038bc, 0x800038c0, 0x800038c4, 0x800038c8, 0x800038cc, 0x800038d0, 0x800038d4, 0x800038d8, 0x800038dc, 0x800039ac, 0x800039b0, 0x800039b4, 0x800039b8, 0x800039bc, 0x800039c0, 0x800039c4, 0x800039c8, 0x800039cc, 0x800039d0, 0x800039d4, 0x800039d8, 0x800039dc, 0x80003aac, 0x80003ab0, 0x80003ab4, 0x80003ab8, 0x80003abc, 0x80003ac0, 0x80003ac4, 0x80003ac8, 0x80003acc, 0x80003ad0, 0x80003ad4, 0x80003ad8, 0x80003adc, 0x80003bac, 0x80003bb0, 0x80003bb4, 0x80003bb8, 0x80003bbc, 0x80003bc0, 0x80003bc4, 0x80003bc8, 0x80003bcc, 0x80003bd0, 0x80003bd4, 0x80003bd8, 0x80003bdc, 0x80003cac, 0x80003cb0, 0x80003cb4, 0x80003cb8, 0x80003cbc, 0x80003cc0, 0x80003cc4, 0x80003cc8, 0x80003ccc, 0x80003cd0, 0x80003cd4, 0x80003cd8, 0x80003cdc, 0x80003dac, 0x80003db0, 0x80003db4, 0x80003db8, 0x80003dbc, 0x80003dc0, 0x80003dc4, 0x80003dc8, 0x80003dcc, 0x80003dd0, 0x80003dd4, 0x80003dd8, 0x80003ddc, 0x800040ac, 0x800040b0, 0x800040b4, 0x800040b8, 0x800040bc, 0x800040c0, 0x800040c4, 0x800040c8, 0x800040cc, 0x800040d0, 0x800040d4, 0x800040d8, 0x800040dc, 0x800041ac, 0x800041b0, 0x800041b4, 0x800041b8, 0x800041bc, 0x800041c0, 0x800041c4, 0x800041c8, 0x800041cc, 0x800041d0, 0x800041d4, 0x800041d8, 0x800041dc, 0x800042ac, 0x800042b0, 0x800042b4, 0x800042b8, 0x800042bc, 0x800042c0, 0x800042c4, 0x800042c8, 0x800042cc, 0x800042d0, 0x800042d4, 0x800042d8, 0x800042dc, 0x800043ac, 0x800043cc, 0x800043d0, 0x800043d4, 0x800043d8, 0x800043dc, 0x800043e0, 0x800043e4, 0x800043e8, 0x800043ec, 0x800043f0, 0x800043f4, 0x800043f8, 0x800043fc, 0x80004400, 0x80004404, 0x80004408, 0x8000440c, 0x80004410, 0x80004414, 0x80004418, 0x8000441c, 0x80004420, 0x80004424, 0x80004428, 0x8000442c, 0x80004430, 0x800044ac, 0x800044b0, 0x800044b4, 0x800044b8, 0x800044bc, 0x800044c0, 0x800044c4, 0x800044c8, 0x800044cc, 0x800044d0, 0x800044d4, 0x800044d8 ...

