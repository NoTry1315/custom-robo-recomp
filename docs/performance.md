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
  memory changes can only be judged on hardware. Its *shares* also mislead: FNPROF put the 2v2 call
  glue at 21.8% of the frame, while the instruction count of that glue is ~5% of the hardware game
  thread (unknowns "Call-boundary cost on the ARM side"). Use Citra to rank and check identity, not
  to decide where hardware time goes.
- **Hardware counters** (`sdmc:/cr_perfctr.txt` or the button "Perf counters", off by default): a
  `# perf` line every 60 retraces with the ARM11 core 0 counters per frame (cycles, instructions,
  I-cache misses, D-cache reads and misses, stall cycles, branch mispredicts; `runtime/perfctr.c`).
  `tools/perf_avg.py <log> [--from N]` averages them per scene next to the `# time` game ms. Citra
  only stubs the SVC. First hardware run: run 33 (costs ~10 us per frame).
- **Static call cost** (`tools/callcost.py`): the call-boundary instructions of every translated
  function in an ELF (objdump -l), weighted by a `RECOMP_SITEPROF=1` replay's call counts.

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

## Where it stands (latest hardware run per scene, 2026-10-26, main@35e6839)

| Scene | fps | game speed | game CPU ms | GPU ms | bound by | run |
|---|---|---|---|---|---|---|
| Campaign 8.3 | 59.0 | 98% | 6.4 | 2.9 | near 60 (GX thread 15.5) | 41a (2v2 replay route) |
| Battle 1v1, mono | 57.3 | 95% | 15.1 | 2.2 | near 60 | 28 |
| Battle 1v1, stereo 3D | 51.4 | 86% | 14.5 | 9.8 | GPU + CPU | 28 |
| Battle 2v2 | 44.7 | 74% | 19.6 | 2.1 | **game CPU** (GX thread 15.8) | 41a (Citra 2v2 replay) |
| Title, menus | ~60 | 97-100% | | | | 31 |

Game speed is fps / 60: the game runs one logic step per frame. From the first battle on
hardware (run 4, 6 fps) to run 28, battle went 6 -> 57 fps; the 2v2 went 30.5 (run 28) -> 40.3 fps (run 32a) -> 44.7 fps (run 41a, after the density steps D1-D5).

## Changes that helped (newest first)

