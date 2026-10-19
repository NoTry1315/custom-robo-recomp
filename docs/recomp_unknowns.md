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
  sdmc:/custom-robo/files/<FST path>, found by the FileInfo's startAddr. Reads finish on
  the host at once; async callbacks run at the next wait point (VIWaitForRetrace,
  DVDGetCommandBlockStatus, DVDGetDriveStatus). Read timing therefore differs from a
  real drive (no seek/transfer delay).
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
  SEQ_MOVIE (the code path after the second logo sets next = 2); whether Dolphin plays
  opening_a.thp first (e.g. after idling on the title) is not checked.

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

<!-- generated -->
## Audio: AX -> ndsp (phase 6; to verify against Dolphin)

runtime/ax.c, runtime/ai.c. Log lines `# AX:` (per-voice events, capped at 400, and
mix stats every 1000 AX frames = 5 s).

- The voice setters (AXSetVoice*, 0x801470d8..0x801479dc) are hand-written and store
  into the AXVPB at the SDK's offsets, read from their disassembly; the AXPB is at
  VPB+0x138 in Dolphin's field order (srcSelect +0x140, state +0x146, mix +0x14a, ve
  +0x19c, addr +0x1a6, adpcm +0x1b6, src +0x1de, adpcm loop +0x1ec). Sync bits are set
  as the SDK does and cleared for every acquired voice at the start of each AX frame.
- Frame order follows the SDK's AX interrupt: consume sync + render, THEN the user
  callback. Callback first trips synth.c:2323 (assert AX_SYNC_USER_VE clear before
  AXSetVoiceVeDelta).
- 0x80144be4 was named AXFreeVoice in the doc; it is AXSetVoicePriority (moves the
  VPB to priority list r4, stores r4 at +0xc). AXFreeVoice is 0x801449f0. Phase 5's
  runtime freed a voice on every priority change.
- Mixer: ADPCM (nibble addresses, header byte at each 16-nibble frame start, loop
  context from the ADPCM-loop block; yn1/yn2 only for non-stream voices), PCM16,
  PCM8; end -> loop or stop (state 0). Resampling is LINEAR for every src type
  (Dolphin uses the polyphase filter for type 0/1). Volume: ve (1.15, delta per
  sample) times main L/R (1.15, delta per sample). Aux A/B buses, surround, ITD,
  depop, LPF and the AXFX reverb/chorus callbacks are not mixed. Output 32000 Hz
  (Dolphin: 32028).
- Written back to the PB each frame: state, current address, pred_scale/yn1/yn2,
  src fraction and history, ve and L/R volumes.
- Output: ndsp channel 0, stereo PCM16, 8 x 20 ms buffers; a frame is dropped if the
  buffer is still playing. The game steps per retrace, not by the clock, so when it
  runs slower than real time the output underruns (gaps), not measured. ndspInit
  needs sdmc:/3ds/dspfirm.cdc; without it the mix still runs (WAV only). Citra: a
  placeholder file (autotest.ps1 writes it), as its HLE DSP never runs the firmware.
- sdmc:/cr_audio.wav: the first 60 s of the mix, for checking without listening; only
  when sdmc:/cr_audio_dump.txt exists (autotest.ps1 writes it). On a New 3DS the
  per-frame SD writes cost ~170 ms per retrace with 4 s stalls (first hardware run).
- Title, checked: SE at r452 (A on the no-card dialog), then cubog39.hps (stereo
  48 kHz ADPCM stream, ring of loop/end updates by the game) from r580 to r1133,
  ended by the game (non-looping HPS). Against tools/sound.py's decode of the same
  file: correlation 0.99 both channels, gain 0.345 (the voice volume). Dolphin
  (DumpAudio): same jingle, rms ~4.6k vs ours 4.5-5.1k, then ~22 s of silence, then
  the attract movie (THP, not implemented). DTK (AI disc stream) is silent in Dolphin
  and unused; ai.c only keeps its registers.
