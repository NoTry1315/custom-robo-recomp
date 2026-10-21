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
  (iterated per function until stable). Exception: ctr, ca, cr0, cr5-7 are still reloaded
  after every call, as before. Keeping them in locals when the callee cannot change them
  made the campaign replay rebuild 4 GX display lists every frame from r1500 (dl cache
  misses; SDK calls and scenes unchanged). Bisected to that class (FPRs and GPRs are fine),
  the reading code was not found.
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

<!-- generated -->
## Generated by tools/recomp/recomp.py

Functions translated: 2744. SDK stubs: 1047. MetroTRK entries skipped: 184. Jump tables as switch: 52.

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

### valid code not reached from any function start (606)

0x800034b0, 0x800034b4, 0x800034b8, 0x800034bc, 0x800034c0, 0x800034c4, 0x800034c8, 0x800034cc, 0x800034d0, 0x800034d4, 0x800034d8, 0x800035ac, 0x800036ac, 0x800036b0, 0x800036b4, 0x800036b8, 0x800036bc, 0x800036c0, 0x800036c4, 0x800036c8, 0x800036cc, 0x800036d0, 0x800036d4, 0x800036d8, 0x800036dc, 0x800036e0, 0x800036e4, 0x800036e8, 0x800036ec, 0x800036f0, 0x800036f4, 0x800037ac, 0x800037b0, 0x800037b4, 0x800037b8, 0x800037bc, 0x800037c0, 0x800037c4, 0x800037c8, 0x800037cc, 0x800037d0, 0x800037d4, 0x800037d8, 0x800037dc, 0x800038ac, 0x800038b0, 0x800038b4, 0x800038b8, 0x800038bc, 0x800038c0, 0x800038c4, 0x800038c8, 0x800038cc, 0x800038d0, 0x800038d4, 0x800038d8, 0x800038dc, 0x800039ac, 0x800039b0, 0x800039b4, 0x800039b8, 0x800039bc, 0x800039c0, 0x800039c4, 0x800039c8, 0x800039cc, 0x800039d0, 0x800039d4, 0x800039d8, 0x800039dc, 0x80003aac, 0x80003ab0, 0x80003ab4, 0x80003ab8, 0x80003abc, 0x80003ac0, 0x80003ac4, 0x80003ac8, 0x80003acc, 0x80003ad0, 0x80003ad4, 0x80003ad8, 0x80003adc, 0x80003bac, 0x80003bb0, 0x80003bb4, 0x80003bb8, 0x80003bbc, 0x80003bc0, 0x80003bc4, 0x80003bc8, 0x80003bcc, 0x80003bd0, 0x80003bd4, 0x80003bd8, 0x80003bdc, 0x80003cac, 0x80003cb0, 0x80003cb4, 0x80003cb8, 0x80003cbc, 0x80003cc0, 0x80003cc4, 0x80003cc8, 0x80003ccc, 0x80003cd0, 0x80003cd4, 0x80003cd8, 0x80003cdc, 0x80003dac, 0x80003db0, 0x80003db4, 0x80003db8, 0x80003dbc, 0x80003dc0, 0x80003dc4, 0x80003dc8, 0x80003dcc, 0x80003dd0, 0x80003dd4, 0x80003dd8, 0x80003ddc, 0x800040ac, 0x800040b0, 0x800040b4, 0x800040b8, 0x800040bc, 0x800040c0, 0x800040c4, 0x800040c8, 0x800040cc, 0x800040d0, 0x800040d4, 0x800040d8, 0x800040dc, 0x800041ac, 0x800041b0, 0x800041b4, 0x800041b8, 0x800041bc, 0x800041c0, 0x800041c4, 0x800041c8, 0x800041cc, 0x800041d0, 0x800041d4, 0x800041d8, 0x800041dc, 0x800042ac, 0x800042b0, 0x800042b4, 0x800042b8, 0x800042bc, 0x800042c0, 0x800042c4, 0x800042c8, 0x800042cc, 0x800042d0, 0x800042d4, 0x800042d8, 0x800042dc, 0x800043ac, 0x800043cc, 0x800043d0, 0x800043d4, 0x800043d8, 0x800043dc, 0x800043e0, 0x800043e4, 0x800043e8, 0x800043ec, 0x800043f0, 0x800043f4, 0x800043f8, 0x800043fc, 0x80004400, 0x80004404, 0x80004408, 0x8000440c, 0x80004410, 0x80004414, 0x80004418, 0x8000441c, 0x80004420, 0x80004424, 0x80004428, 0x8000442c, 0x80004430, 0x800044ac, 0x800044b0, 0x800044b4, 0x800044b8, 0x800044bc, 0x800044c0, 0x800044c4, 0x800044c8, 0x800044cc, 0x800044d0, 0x800044d4, 0x800044d8 ...