| Date | Change | Measured effect | Status | Details (unknowns) |
|---|---|---|---|---|
| 10-26 | **Float locals for single-precision FPR halves** (D5: the 10-22 branch claude/float-locals-d57296 @3f77d16, cherry-picked onto main: recomp.py fp_single() keeps FPR halves that provably hold a single as float locals; single ops of floats in float) | .text 4.27 -> 4.25 MB; static vcvt f32<->f64 14285 -> 10621 (-26%). Citra game: 2v2 4.2 32.3 -> 32.1 ms, battle 13.1 -> 13.0, campaign 7.6 -> 7.5; boot/battle/campaign/2v2 logs + fb identical to D3. **HW runs 39a -> 39b (2v2 replay through cr_agent): game 19.81 -> 19.68 ms (-0.13, median -0.20, sd 0.40, faster in 61 of 73), busy cycles -0.20M (-1.2%, 73 of 73); campaign 8.3 -0.11 ms (cycles fewer in 37 of 37). Stall-data share unchanged (26.0 -> 25.9%): the gain is fewer instructions** | default on | Single-precision FPR halves |
| 10-26 | **Stores without the hardware check** (D3: stores write RAM untested outside the 33 functions that set a hardware address, gen/recomp_hw_*.c; an address GCC knows is the GX pipe still goes to the FIFO; `make STORECHECK=1` = all checked; HWCHECK=verify counts stores reaching hw_write) | .text 5.12 -> 4.27 MB (-16.6%); 2v2 hot set 89.3k -> 76.3k ARM instructions (-14.5%). Verify replays: 0 stores reached hw_write. **HW runs 37a -> 37b (2v2 replay, two builds back to back, run by tools/hw_loop.py): 2v2 game 20.26 -> 19.80 ms (-0.46, median -0.50, sd 0.13, faster in 72 of 73), 43.5 -> 44.4 fps; busy cycles -0.39M (-2.2%, 73 of 73), I-cache misses 35.2 -> 33.6 per 1000 instructions; campaign 8.3 game -0.13 ms.** Citra game: 2v2 4.2 32.6 -> 32.3 ms, battle 13.1 -> 13.1, campaign 7.6 -> 7.6; boot/battle/campaign/2v2 logs + fb dumps identical to D2 | default on | Stores without the hardware check |
| 10-26 | **Write tracking through a byte map** (D2: guest stores mark their 1 KB page with a constant byte instead of writing the epoch; sdk_GXCopyDisp sweeps the marks into `ram_wr[]` epochs; `make WRMAP=0` = old) | .text 5.31 -> 5.14 MB (-3.1%); 2v2 hot set 91.6k -> 89.3k ARM instructions (-2.5%). **HW runs 36a -> 36b (2v2 replay, two builds back to back): 2v2 game -0.18 ms vs the clean run 35b (20.25 -> 20.07, -0.9%; sd 0.11, faster in 65 of 73), busy cycles 17.79 -> 17.62M (-1.0%, fewer in 73 of 73), instructions 6.07 -> 6.03M; the sweep itself is ~0.12 ms of it; campaign unchanged.** Citra game (sweep included, 0.21 ms in the 2v2): 2v2 4.2 32.9 -> 32.6 ms, battle 13.1 -> 13.1, campaign 7.6 -> 7.6; boot/battle/campaign/2v2 logs + fb dumps identical to D1 | default on | Write tracking through a byte map |
| 10-26 | **Loads without the hardware check** (D1: `rd8/16/32` and `_b` forms read RAM through the mask without testing for 0xCC/0xCD addresses; stores keep the check for the GX pipe; `make HWCHECK=1` = old, `HWCHECK=verify` = checked + every hardware load logged) | .text 6.82 -> 5.31 MB (-22%), .3dsx 7.06 -> 5.54 MB. **HW runs 35a -> 35b (2v2 replay, two builds back to back): 2v2 game 22.24 -> 20.25 ms (-9.0%; faster in 74 of 74 paired windows, sd 1.3), 40.0 -> 43.4 fps (67 -> 72%); instructions 6.52 -> 6.07M per frame, I-cache misses 243k -> 215k (-12%), busy cycles 19.18 -> 17.79M; campaign 8.3 game 7.3 -> 6.6 ms.** Citra game: 2v2 4.2 36.2 -> 33.8 ms (-6.6%), battle 14.0 -> 13.1, campaign 8.0 -> 7.6. Verify build: 0 hardware loads in the boot, battle, campaign and 2v2 replays; logs + fb dumps identical checked vs unchecked | default on | Loads without the hardware check |
| 10-26 | **Hardware test build 2v2-perf-3** (branch task/2026-10-26-2v2-perf-3: particle probe, pointer-extent promotion, lazy vertex descriptor, sin/cos memo + quad merge studies, inline sites). Pointer-extent promotion and the inline sites became run-time toggles: recomp.py `RECOMP_PTR_EXTENT=dual` gives the 16 hottest of the 142 changed functions (`PTR_EXTENT_DUAL`, >= 0.05% of FNPROF samples in the 2v2/battle replays) a second body `fp_X`, `f_X` dispatches on `recomp_ptr_extent` at entry (all 142 would be +15.7% .text); `RECOMP_INLINE_SITES=dual` = `if (recomp_inline_sites) fi_X(); else f_X();` at the 5 sites | Citra 2v2 4.2 game (r4500+): plain build 36.6, toggles on 36.2 (-1.1%), off 36.7 (main ad99a97: 36.7); battle 4.2 14.0 / 14.1 (main 14.0); campaign 8.3 8.0 / 8.0 (main 8.0); .text 6,535,672 (plain) -> 6,820,008 (+4.35%: fp_ bodies 210 KB, inline copies + dispatch 75 KB); 2v2/battle/campaign/boot SDK logs + fb dumps identical to main with all toggles on and all off | on; buttons "Ptr extent" / "Inline sites", `cr_ptrext_off.txt` / `cr_inline_off.txt`; not yet on hardware | Run-time translator toggles |
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
| 10-26 | **gen/ without pre-allocation scheduling** (D7: `make GENOPT=-fno-schedule-insns` on main + D5, to cut register pressure and spills) | .text 4.25 -> 4.21 MB (-0.9%). **HW runs 41a -> 41b: 2v2 game 19.66 -> 19.75 ms (+0.09), busy cycles +0.16M (more in 73 of 73), instructions -0.8% but I-cache misses 33.7 -> 35.1 per 1000; campaign +0.23 ms** | The in-order ARM11 needs GCC's pre-RA scheduling to hide load and VFP latency more than it gains from the spills it causes. Stays a GENOPT choice only |
| 10-26 | **gen/ at -Os** (D6: `make GENOPT=-Os` on main + D5) | .text 4.25 -> 3.95 MB (-7%), 2v2 hot set (185 functions) 76.3k -> 70.2k ARM instructions; Citra logs + fb identical, Citra 2v2 game 32.1 -> 33.4 ms. **HW runs 40a -> 40b: 2v2 game 19.54 -> 20.26 ms (+0.72), busy cycles +0.50M, instructions +1.8%, I-cache misses per 1000 instructions unchanged (33.7 -> 34.0); campaign +0.26 ms** | Smaller code did not miss less: the misses are in loops whose working set -Os does not shrink, while it adds instructions (no if-conversion, more branches). Stays a build option only |
| 10-26 | **GX pipe stores as a call** (D4, branch task/2026-10-26-fifo-call @16fd3a4: the 1272 inline WGPIPE appends in gen/ became `mov r0, v` + `bl` to tiny asm routines in runtime/fifo_put.c that keep every register but r0-r2/ip/lr; `make FIFOCALL=0` = inline) | .text 4.27 -> 4.18 MB (-2.2%); Citra logs + fb identical, Citra 2v2 game 32.3 -> 32.7 ms (+1.2%). **HW runs 38a -> 38b (2v2 replay): game 20.05 -> 20.40 ms (+0.34, median +0.50), busy cycles +0.37M (more in 66 of 73 windows); campaign +0.15 ms** | Slower on hardware: the call and return, and the routine's loads of the FIFO count and limit at every store, cost more than the 90 KB of code saved; the pipe stores sit in loops that stay in the I-cache anyway. Not merged |
| 10-26 | **Cold-path partitioning** (`cold` on hw_read/hw_write/recomp_bad/recomp_log.., `-freorder-blocks-and-partition` on gen/, .text.unlikely linked first) | GCC 16.1 for ARM: "-freorder-blocks-and-partition not supported on this architecture", so nothing was split; `cold` alone +1.4% code (recomp_036.c), with a WGPIPE test ahead of the cold hw_write +8.1% (.text 6.82 -> 7.89 MB) | not possible on this target; would only grow the hot code. GCC already places most __builtin_expect'ed hardware paths near the function end. Not built for hardware (unknowns "Cold-path partitioning") |
| 10-26 | **Pointer-extent stack promotion** (translator: frame words outside every byte range reached through a frame pointer, with per-callee r3-r12 extents and `tools/recomp/native_ptr_ext.tsv` for natives; 3439 -> 4578 words) | Citra 2v2 game 36.0 -> 35.6 ms (-1.1%, 2 runs), battle 14.0 -> 13.9, campaign 8.0 -> 7.9; .text -0.7%; logs + fb identical (branch ptr-extent-promo) | below the 2% bar: the hot particle frames' remaining words are buffers really passed to PSMTX*/OSReport. Off by default; `RECOMP_PTR_EXTENT=1` at generation turns it on. Not on hardware. 2026-10-26: in the test build as a run-time toggle (hottest 16 functions; see "Hardware test build 2v2-perf-3") |
| 10-26 | Memo for repeated sin/cos arguments in `runtime/libm.c` (last-value, direct-mapped table, sincos pair) | measured only (Citra 2v2 probe): 2350 calls/frame, 63% repeat in the frame, but half the calls are x ~ 0 (tiny path, ~ the cost of a lookup); best shape ~0.1-0.15 ms of 36.7 (~0.3%) | below the 2% bar; not built (unknowns "sin/cos memo study") |
| 10-26 | Selective inlining of the per-particle helpers (`RECOMP_INLINE_SITES=1`: 5 listed bl sites to ps_disp_tev_setup / chan_color / tev_color / ps_disp_quad call an always_inline copy; logs + fb dumps identical) | Citra 2v2 game 36.72 -> 36.64 ms (-0.2%), +0.63% .text (ps_disp sites alone -0.02 ms) | below the 2% bar; the `cpu` stores stay (unknowns "Selective inlining of the per-particle helpers"). Not on hardware. 2026-10-26: in the test build as a run-time toggle |
| 10-26 | Particle quad merge (immediate draws differing only in TEV colour, via a palette texture) | measured only: TEV-only causes ~37 of ~113 immediate batch breaks per 2v2 frame; upper bound ~1.3 ms of the ~40 ms Citra GX thread | not built: not the main breaker, needs a third texture unit + texenv rewrite (unknowns "Particle quad merge") |
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