- Only one of the four A presses on the no-card dialogs makes a sound; Dolphin, with
  a card, never shows those dialogs, so this is not compared.

## First New 3DS runs (2026-10-18, `# time` lines, ms per frame)

- Run 1 was dominated by the audio WAV dump (SD writes, now opt-in).
- Run 2 (manual input, logo -> title -> mode select (scene 5) -> scenario (scene 8,
  first campaign scene, cubog08c.hps)): logo/dialogs 57 fps (game ~14 ms); title
  ~9 fps (gx vertex 52 ms, game 45); campaign scene 4.2 fps (gx vertex 131.5,
  game 100, tex 8; 968 draws, 16384 tris = the vertex buffer cap). Hardware runs our
  CPU vertex path FASTER than Citra (title: 52 vs 140 ms); Citra is not a lower bound.
- Campaign scene logs `vertex buffer full` (geometry past 16384 triangles dropped) and
  `C3D_TexInit failed` (linear/VRAM exhausted?). Not investigated.
- Audio is choppy only because the game steps per retrace at 4-9 fps (AX frames are
  tied to retraces); ndsp itself never dropped a buffer.
- Next: vertex transform/lighting in the PICA vertex shader; find the campaign's 100 ms
  of game time (translated code vs SDK layer, e.g. MTX); vertex buffer and texture
  memory limits.

## Performance pass (2026-10-18, Citra -Fast `# time`, ms per frame)

| | title vtx | campaign vtx | campaign game | campaign tex | campaign fps |
|---|---|---|---|---|---|
| CPU transform | 139.5 | 312.5 | 165 | 22 | 2.0 |
| PICA vertex shader | 32.1 | 100.7 | 182.7 | - | 3.3 |
| this pass | 27.8 | 82.7 | 76.7 | 58.2 | 3.7 |
| EFB copy textures skipped | 27.7 | 82.3 | 77.8 | 7.4 | 4.6 |

- `# prof` (cpu.lr sampler, runtime/log.c): ~80% of campaign samples at lr 0x801961ac,
  i.e. inside sdk_GXCallDisplayList. "game" time was our host GX code, not translated
  game code: two GSPGPU_FlushDataCache syscalls per draw (~1260 draws). Now one flush
  of the used vertex/index range per frame (frame_end). The rest of game code: under
  3% per call site.
- `C3D_TexInit failed`: fixed. I4/IA4 -> LA4, I8/IA8 -> LA8, RGB565 -> RGB565 (exact,
  converted straight from the GX tiles), LRU eviction and retry on failure, and a
  texture whose contents change reuses its allocation. The failing texture was
  0x81327020, 600x440 I4: the destination of a GXCopyTex every frame (GXSetTexCopySrc
  0,0,600,440; GXSetTexCopyDst fmt 0x20, believed to be the red channel at 4 bits; clear 1). EFB -> texture copies are not
  emulated, so RAM there is stale (the dump decodes to noise); GXCopyTex destinations
  are now not drawn (`# gx: EFB copy texture not drawn`). Dolphin draws a copy of the
  frame there (an effect layer); emulating it = render to a PICA texture at GXCopyTex.
- Vertex: XF matrices uploaded only after a GXLoad*MtxImm (setup 40 -> 20 ms per
  frame); decode keeps only the attributes the PICA vertex reads. Decode is ~61 ms for
  28k vertices; inlining the readers did not help in Citra. gx state (setup_tev /
  setup_pixel / C3D_DrawElements, ~39 us per draw) is 49 ms: next target (dirty flags).
- The campaign's first dialog appears after some beeps. At 3-4 fps the autotest needs
  ~3000 retraces and A pressed every 120 retraces (held 30) to reach and advance it.

## Generated by tools/recomp/recomp.py

Functions translated: 2723. SDK stubs: 1049. MetroTRK entries skipped: 184. Jump tables as switch: 52.

### flow into non-translated code that is not a known entry (1)

