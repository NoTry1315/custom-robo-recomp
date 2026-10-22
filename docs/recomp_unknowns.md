# Recompiler unknowns

Things the translator does not decide. These are not guesses: each one is either
logged here with addresses or turned into a runtime call that reports it
(`recomp_bad`). Fix them against Dolphin, not by changing game logic.

## Semantics decisions (follow Dolphin's interpreter; to verify in phase 7)

- Single-precision ops round the double result to float (`fsgl`). The C operand of
  fmuls/fmadds/ps_mul* is first rounded to a 25-bit mantissa (`f25`), as Dolphin does.
- fmadd family uses C `fma()` then rounds to single (double rounding, same as
  Dolphin). It is slow on 3DS because VFPv2 has no fused multiply-add.
- The ARM FPSCR is not configured yet. If libctru enables flush-to-zero or default-NaN
  ("RunFast") mode, denormals and NaN payloads differ from Gekko.
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

## Vertex shader variants (2026-10-22)

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

<!-- generated -->
## Generated by tools/recomp/recomp.py

Functions translated: 2743. SDK stubs: 1048. MetroTRK entries skipped: 184. Jump tables as switch: 52.

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

### valid code not reached from any function start (621)

0x800034b0, 0x800034b4, 0x800034b8, 0x800034bc, 0x800034c0, 0x800034c4, 0x800034c8, 0x800034cc, 0x800034d0, 0x800034d4, 0x800034d8, 0x800035ac, 0x800036ac, 0x800036b0, 0x800036b4, 0x800036b8, 0x800036bc, 0x800036c0, 0x800036c4, 0x800036c8, 0x800036cc, 0x800036d0, 0x800036d4, 0x800036d8, 0x800036dc, 0x800036e0, 0x800036e4, 0x800036e8, 0x800036ec, 0x800036f0, 0x800036f4, 0x800037ac, 0x800037b0, 0x800037b4, 0x800037b8, 0x800037bc, 0x800037c0, 0x800037c4, 0x800037c8, 0x800037cc, 0x800037d0, 0x800037d4, 0x800037d8, 0x800037dc, 0x800038ac, 0x800038b0, 0x800038b4, 0x800038b8, 0x800038bc, 0x800038c0, 0x800038c4, 0x800038c8, 0x800038cc, 0x800038d0, 0x800038d4, 0x800038d8, 0x800038dc, 0x800039ac, 0x800039b0, 0x800039b4, 0x800039b8, 0x800039bc, 0x800039c0, 0x800039c4, 0x800039c8, 0x800039cc, 0x800039d0, 0x800039d4, 0x800039d8, 0x800039dc, 0x80003aac, 0x80003ab0, 0x80003ab4, 0x80003ab8, 0x80003abc, 0x80003ac0, 0x80003ac4, 0x80003ac8, 0x80003acc, 0x80003ad0, 0x80003ad4, 0x80003ad8, 0x80003adc, 0x80003bac, 0x80003bb0, 0x80003bb4, 0x80003bb8, 0x80003bbc, 0x80003bc0, 0x80003bc4, 0x80003bc8, 0x80003bcc, 0x80003bd0, 0x80003bd4, 0x80003bd8, 0x80003bdc, 0x80003cac, 0x80003cb0, 0x80003cb4, 0x80003cb8, 0x80003cbc, 0x80003cc0, 0x80003cc4, 0x80003cc8, 0x80003ccc, 0x80003cd0, 0x80003cd4, 0x80003cd8, 0x80003cdc, 0x80003dac, 0x80003db0, 0x80003db4, 0x80003db8, 0x80003dbc, 0x80003dc0, 0x80003dc4, 0x80003dc8, 0x80003dcc, 0x80003dd0, 0x80003dd4, 0x80003dd8, 0x80003ddc, 0x800040ac, 0x800040b0, 0x800040b4, 0x800040b8, 0x800040bc, 0x800040c0, 0x800040c4, 0x800040c8, 0x800040cc, 0x800040d0, 0x800040d4, 0x800040d8, 0x800040dc, 0x800041ac, 0x800041b0, 0x800041b4, 0x800041b8, 0x800041bc, 0x800041c0, 0x800041c4, 0x800041c8, 0x800041cc, 0x800041d0, 0x800041d4, 0x800041d8, 0x800041dc, 0x800042ac, 0x800042b0, 0x800042b4, 0x800042b8, 0x800042bc, 0x800042c0, 0x800042c4, 0x800042c8, 0x800042cc, 0x800042d0, 0x800042d4, 0x800042d8, 0x800042dc, 0x800043ac, 0x800043cc, 0x800043d0, 0x800043d4, 0x800043d8, 0x800043dc, 0x800043e0, 0x800043e4, 0x800043e8, 0x800043ec, 0x800043f0, 0x800043f4, 0x800043f8, 0x800043fc, 0x80004400, 0x80004404, 0x80004408, 0x8000440c, 0x80004410, 0x80004414, 0x80004418, 0x8000441c, 0x80004420, 0x80004424, 0x80004428, 0x8000442c, 0x80004430, 0x800044ac, 0x800044b0, 0x800044b4, 0x800044b8, 0x800044bc, 0x800044c0, 0x800044c4, 0x800044c8, 0x800044cc, 0x800044d0, 0x800044d4, 0x800044d8 ...