- **2v2 battle is game-CPU bound** (game 22.2 ms of a 24.8 ms frame, GX thread 15.6 ms, run 32a). Path so far:
  run 28 29.8 ms (30.5 fps) -> run 29 22.2 ms (39.6 fps; unknowns "2v2 game CPU", "2v2 game CPU 2") -> run 32a
  22.2 ms (40.3 fps; 2v2-perf-3, -1.5% vs its toggles off in run 32b). The particle system is 7.0 ms of the
  Citra 2v2 game thread and scales with particle count (unknowns "2v2 particle family").
  **Closed for small fixes (2026-10-26, user decision):** the 2v2-perf-3 round (particle probe plus five
  experiments: pointer extents, lazy vertex descriptor, sin/cos memo, particle inlining, particle quad merge)
  gave -1.5% game time on hardware, and each item was 0.2-1.1% in Citra. No more micro-optimisation in this
  area (particle family, call glue, translator peepholes for the 2v2). The game thread needs ~6 ms less for
  60 fps, which only a structural change could give. Do not reopen these items without a new idea of that size.
- **Where the 2v2 game thread's hardware time goes** (2026-10-26, hardware run 33 with "Perf counters"; unknowns
  "Hardware performance counters"): core 0 is busy 19.1M cycles (23.8 ms) per 2v2 frame, IPC 0.35. **27.6% of the
  cycles (~6.6 ms) wait for instructions** (243k I-cache misses per frame, 36.6 per 1000 instructions), 26.6%
  (~6.3 ms) wait on data dependencies (load-use, VFP latency; D-cache read misses only 3%), and the rest issues 6.65M
  instructions. The call glue is ~0.73M of them, ~5% of the time (`tools/callcost.py`, unknowns "Call-boundary cost
  on the ARM side"; Citra FNPROF said 21.8%): a calling-convention redesign is capped near 1 ms.
  The hot code is far larger than the 16 KB I-cache: 446 translated functions (1.5 MB) are entered at least once
  per 2v2 frame, 185 (526 KB) at least 10 times, spread over 5.7 MB of .text; 402k guest instructions became
  1.52M ARM instructions (3.8 each). Cold-path splitting is not available (GCC on ARM rejects
  -freorder-blocks-and-partition; see "Tried and not kept"), and GCC already puts most unlikely hardware-access
  paths near each function's end (78% of those calls in the last 30% of the function). Hardware run 34 (TLB counters): 0.22 main-TLB misses per 1000 instructions, under 1% of
  the cycles, so function order buys nothing through the TLB and the I-cache misses are capacity misses. D1 (loads
  without the hardware check, runs 35a/35b): 2v2 game 22.24 -> 20.25 ms, the first density step. D2 (write
  tracking through a byte map, branch task/2026-10-26-wr-tracking): -2.5% hot instructions, runs 36a/36b: 2v2 game -0.18 ms
  (-0.9%), busy cycles -1.0%, of which the per-frame sweep gives back ~0.12 ms. D3 (stores
  without the hardware check outside recomp_hw_*.c, branch task/2026-10-26-store-hwcheck): -14.5% hot instructions,
  runs 37a/37b: 2v2 game -0.46 ms (-2.3%), busy cycles -2.2%. D5 (float locals, runs 39a/39b): -0.13 ms, cycles
  -1.2%. Not kept: D4 (pipe stores as calls, +0.34 ms), D6 (-Os, +0.72 ms), D7 (-fno-schedule-insns, +0.09 ms).
  **Density steps done (2026-10-26):** 2v2 game 22.24 (run 35a) -> 19.6 ms (run 41a), 40.0 -> 44.7 fps. What is
  left in the hot code (2v2 hot set, 185 functions, ~76k ARM instructions = ~300 KB against the 16 KB I-cache):
  guest-access helpers rd32_b 10% / wr32_b 5.6% / stack 7% (mask + byte swap, about minimal), WGPIPE appends 6%,
  compares 8%; core 0 still waits ~26% of its cycles for instructions and ~26% on data. Shrinking code with
  slower instructions loses (D4, D6): the misses are in loops whose working set did not get smaller. Ideas not
  yet tried: profile-guided per-function choices (hot functions only), keeping guest RAM word-swapped (no `rev`
  on 32-bit accesses; touches every runtime/ reader of guest memory), a cheaper write-tracking sweep (~0.12 ms),
  moving work off the game thread (GX recording, audio) onto core 2/3. Layout
  A/Bs use two builds run back to back on the 2v2 hardware replay (user decision 2026-10-26); `tools/hw_ab.sh`
  runs one unattended through cr_agent. GX-thread work
  stays second: 15.6 ms, under the 16.7 ms budget.
- Render skip when a frame runs late (keep game speed at 100% by drawing every other frame),
  needed most with stereo 3D.

## Hardware run index

| Run | Date | Build | Log (`logs/`) | Tested |
|---|---|---|---|---|
| 35b | 10-26 | branch load-hwcheck @1059b28 (default: loads unchecked) | cr_autotest_2026-10-26_hw-run35b-replay-2v2-unchecked.log | B of the D1 A/B, run right after 35a with the same save and replay: route identical. 2v2 4.2: 43.4 fps (72%), game 20.25 ms, frame 23.0, worst 50.6; per frame 17.79M busy cycles, 6.07M instructions (IPC 0.34), I-miss 35.4/k (215k), stall inst 26.5%, stall data 26.5%. Paired with 35a: -1.99 ms per window, faster in 74 of 74. Campaign 8.3 game 6.6 ms, 59.4 fps |
| 36a | 10-26 | branch wr-tracking @3792cb4 WRMAP=0 (old per-store epoch; code = run 35b) | cr_autotest_2026-10-26_hw-run36a-replay-2v2-wrepoch.log | A of the D2 A/B (2v2 replay, cr_perfctr): route identical to 35b. 2v2 4.2: 42.1 fps, game 20.94 ms (median window as 35b), 17.79M busy cycles and 6.07M instructions (= 35b). Three windows had 626-813 ms frames: SD reads of sound/v_cubog09.hps at 130-270 ms per 16 KB while the log writer was busy (cr_dvdasync_off = synchronous DVD), so its mean game ms is +0.70 over 35b with the same cycles. Campaign 8.3 game 6.6 ms |
| 36b | 10-26 | branch wr-tracking @3792cb4 (default: byte map + sweep) | cr_autotest_2026-10-26_hw-run36b-replay-2v2-wrmap.log | B of the D2 A/B, right after 36a, same save and replay: route identical. 2v2 4.2: 43.8 fps (73%), game 20.07 ms, frame 22.8; 17.62M busy cycles, 6.03M instructions. Paired with 35b (clean baseline): game -0.18 ms (sd 0.11, faster in 65 of 73), cycles -0.17M (fewer in 73 of 73); with 36a: median -0.20 ms, cycles -0.16M (73 of 73). Sweep 117-121 us per 2v2 frame (~700-780 pages), 73-99 us elsewhere. Campaign 8.3 game 6.7 ms (+0.1, cycles -0.02M) |
| 37a | 10-26 | branch store-hwcheck @2b40f70 STORECHECK=1 (all stores checked; D3 file layout) | cr_autotest_2026-10-26_hw-run37a-replay-2v2-storecheck.log | A of the D3 A/B, the first run driven by tools/hw_loop.py (nobody at the console; --wait 420): route identical to 36b. 2v2 4.2: 43.5 fps, game 20.26 ms (36b: 20.17 in the same windows, +0.09 from the new file layout or noise), 17.69M busy cycles, 6.03M instructions, I-miss 35.2/k. Campaign 8.3 game 6.6 ms |
| 37b | 10-26 | branch store-hwcheck @2b40f70 (default: stores unchecked outside recomp_hw_*.c) | cr_autotest_2026-10-26_hw-run37b-replay-2v2-storefast.log | B of the D3 A/B, right after 37a (hw_loop, --wait 300), same save and replay: route identical. 2v2 4.2: 44.4 fps (74%), game 19.80 ms, frame 22.5; 17.31M busy cycles, 5.95M instructions, I-miss 33.6/k, stall inst 25.9%. Paired with 37a: -0.46 ms (median -0.50, sd 0.13, 72 of 73 faster), cycles -0.39M (73 of 73). Campaign 8.3 game 6.49 ms (-0.13) |
| 38a | 10-26 | branch fifo-call @3875743 FIFOCALL=0 (= main db5f1ac code) | cr_autotest_2026-10-26_hw-run38a-replay-2v2-fifoinline.log | A of the D4 A/B (hw_loop): route identical to 37b. 2v2 4.2: game 20.05 ms, 43.8 fps. Campaign 8.3 6.48 ms. The next app switch after it (ftpd started 6 s after leaving ftpd, ~10th netload since boot) froze the 3DS and dropped its Wi-Fi: reboot |
| 38b | 10-26 | branch fifo-call @3875743 (default: pipe stores as calls) | cr_autotest_2026-10-26_hw-run38b-replay-2v2-fifocall-froze.log | B of the D4 A/B after the reboot: route identical, the game reached its run length, but boot had four 5-6.5 s stalls (SD reads, one GX wait; 38a: 0.2-0.5 s) and the 3DS froze with Wi-Fi gone after the game exited (log fetched after another reboot). 2v2 4.2: game 20.40 ms (+0.34 vs 38a), busy cycles +0.37M: D4 not kept |
| soak-01 | 10-26 | branch 3ds-crash-hunt (main 762b50c + hw_return) | cr_autotest_2026-10-26_hwagent-soak-01.log | Agent loop check (tools/hw_loop.py through cr_agent, no InputRedirection/netloader/ftpd), 2v2 replay, back to back; route identical to 37b. 2v2 4.2 game 19.7 ms (time_avg), campaign 8.3 6.5 ms; no stall > 0.6 s |
| soak-02 | 10-26 | branch 3ds-crash-hunt (main 762b50c + hw_return) | cr_autotest_2026-10-26_hwagent-soak-02.log | Agent loop check (tools/hw_loop.py through cr_agent, no InputRedirection/netloader/ftpd), 2v2 replay, back to back; route identical to 37b. 2v2 4.2 game 19.7 ms (time_avg), campaign 8.3 6.5 ms; no stall > 0.6 s |
| soak-03 | 10-26 | branch 3ds-crash-hunt (main 762b50c + hw_return) | cr_autotest_2026-10-26_hwagent-soak-03.log | Agent loop check (tools/hw_loop.py through cr_agent, no InputRedirection/netloader/ftpd), 2v2 replay, back to back; route identical to 37b. 2v2 4.2 game 19.6 ms (time_avg), campaign 8.3 6.5 ms; no stall > 0.6 s |
| soak-04 | 10-26 | branch 3ds-crash-hunt (main 762b50c + hw_return) | cr_autotest_2026-10-26_hwagent-soak-04.log | Agent loop check (tools/hw_loop.py through cr_agent, no InputRedirection/netloader/ftpd), 2v2 replay, back to back; route identical to 37b. 2v2 4.2 game 19.7 ms (time_avg), campaign 8.3 6.5 ms; no stall > 0.6 s |
| 39a | 10-26 | main @9e688b9 (D3 code + hw_return) | cr_autotest_2026-10-26_hw-run39a-replay-2v2-main.log | A of the D5 A/B, the first A/B through cr_agent (no InputRedirection, netloader or ftpd). 2v2 4.2: game 19.81 ms, 44.4 fps; against run 37b (same code, netloaded) +0.01 ms (sd 0.13): both loops measure the same. The first log download, 4 s after the agent restarted, stalled 60 s; the retry a minute later worked (hw_loop.py now waits 10 s and retries once) |
| 39b | 10-26 | branch float-locals @d1d6633 (D5) | cr_autotest_2026-10-26_hw-run39b-replay-2v2-floatlocals.log | B of the D5 A/B, right after 39a: route identical. 2v2 4.2: game 19.68 ms, 44.6 fps, 17.06M busy cycles, 5.93M instructions; paired with 39a -0.13 ms (median -0.20), cycles -0.20M (73 of 73). Campaign 8.3 game 6.40 ms (-0.11) |
| 40a | 10-26 | branch float-locals @d1d6633 (D5) | cr_autotest_2026-10-26_hw-run40a-replay-2v2-floatlocals.log | A of the D6 A/B (cr_agent): same build as 39b, 2v2 game 19.54 ms (perf_avg; 39b 19.58): repeatable |
| 40b | 10-26 | D5 + GENOPT=-Os | cr_autotest_2026-10-26_hw-run40b-replay-2v2-os.log | B of the D6 A/B: route identical; 2v2 game 20.26 ms (+0.72), 17.56M busy cycles (+0.50M), 6.03M instructions. -Os not kept |
| 41a | 10-26 | main @35e6839 (D5 merged) | cr_autotest_2026-10-26_hw-run41a-replay-2v2-floatlocals.log | A of the D7 A/B (cr_agent). 2v2 4.2: 44.7 fps (74%), game 19.6 ms, frame 22.4, GX thread busy 15.8, GPU 2.1; campaign 8.3 59.0 fps, game 6.4 ms: the "Where it stands" numbers |
| 41b | 10-26 | main + GENOPT=-fno-schedule-insns | cr_autotest_2026-10-26_hw-run41b-replay-2v2-nosched1.log | B of the D7 A/B: route identical; 2v2 game +0.09 ms, cycles +0.16M: not kept |
| 35a | 10-26 | branch load-hwcheck @1059b28 HWCHECK=1 (checked loads, = main's code) | cr_autotest_2026-10-26_hw-run35a-replay-2v2-hwcheck.log | A of the D1 A/B (2v2 replay, cr_perfctr): route identical to run 34. 2v2 4.2: 40.0 fps, game 22.24 ms, frame 25.0; 19.18M busy cycles, 6.52M instructions, I-miss 37.3/k, stall inst 27.6%, stall data 26.1% (as runs 33-34). Campaign 8.3 game 7.3 ms. A hand-played campaign session on ade9b5e before it is kept as cr_autotest_2026-10-26_hw-play-ade9b5e-campaign-perfctr.log |
| 34 | 10-26 | branch icache-cold-paths @ade9b5e | cr_autotest_2026-10-26_hw-run34-replay-2v2-tlb.log | run 33 repeated with the TLB counter pair: route identical, game 22.0 ms; I-miss 37.4/k (33: 36.6), stall inst 27.56% (33: 27.56%); instruction micro-TLB 0.15/k, main TLB 0.22/k misses (< 1% of cycles even at 100 cycles each): TLB is not a cost, function order is not a lever |
| 33 | 10-26 | branch perf-counters @1838462 | cr_autotest_2026-10-26_hw-run33-replay-2v2-perfctr.log | the 2v2 hardware replay (as 32a, all toggles on) with cr_perfctr: route identical to 32a, game 21.9 ms / frame 24.7 (32a by the same tool: 21.9 / 24.6). Core 0 per 2v2 frame: 19.1M busy cycles, 6.65M instructions (IPC 0.35), stall for instructions 27.6% (243k I-cache misses), stall for data 26.6%, D-cache read misses 3%. Campaign 8.3: IPC 0.38, I-stall 20.8%, data stall 27.8% |
| 32b | 10-26 | branch 2v2-perf-3 @c4781f5 | cr_autotest_2026-10-26_hw-run32b-replay-2v2-all-off.log | hardware replay of the Citra 2v2 test (data/saves/2026-10-22_pre-2v2 + tools/citra/pad_2v2.txt / pad_2v2.log, run length 8600, cr_dvdasync_off), with cr_lazydesc_off + cr_ptrext_off + cr_inline_off: route identical to Citra and to 32a (0 scene / pad line differences, no late DVD read). 2v2 4.2 (from r4300, 72 windows): 39.7 fps (66%), game 22.5 ms, GX busy 15.8, worst 53.9 |
| 32a | 10-26 | branch 2v2-perf-3 @c4781f5 | cr_autotest_2026-10-26_hw-run32a-replay-2v2-all-on.log | same replay, all three toggles on: 40.3 fps (67%), game 22.2 ms, GX busy 15.6, worst 53.1. Paired by window vs 32b: game -0.34 ms (sd 0.14, faster in 71 of 72 windows) = -1.5%, matching Citra's -1.1%. So run 31's -7% was mostly play variance. The first attempt (32a0, @e51105e, log cr_autotest_2026-10-26_hw-run32a0-replay-2v2-diverged-dvd-late.log) left Citra's route at r3720: the real circle pad rests at -5 -3, which counted as a moved live stick and overrode the replay's sticks; fixed in pad.c (c4781f5) |
| 31 | 10-26 | branch 2v2-perf-3 @e51105e | cr_autotest_2026-10-26_hw-run31-2v2-perf-3.log | all three toggles on the whole run (lazyd, ptrx, inl; no on/off A/B). 2v2 battle 4.2: 41.8 fps (70%), game 20.6 ms, GX busy 16.3, worst 40.3 (run 29: 39.6 fps, game 22.2, GX 16.6, worst 50.1); hand-played, and Citra predicted only -1.1%, so most of the -7% is likely play variance. Title, menus, campaign 8.3 (58.8 fps) and 1v1 as run 29. No crash. Scene c average 31.9 fps comes from one 9.5 s stall (r11086 gx sync 5981 ms, r11087 game 3569 ms) right after a p2 button press, followed by a HOME exit (APT); without it 59.8 fps |
| 30 | 10-25 | branch 2v2-perf-2 @40381bd | cr_autotest_2026-10-25_hw-run30-t06-text-hang.log (+ hw-run30-shots/) | story event snr_t_06 hangs ("Awex!" box without text, A/B do nothing). A Rosalina pause at r14951 made real-time audio pacing skip 807 AX frames, but that is not the cause: Citra replays with an injected 4.3 s stall (cr_ax_stall.txt, real-time pacing) play through, and the screenshot taken during the pause already shows the previous box broken (unknowns "Run 30 text hang"). Same build, save and event run fine in Citra. Not a 2v2-perf-2 change; cause open |
| 29 | 10-23 | branch 2v2-perf-2 @40381bd | cr_autotest_2026-10-23_hw-run29-2v2-perf2.log | 2v2 battle 4.2: 39.6 fps (66%), game 22.2 ms, GX busy 16.6, gpu 2.6 (run 28: 30.5 fps, game 29.7, GX 18.6); campaign 8.3 58.5 fps, game 11.0 |
| 28 | 10-22 | main@75ea35d LTO | cr_autotest_2026-10-22_hw-run28-merged-lto.log | VRAM textures, stereo 3D, FPSCR value, 2v2 battle |
| 27 | 10-22 | main 8575bae | cr_autotest_2026-10-22_hw-run27-clean-main.log | campaign GPU clean build |
| 26 | 10-22 | campaign-gpu | cr_autotest_2026-10-22_hw-run26-campaign-gpu-vsh-single.log | culling, vertex shader |
| 25 | 10-22 | perf-test bcd754a | cr_autotest_2026-10-22_hw-run25-perf-branches.log | codegen, model scaling, call glue |
| 24 | 10-21 | | cr_autotest_2026-10-21_hw-run24-gx-thread.log | GX thread |
| 20-23 | 10-21 | | cr_autotest_2026-10-21_hw-run2x-*.log | GPU kick, DVD thread, movie |
| 4-19 | 10-18/19 | | on the SD card root (`cr_autotest_2026-10-1x_hw-run*.log`) | see CLAUDE.md status |