0x801aea48 -> 0x801346c8

### fres/frsqrte estimate (needs Dolphin table) (99)

0x800082ec frsqrte, 0x8001b5dc frsqrte, 0x8001b824 frsqrte, 0x8001c03c frsqrte, 0x8001c678 frsqrte, 0x8001c79c frsqrte, 0x8001cbb4 frsqrte, 0x80043848 frsqrte, 0x80043980 frsqrte, 0x80048c4c frsqrte, 0x8004a69c frsqrte, 0x8004cbc4 frsqrte, 0x8005fdcc frsqrte, 0x8006bab4 frsqrte, 0x8006c2c8 frsqrte, 0x8006c468 frsqrte, 0x8006d2b8 frsqrte, 0x8006d778 frsqrte, 0x80076d00 frsqrte, 0x80076e00 frsqrte, 0x800979fc frsqrte, 0x80097e58 frsqrte, 0x800e5604 frsqrte, 0x800e5734 frsqrte, 0x801396b4 fres, 0x801397ac fres, 0x80139950 frsqrte, 0x80139ac8 fres, 0x8013a1e0 frsqrte, 0x8013a244 frsqrte, 0x8016d8bc frsqrte, 0x8016d924 frsqrte, 0x8016d97c frsqrte, 0x8016d9e8 frsqrte, 0x8016ddc8 frsqrte, 0x8016de30 frsqrte, 0x8016de88 frsqrte, 0x8016def4 frsqrte, 0x8016f700 frsqrte, 0x8016f768 frsqrte, 0x8016f7c0 frsqrte, 0x8016f82c frsqrte, 0x801707f8 frsqrte, 0x801708b8 frsqrte, 0x8017094c frsqrte, 0x80170a10 frsqrte, 0x80179a14 frsqrte, 0x8017ae10 frsqrte, 0x8017b080 frsqrte, 0x8017b26c frsqrte, 0x8017b2d8 frsqrte, 0x8017e664 frsqrte, 0x8017f56c frsqrte, 0x8017fb84 frsqrte, 0x80180404 frsqrte, 0x80181094 frsqrte, 0x80181104 frsqrte, 0x80183060 frsqrte, 0x801830e8 frsqrte, 0x801909f0 frsqrte, 0x80190a8c frsqrte, 0x80191140 frsqrte, 0x801911dc frsqrte, 0x80199ec4 frsqrte, 0x80199f44 frsqrte, 0x80199ffc frsqrte, 0x8019a068 frsqrte, 0x8019a144 frsqrte, 0x8019a3d0 frsqrte, 0x8019a4c0 frsqrte, 0x8019a6d0 frsqrte, 0x8019a900 frsqrte, 0x8019b578 frsqrte, 0x8019b5f0 frsqrte, 0x8019b860 frsqrte, 0x8019b8d8 frsqrte, 0x8019b944 frsqrte, 0x8019bb4c frsqrte, 0x8019bbc4 frsqrte, 0x8019bd24 frsqrte, 0x8019becc frsqrte, 0x8019bf44 frsqrte, 0x8019bfbc frsqrte, 0x8019e694 frsqrte, 0x8019e748 frsqrte, 0x8019e804 frsqrte, 0x8019e8ac frsqrte, 0x8019efe4 frsqrte, 0x8019f068 frsqrte, 0x8019f0ec frsqrte, 0x8019f65c frsqrte, 0x8019f728 frsqrte, 0x8019f814 frsqrte, 0x8019f938 frsqrte, 0x8019f9e0 frsqrte, 0x801a1290 frsqrte, 0x801a1310 frsqrte, 0x801a19bc frsqrte, 0x801a66ac frsqrte

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

### valid code not reached from any function start (459)

