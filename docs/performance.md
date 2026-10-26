# Performance history

Every change made for speed, with what it was measured to do, how it was measured, and
whether it is on. Negative results stay listed: knowing what did not help is as useful as
knowing what did. Details of each change live in `docs/recomp_unknowns.md` (section names
in the last column); the day-by-day log is the Status list in `CLAUDE.md`.

Rule for every entry: the number, where it was measured (hardware run N or Citra replay),
the scene, and the commit or branch. "Not yet on hardware" until a hardware run has it.

## How performance is measured

- **One test build.** The SD card holds one build (`sdmc:/3ds/custom-robo-recomp.3dsx`). Features are
  compared with bottom-screen buttons / flag files within that build. Build-time options (LTO, GENOPT,
  FLOATOPS) are decided on or off in main, not shipped as a second build.

- **Hardware runs** (New 3DS, played by hand): the game writes `sdmc:/cr_autotest.log` with a
  `# time` window every 60 retraces. `tools/time_avg.py <log> [--from N]` averages them per
  scene (fps, game speed, game CPU ms, GX thread ms, GPU ms). Logs are kept in `logs/` as
  `cr_autotest_<date>_hw-run<N>-<what>.log`. Hand play means two runs never show the same
  frames: compare scenes with many windows, or the same scene within one run (bottom-screen
  buttons switch a feature on and off live, and `time_avg.py` splits windows by mode).
- **Citra replays** (`tools/citra/replay.ps1`): the same pad input every run, so logs are
  deterministic. `tools/citra/compare_logs.py` checks that a change keeps every SDK call,
  scene and pad line and the framebuffer dumps identical. Citra's game ms follows the amount
  of ARM code run, so it measures translator and CPU-side changes well. It does **not** model
  ARM11 caches, FCRAM vs VRAM bandwidth, the PICA's real speed or VFP traps, so GPU and
  memory changes can only be judged on hardware.

  | Replay | What it plays | Heavy part |
  |---|---|---|
  | `battle` | Vs. mode 1v1, idle robots (no card) | scene 4.2 r2400-3600 |
  | `campaign` | story walking (no card) | scene 8.3 r1470-2400 |
  | `2v2` | story save `data/saves/2026-10-22_pre-2v2` -> dialogue -> 2v2 battle (card on) | scene 4.2 r4200-8610 |

  Citra baselines (main@ccbd9f7 + the 2v2 test, `time_avg.py --from 4260`): 2v2 scene 4.2 game 52.9 ms,
  GX thread busy 37.7 ms (two runs identical, 5269/5269 log lines). Citra ms are slower than hardware
  (2v2 on hardware: game 29.8 ms) but rank changes the same way for CPU-side work.
  After d1e6d03 + 3f87081 (2026-10-22): 2v2 game 43.2 ms, battle 4.2 (`--from 2460`) 17.1, campaign 8.3 8.9.
  After the 2026-10-23 2v2 work (branch claude/2v2-perf-2-5cf2f9 @ 40381bd): 2v2 game 36.0 ms (GX thread busy 38.6,
  now longer than the game thread in Citra), battle 14.1, campaign 8.0.

## Where it stands (hardware run 28, 2026-10-22, main@75ea35d LTO build)

| Scene | fps | game speed | game CPU ms | GPU ms | bound by |
|---|---|---|---|---|---|
| Campaign 8.3 | 56.7 | 95% | 9.9 | 3.7 | near 60 |
| Battle 1v1, mono | 57.3 | 95% | 15.1 | 2.2 | near 60 |
| Battle 1v1, stereo 3D | 51.4 | 86% | 14.5 | 9.8 | GPU + CPU |
| Battle 2v2 | 30.5 | 51% | 29.8 | 5.6 | **game CPU** |
| Title, menus | ~60 | 97-100% | | | |

Game speed is fps / 60: the game runs one logic step per frame. From the first battle on
hardware (run 4, 6 fps) to run 28, battle went 6 -> 57 fps.