0x800034b0, 0x800034b4, 0x800034b8, 0x800034bc, 0x800034c0, 0x800034c4, 0x800034c8, 0x800034cc, 0x800034d0, 0x800034d4, 0x800034d8, 0x800035ac, 0x800036ac, 0x800036b0, 0x800036b4, 0x800036b8, 0x800036bc, 0x800036c0, 0x800036c4, 0x800036c8, 0x800036cc, 0x800036d0, 0x800036d4, 0x800036d8, 0x800036dc, 0x800036e0, 0x800036e4, 0x800036e8, 0x800036ec, 0x800036f0, 0x800036f4, 0x800037ac, 0x800037b0, 0x800037b4, 0x800037b8, 0x800037bc, 0x800037c0, 0x800037c4, 0x800037c8, 0x800037cc, 0x800037d0, 0x800037d4, 0x800037d8, 0x800037dc, 0x800038ac, 0x800038b0, 0x800038b4, 0x800038b8, 0x800038bc, 0x800038c0, 0x800038c4, 0x800038c8, 0x800038cc, 0x800038d0, 0x800038d4, 0x800038d8, 0x800038dc, 0x800039ac, 0x800039b0, 0x800039b4, 0x800039b8, 0x800039bc, 0x800039c0, 0x800039c4, 0x800039c8, 0x800039cc, 0x800039d0, 0x800039d4, 0x800039d8, 0x800039dc, 0x80003aac, 0x80003ab0, 0x80003ab4, 0x80003ab8, 0x80003abc, 0x80003ac0, 0x80003ac4, 0x80003ac8, 0x80003acc, 0x80003ad0, 0x80003ad4, 0x80003ad8, 0x80003adc, 0x80003bac, 0x80003bb0, 0x80003bb4, 0x80003bb8, 0x80003bbc, 0x80003bc0, 0x80003bc4, 0x80003bc8, 0x80003bcc, 0x80003bd0, 0x80003bd4, 0x80003bd8, 0x80003bdc, 0x80003cac, 0x80003cb0, 0x80003cb4, 0x80003cb8, 0x80003cbc, 0x80003cc0, 0x80003cc4, 0x80003cc8, 0x80003ccc, 0x80003cd0, 0x80003cd4, 0x80003cd8, 0x80003cdc, 0x80003dac, 0x80003db0, 0x80003db4, 0x80003db8, 0x80003dbc, 0x80003dc0, 0x80003dc4, 0x80003dc8, 0x80003dcc, 0x80003dd0, 0x80003dd4, 0x80003dd8, 0x80003ddc, 0x800040ac, 0x800040b0, 0x800040b4, 0x800040b8, 0x800040bc, 0x800040c0, 0x800040c4, 0x800040c8, 0x800040cc, 0x800040d0, 0x800040d4, 0x800040d8, 0x800040dc, 0x800041ac, 0x800041b0, 0x800041b4, 0x800041b8, 0x800041bc, 0x800041c0, 0x800041c4, 0x800041c8, 0x800041cc, 0x800041d0, 0x800041d4, 0x800041d8, 0x800041dc, 0x800042ac, 0x800042b0, 0x800042b4, 0x800042b8, 0x800042bc, 0x800042c0, 0x800042c4, 0x800042c8, 0x800042cc, 0x800042d0, 0x800042d4, 0x800042d8, 0x800042dc, 0x800043ac, 0x800043cc, 0x800043d0, 0x800043d4, 0x800043d8, 0x800043dc, 0x800043e0, 0x800043e4, 0x800043e8, 0x800043ec, 0x800043f0, 0x800043f4, 0x800043f8, 0x800043fc, 0x80004400, 0x80004404, 0x80004408, 0x8000440c, 0x80004410, 0x80004414, 0x80004418, 0x8000441c, 0x80004420, 0x80004424, 0x80004428, 0x8000442c, 0x80004430, 0x800044ac, 0x800044b0, 0x800044b4, 0x800044b8, 0x800044bc, 0x800044c0, 0x800044c4, 0x800044c8, 0x800044cc, 0x800044d0, 0x800044d4, 0x800044d8 ...