## Changes that helped (newest first)

| Date | Change | Measured effect | Status | Details (unknowns) |
|---|---|---|---|---|
| 10-26 | **Lazy vertex descriptor** (gx.c: GXSetVtxDesc/GXClearVtxDesc only change the game thread's copy; one whole-descriptor record before a draw record, only when it differs from the last one sent) | Citra 2v2 4.2: GX-queue records 7586 -> 6428 per frame (descriptor records ~1200 -> ~80), game 35.7 (base) / 36.1 (flag off) -> 35.6 ms, GX busy 38.3 -> 37.9, frame 45.7 -> 45.4; battle and campaign unchanged; logs + fb identical on and off | on (small, below 2%); button "Lazy vtx desc", `cr_lazydesc_off.txt`; not yet on hardware | Lazy vertex descriptor |
| 10-23 | **Stack words in C locals** (translator `promote_stack_slots`: frame words only the function uses, below every r1-derived pointer and not reached by a callee; 3439 words in 483 functions) | Citra 2v2 game 37.4 -> 36.0 ms, battle 14.6 -> 14.1, campaign 8.2 -> 8.0; logs + fb identical (40381bd) | on; `RECOMP_STACK_SLOTS=0` at generation = memory; not yet on hardware | 2v2 game CPU 2 |
| 10-23 | **SDK call path**: `LOG()` inline with a per-function count slot (no hash probe); GXBegin's vertex size cached per format | Citra 2v2 game 39.0 -> 37.4 ms, battle 15.5 -> 14.6, campaign 8.4 -> 8.2; logs + fb identical (136d224) | always on; not yet on hardware | 2v2 game CPU 2 |
| 10-23 | **WGPIPE stores inline** (immediate-mode vertex bytes go straight into the FIFO while the primitive is incomplete) | Citra 2v2 game 40.9 -> 39.0 ms, battle 16.3 -> 15.5; logs + fb identical (0c8c024) | always on; not yet on hardware | 2v2 game CPU 2 |
| 10-23 | **Base-register accesses** (translator: `rA + k` loads/stores check and mask rA, k is the offset; guard space around ram[]) | Citra 2v2 game 43.2 -> 40.9 ms, battle 17.1 -> 16.3, campaign 8.9 -> 8.4; logs + fb identical (c27c2f2) | on; `RECOMP_BASE_ACCESS=0` at generation = old; not yet on hardware | 2v2 game CPU 2 |
| 10-22 | **Vertex packing**: display-list draws store only the inputs the draw reads, in the GX source format (s16/s8/u8/f32) where the PICA has one; normal dropped when unlit | Citra: vertex fetch 68 -> 32.6 B per index (campaign, 4154 -> 1992 KB/frame), 29.4 B (battle, 5815 -> 2513 KB/frame); logs + fb dumps identical. GPU effect not yet on hardware | on; button "Vertex pack", `cr_vtxpack_off.txt` | Vertex packing |
| 10-22 | **Non-volatile saves dropped** (translator: MWCC prologue saves / epilogue restores of r14-r31, f14-f31 that only fed dead locals; 1886 functions) | Citra 2v2 game 50.4 -> 43.2 ms, battle 19.2 -> 17.1, campaign 9.8 -> 8.9; logs + fb identical (3f87081) | on; `RECOMP_KEEP_NV_SAVES=1` at generation keeps them; not yet on hardware | 2v2 game CPU |
| 10-22 | **Native MSL sin/cos/sinf/cosf + sqrtf**; double fmadd as inline `a*c + b` instead of newlib's (already non-fused) `fma()` | Citra 2v2 game 52.9 -> 50.4 ms (libm off in the same build: 52.3), battle 20.0 -> 19.2, campaign 9.9 -> 9.8; logs + fb identical (d1e6d03) | on; button "Native sin/cos", `cr_libm_off.txt`; not yet on hardware | 2v2 game CPU |
| 10-22 | **Textures in VRAM** (copied from linear FCRAM after 2 unchanged frames; LRU back to linear) | HW run 27 -> 28: campaign GPU 12.2 -> 3.7 ms, 51.2 -> 56.7 fps; battle GPU 9.3 -> 2.2 ms (different fights: rough) | on; button "VRAM textures", `cr_vramtex_off.txt` | VRAM textures |
| 10-22 | **Stereo 3D** by replaying the left eye's GPU commands for the right eye (mw3ds technique) | HW run 28: 1v1 battle 51.4 fps with 3D on (GPU 9.8 ms) vs 57.3 mono; 0 fallbacks | on with the 3D slider | Stereo 3D |
| 10-22 | Back-face culling honoured (GXSetCullMode) | HW run 26: campaign GPU 18.9 -> 15.6 ms, 44.4 -> 52.1 fps | on | Cull mode |
| 10-22 | Lossless 16-bit texture formats, mip chains | inert/small (game uses no mips) | on | Texture mipmaps and lossless formats |
| 10-22 | Codegen per instruction, model scaling, GX display-list path, call-boundary glue (perf-test branch) | HW run 24 -> 25: battle 47.9 -> 54.2 fps (game 19.1 -> 16.1 ms); campaign game 12.3 -> 5.5 ms | on | Codegen per guest instruction; Per-model cost; GX thread per display-list cost; Call-boundary glue |
| 10-21 | GX thread on core 2 (GX work off the game thread) | HW run 23 -> 24: battle 35.0 -> 47.9 fps (58 -> 80%); same run on/off 32 -> 44 fps | on; button | GX thread |
| 10-21 | Mid-frame GPU submit (GPU starts while the CPU builds the frame) | HW runs 20-21 on/off: battle 29.8 -> 37.5 fps (gx sync 9.1 -> 1.0 ms), campaign 20 -> 29.8 fps | on; `cr_gpukick_off.txt` | Mid-frame GPU submit |
| 10-21 | DVD reader thread | removes read stalls from the game thread | on; `cr_dvdasync_off.txt` | DVD reader thread |
| 10-19 | Texture write tracking (skip re-hashing unchanged textures) | Citra battle frame 36.3 -> 32.1 ms | on | CLAUDE.md status |
| 10-19 | Display-list write tracking (per-1 KB page store epochs) | Citra battle frame 38.7 -> 36.3 ms | on; `cr_dlclean_off.txt` | CLAUDE.md status |
| 10-19 | GXBegin FIFO size computed once | Citra battle game 23.3 -> 19.5 ms (-16%) | on | CLAUDE.md status |
| 10-19 | .sdata2 constants folded by the translator | Citra battle game -3.8% | on | CLAUDE.md status |
| 10-19 | Register spills only where needed (interprocedural summaries) | Citra game ms -4.5..-5.5% | on | Register spills per call site |
| 10-19 | Frame pacing: 60 Hz cap instead of a vblank wait per frame | Citra battle 12.8 -> 14.6 fps; HW battle game 34.9 -> 21.8 ms | on; `cr_vsync_on.txt` = old | Frame pacing: vblank wait |
| 10-19 | Native MTX/VEC functions; single-precision float ops (FLOATOPS) | HW battle 19.9 -> 21.7 -> 21.9 fps | on; `cr_mtx_off.txt`, `make FLOATOPS=0` | Float ops and native MTX |
| 10-19 | Whole display-list cache | HW battle 11-12.5 -> 16-20 fps (run 9 -> 10) | on; `cr_dlcache_off.txt` | Display-list cache |
| 10-19 | GX CPU cost (texture index memo, TEV/scissor compare in place) | Citra battle 5.4 -> 6.5 fps | on | GX CPU cost |
| 10-19 | Draw batching (same-state draws merged) | Citra battle 4.6 -> 5.4 fps | on; `cr_batch_off.txt` | GX draw batching |
| 10-19 | Open DVD file handles kept (LRU of 8) | HW run 16: longest read 3633 -> 187 ms, no stalls | on | CLAUDE.md status |
| 10-19 | GX_CMDLIST_FLUSH, log threads on core 2, native memcpy/memset/sqrt | HW battle 165 -> 105-122 ms per frame (run 4 -> 8) | on | Perf wins 2026-10-19 |
| 10-19 | SD log writes buffered on a thread | removed multi-second stalls | on | Hardware stalls and jaggies |
| 10-18 | Vertex cache (decode display-list draws once) | Citra battle 3.7 -> 4.5-4.8 fps, title 15 -> 30 | on | Vertex cache |
| 10-18 | Translator: registers as C locals, EABI spills | Citra battle game 118 -> 59 ms | on | Translator performance |
| 10-18 | GX transform/lighting/texgen in the PICA vertex shader | Citra campaign 2.0 -> 4.6 fps | on | CLAUDE.md status |

## Tried and not kept

| Date | Change | Result | Why not kept |
|---|---|---|---|
| 10-26 | **Pointer-extent stack promotion** (translator: frame words outside every byte range reached through a frame pointer, with per-callee r3-r12 extents and `tools/recomp/native_ptr_ext.tsv` for natives; 3439 -> 4578 words) | Citra 2v2 game 36.0 -> 35.6 ms (-1.1%, 2 runs), battle 14.0 -> 13.9, campaign 8.0 -> 7.9; .text -0.7%; logs + fb identical (branch ptr-extent-promo) | below the 2% bar: the hot particle frames' remaining words are buffers really passed to PSMTX*/OSReport. Off by default; `RECOMP_PTR_EXTENT=1` at generation turns it on. Not on hardware |
| 10-23 | Inline copies: `static inline` copies of translated functions of <= 32 instructions in each gen/ file that calls them (GCC inlines across files) | Citra 2v2 game 37.4 -> 37.2 ms (-0.5%), +5.5% ARM instructions | below the 2% bar; small callees are cheap to call already |
| 10-23 | Per-native in-sets, LR save/restore removal | sized from site counts: < 0.1 ms and ~0.3 ms | not built (see unknowns "2v2 game CPU 2") |
| 10-22 | -O3 on gen/ (`make GENOPT=-O3`) | Citra battle game 19.96 -> 19.94 ms, +9% code | no gain; more code for the 16 KB I-cache |
| 10-22 | LTO (`make LTO=1`) | Citra battle game -1.5%, +2% code | decided 2026-10-22: off by default (small Citra-only gain, no hardware evidence; one test build, so it cannot be A/B tested on hardware). `make LTO=1` stays available. Run 28 ran an LTO build without problems |
| 10-22 | FPSCR RunFast (`cr_runfast.txt`) | boot FPSCR is already 03000000 (flush-to-zero + default NaN) on Citra and on hardware (run 28) | nothing to gain: already on |
| 10-22 | Vertex shader variants | GPU hang at the first draw on New 3DS (fine in Citra) | reverted; one shader only |
| 10-19 | Mid-frame C3D_FrameSplit every 1024 words | HW battle 35.0 -> 39.3 ms | slower |
| 10-19 | int->double stack peephole in the translator | -0.05% | GCC already forwards the store |
| 10-19 | 2x supersampling (anti-aliasing) | too slow on hardware | reverted; jaggies remain |

## Open (next)

- **2v2 battle is game-CPU bound** (29.8 ms of a 32.8 ms frame, run 28). Profiled 2026-10-22 (unknowns
  "2v2 game CPU"); after native sin/cos and dropped saves, Citra 2v2 game 52.9 -> 43.2 ms (-18%), and after the
  2026-10-23 work (unknowns "2v2 game CPU 2") 36.0 ms. Hardware run 29 (both together): 2v2 30.5 -> 39.6 fps,
  game 29.7 -> 22.2 ms, GX thread 16.6 ms, so the game thread still decides the frame. Next: stack
  words between address-taken locals (needs per-callee pointer extents); call glue (22% of the FNPROF frame, mostly
  the C call itself); MTX native schedule (hardware only).
- **2v2 particle family profiled** (2026-10-25, Citra, main@ad99a97, no change made; unknowns "2v2 particle
  family"): 0x8017b000-0x80185000 is the HSD particle system. Inclusive it costs 7.0 ms of the 2v2 game thread
  (18.6%; draw 5.56, generators 0.86, interpreter 0.58) against 0.47 ms in the 1v1. It scales with particles
  (186 drawn per frame vs 10), not with robots. Confirmed repeats: 87% of particle GXBegins keep the previous
  vertex descriptor, 194 PSMTXMultVec per frame repeat per attached object, and 63% of all sin/cos calls game-wide
  repeat an argument within the frame. The family's 16.4k remaining stack accesses per frame sit above
  address-taken locals. Ranked next: pointer-extent stack promotion, then a lazy vertex descriptor in gx.c and a
  sin/cos memo study; the in-family redundant math is game logic and too small for a session.
- **Pointer-extent stack promotion tried** (2026-10-26, unknowns "Pointer-extent stack promotion"): +33% promoted
  words, Citra 2v2 game -1.1%, identical replays; off by default (below 2%). Next in the 2v2 sequence: lazy vertex
  descriptor (gx.c), then the sin/cos memo study.
- Hardware run of main@dad2161 (vertex packing + 2v2 CPU work): campaign/battle GPU ms with the "Vertex pack"
  button on/off, 2v2 game ms vs run 28 (29.8), "Native sin/cos" on/off.
- Render skip when a frame runs late (keep game speed at 100% by drawing every other frame),
  needed most with stereo 3D.

## Hardware run index

| Run | Date | Build | Log (`logs/`) | Tested |
|---|---|---|---|---|
| 30 | 10-25 | branch 2v2-perf-2 @40381bd | cr_autotest_2026-10-25_hw-run30-t06-text-hang.log (+ hw-run30-shots/) | story event snr_t_06 hangs ("Awex!" box without text, A/B do nothing). A Rosalina pause at r14951 made real-time audio pacing skip 807 AX frames, but that is not the cause: Citra replays with an injected 4.3 s stall (cr_ax_stall.txt, real-time pacing) play through, and the screenshot taken during the pause already shows the previous box broken (unknowns "Run 30 text hang"). Same build, save and event run fine in Citra. Not a 2v2-perf-2 change; cause open |
| 29 | 10-23 | branch 2v2-perf-2 @40381bd | cr_autotest_2026-10-23_hw-run29-2v2-perf2.log | 2v2 battle 4.2: 39.6 fps (66%), game 22.2 ms, GX busy 16.6, gpu 2.6 (run 28: 30.5 fps, game 29.7, GX 18.6); campaign 8.3 58.5 fps, game 11.0 |
| 28 | 10-22 | main@75ea35d LTO | cr_autotest_2026-10-22_hw-run28-merged-lto.log | VRAM textures, stereo 3D, FPSCR value, 2v2 battle |
| 27 | 10-22 | main 8575bae | cr_autotest_2026-10-22_hw-run27-clean-main.log | campaign GPU clean build |
| 26 | 10-22 | campaign-gpu | cr_autotest_2026-10-22_hw-run26-campaign-gpu-vsh-single.log | culling, vertex shader |
| 25 | 10-22 | perf-test bcd754a | cr_autotest_2026-10-22_hw-run25-perf-branches.log | codegen, model scaling, call glue |
| 24 | 10-21 | | cr_autotest_2026-10-21_hw-run24-gx-thread.log | GX thread |
| 20-23 | 10-21 | | cr_autotest_2026-10-21_hw-run2x-*.log | GPU kick, DVD thread, movie |
| 4-19 | 10-18/19 | | on the SD card root (`cr_autotest_2026-10-1x_hw-run*.log`) | see CLAUDE.md status |
