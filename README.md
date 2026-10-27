# custom-robo-recomp

Static recompilation of Custom Robo (GameCube, USA, GXCE01) for the New 3DS.

The game's own code is translated from your disc's `main.dol` into C by the
translator in `tools/recomp/`. The GameCube SDK layer (graphics, disc, pad, OS,
audio, memory card, video) is rewritten by hand for the 3DS in `runtime/`, on
libctru and citro3d. No game files are included: bring your own disc.

## Status (2026-10-26)

Playable on a New 3DS from boot to the title, menus, campaign and Vs. battle,
with sound, memory card saves, stereoscopic 3D, the attract movie and the HOME button.

Frame rates on hardware (New 3DS, averages of the in-game `# time` windows from
`tools/time_avg.py`). Each run was played by hand, so the scenes differ from run to run:

| Scene | Run 24 (10-21) | Run 25 (10-22) | Run 27 (10-22) | Run 28 (10-22) | Run 29 (10-23, 2v2 CPU work, now in main) | Game speed now |
|---|---|---|---|---|---|---|
| Title, menus (2.x, 5.0) | 49-60 | ~60 | 58-60 | 58-60 | not recorded | 97-100% |
| Campaign (8.3) | 42.4 | 44.4 | 51.2 | 56.7 (39 windows) | **58.5** | 97% |
| Battle 1v1 (4.2) | 47.9 | 54.2 | 45.3 (campaign battles) | **57.3** (19 windows) | not played | 95% |
| Battle 1v1, 3D slider up | - | - | - | 51.4 (20 windows) | not played | 86% |
| Battle 2v2 (4.2) | - | - | - | 30.5 (72 windows) | **39.6** | 66% |
| Pause (c.0) | 40.1 | 55.8 | 59.1 | 57.5 (2 windows) | not played | 96% |
| Attract movie | ~9 | 13.9 | not played | not played | not played | ~46% |

The game runs one logic step per frame, so game speed is fps / 60: at 48 fps a
battle plays in slow motion at 80% of real time. 100% needs a steady 60 fps.
The movie is measured against its own 30 fps.

Where the time goes now (run 28; 2v2 from run 29; ms per frame):

| Scene | frame | game CPU | GX thread busy | GPU | bound by |
|---|---|---|---|---|---|
| Campaign 8.3 | 17.6 | 9.9 | 16.1 | 3.7 | near 60 fps |
| Battle 1v1 | 17.5 | 15.1 | 12.2 | 2.2 | near 60 fps |
| Battle 1v1, 3D | 19.5 | 14.5 | 16.5 | 9.8 | GPU + CPU |
| Battle 2v2 (run 29) | 25.3 | 22.2 | 16.6 | 2.6 | **game CPU** |

Textures in VRAM (2026-10-22) cut GPU time to under a third: campaign 12.2 -> 3.7 ms, battle
9.3 -> 2.2 ms (runs 27 -> 28; the battles were different fights, so that one is rough). The 2v2 battle is now the heaviest scene, and it is bound by the
game's own code: 29.8 ms per frame in run 28, 22.2 ms after the 2v2 CPU work (run 29: 30.5 -> 39.6 fps). Every performance change and its measured effect, including
the ones that did not help, is listed in `docs/performance.md`.

2v2-perf-3 (merged to main 2026-10-26, from `task/2026-10-26-2v2-perf-3`): lazy vertex descriptor, pointer-extent
stack promotion and inlined particle helpers, each with a bottom-screen button, all three on by default. This
closes small-scale 2v2 CPU work: the gains have run out (see docs/performance.md "Open"). In Citra the 2v2 game thread
takes 36.2 ms with all three on and 36.7 ms with them off (main: 36.7). Hardware run 31 (all on, hand-played):
2v2 41.8 fps, game 20.6 ms, against 39.6 fps / 22.2 ms in run 29 (main's code), mostly play variance. Hardware
runs 32a/32b replay the Citra 2v2 test on the 3DS (identical fight): all three on 40.3 fps, game 22.2 ms; all
off 39.7 fps, 22.5 ms. A real but small gain (-1.5% game time, faster in 71 of 72 one-second windows).

Measuring the 2v2 game thread (2026-10-26, branch `task/2026-10-26-perf-counters`): the cost at translated
call boundaries (register stores to `cpu` around each call, reloads at entry, return stores, push/pop) is
~725k ARM instructions per 2v2 frame (`tools/callcost.py`: main's ELF weighted by the replay's call counts),
estimated ~1.1 ms, about 5% of the 22.2 ms hardware game thread. Citra's FNPROF had put it at 21.8%, so the
other ~95% (function bodies, memory access helpers, C-stack spills, SDK code) is where the time is, and Citra
cannot say how it splits. The "Perf counters" button (or `cr_perfctr.txt`) logs the ARM11's own counters per
frame (cycles, instructions, I-cache and D-cache misses, stall cycles, branch mispredicts; `tools/perf_avg.py`
averages them per scene). Citra only stubs the counters. Hardware run 33 (the 2v2 replay with the counters on,
same route and timing as run 32a): per 2v2 frame the core is busy 19.1M cycles at 0.35 instructions per cycle;
27.6% of the cycles (~6.6 ms) wait for instructions (I-cache misses), 26.6% (~6.3 ms) wait on data dependencies.
The instruction-fetch stalls alone are about the ~5.5 ms the 2v2 is over 60 fps. Run 34 added TLB counters: TLB
misses cost under 1%, so where functions sit does not matter; the translated code has to get smaller on the hot paths.
The first step (guest loads without the hardware-address check, runs 35a/35b, two builds on the same replay): the
2v2 game thread went from 22.2 to 20.3 ms, 40.0 -> 43.4 fps (game speed 67 -> 72%), and the code from 6.8 to 5.3 MB.
The second (guest stores mark a byte map instead of loading and storing the write epoch, branch
`task/2026-10-26-wr-tracking`) cuts the hot code by another 2.5% (code 5.3 -> 5.1 MB): runs 36a/36b, 2v2 game 20.25 -> 20.07 ms (-0.9%).
The third (stores without the hardware check outside the 33 functions that set a hardware address, branch
`task/2026-10-26-store-hwcheck`) cuts the hot code by 14.5% (code 5.1 -> 4.3 MB); its hardware A/B is next.

Hardware replay: put the 2v2 save in sdmc:/custom-robo/save/ (back up the real one first), `cr_pad_script.txt`
("8600 0 0" plus the numeric lines of tools/citra/pad_2v2.txt), `cr_pad_replay.txt` (the "# pad r" lines of
tools/citra/pad_2v2.log) and `cr_dvdasync_off.txt` on the SD root, launch, hands off. The route must match the
Citra log's scene and pad lines before the timings count.

Known problems:

- A story text box can break (letters missing, even spacing) and the event then stops
  (seen once, hardware run 30 in event `snr_t_06`; not reproduced in Citra, cause unknown, see
  `docs/recomp_unknowns.md` "Run 30 text hang"). If it happens, tap "Dump RAM" on the bottom
  screen before quitting.
- Music can repeat a 1-2 s segment when the SD card stalls for several seconds
  (under investigation).
- The attract movie is slow: its JPEG frames are decoded on the CPU.
- Missing graphics features: EFB-to-texture copies, lines/points, specular
  (COLOR1) lighting, post-transform texture matrices, anti-aliasing. See the GX
  sections of `docs/recomp_unknowns.md`.
- Old 3DS models are untested; the build targets the New 3DS's 804 MHz mode.

## What you need

- A New 3DS / New 2DS XL with custom firmware and the Homebrew Launcher.
- `sdmc:/3ds/dspfirm.cdc` (dump it with DSP1), or there is no sound.
- Your own Custom Robo (USA) disc image.
- To build: Python 3 and devkitPro's devkitARM with libctru and citro3d
  (the `3ds-dev` group). The author builds in a devkitARM Docker image.

## Build

```
python tools/extract_disc.py "Custom Robo (USA).iso" data/extracted   # once
PYTHONUTF8=1 python tools/recomp/recomp.py                            # main.dol -> gen/
(cd tools/recomp && PYTHONUTF8=1 python gen_sdk_log.py)               # after adding an sdk_* function to runtime/
make -j20                                                             # -> custom-robo-recomp.3dsx
```

With Docker, from Git Bash on Windows:

```
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/proj" -w /proj <devkitarm-image> make -j20
```

Every build runs `tools/check_3dsx.py`, which fails the build on a relocation
the 3dsx loader would reject. `data/`, `gen/` and the build outputs are
git-ignored; never commit them (they contain the game).

## Install

Copy to the SD card:

```
sdmc:/3ds/custom-robo-recomp.3dsx
sdmc:/3ds/dspfirm.cdc
sdmc:/custom-robo/main.dol      <- data/extracted/sys/
sdmc:/custom-robo/boot.bin      <- data/extracted/sys/
sdmc:/custom-robo/fst.bin       <- data/extracted/sys/
sdmc:/custom-robo/files/...     <- the whole data/extracted/files/ tree
```

Start it from the Homebrew Launcher. Saves go to `sdmc:/custom-robo/save/`
(memory card in slot A). Each run writes `sdmc:/cr_autotest.log`, which is
overwritten at the next start; rename it to keep it. The bottom screen shows
the build id and any flag files present, then a fixed stats table, one metric per
row, updated once a second (the same numbers as the log's `# time` lines, in ms per
frame), then a 3-row message area. Example values:

```
fps         51.2  speed  85%
frame ms    19.5  worst  37.2
game ms      8.7
gx wait     10.1  thread busy 18.0
  parts   vtx  1.7 tex  1.3 state  0.8
gpu ms      12.2  draws   60 tris  18000
audio ms     0.7  dvd wait  0.0
scene          8  sub 3  r12345
io  late 0 slow 3 stall 0 lin 17714K
```

Touch buttons on the bottom screen switch test features live (the log records each switch,
and `tools/time_avg.py` splits the `# time` windows by mode): "VRAM textures", "Vertex pack",
"Lazy vtx desc", "Native sin/cos", "Ptr extent" (pointer-extent stack promotion in the 16 hottest
functions it changes) and "Inline sites" (the 5 inlined particle-helper calls), all on by default.
"Perf counters" (off by default) is a measurement, not a feature: while on, the log gets a `# perf` line every
60 retraces with the ARM11 core 0 hardware counters per frame (see Test flags, `cr_perfctr.txt`).
"Dump RAM" is an action, not a switch: it writes the
game's RAM (24 MB), ARAM (16 MB), the disc reads still in flight and the next frame to the SD card
root as `cr_dump_r<retrace>_*` and `cr_fb_<frame>.bmp` (the game stops for a few seconds).

## Controls

| 3DS | GameCube |
|---|---|
| Circle pad | Control stick |
| C-stick (New 3DS) | C stick |
| D-pad, A, B, X, Y, L, R, Start | same |
| ZR | Z |
| Hold SELECT | input goes to controller 2 (Vs. mode) |
| 3D slider | stereoscopic 3D on the top screen (0 = 2D, no extra cost) |

## Test flags

Empty files in the SD card root change one behaviour each, so a single build
can be compared with a feature on and off:

| File | Effect |
|---|---|
| `cr_gxthread_off.txt` | GX thread off: GX work on the game thread |
| `cr_gxthread_fliptest.txt` | switch the GX thread on/off every 300 retraces (tests the switch) |
| `cr_gpukick_off.txt` | mid-frame GPU submit off |
| `cr_batch_off.txt` | no draw batching |
| `cr_dlcache_off.txt` | no display-list cache |
| `cr_dlclean_off.txt` | always re-hash display lists and textures (no write tracking) |
| `cr_mtx_off.txt` | translated MTX functions instead of the native ones |
| `cr_vramtex_off.txt` | textures stay in linear memory (no VRAM textures) |
| `cr_vtxpack_off.txt` | every vertex in the full 68-byte layout (no vertex packing) |
| `cr_lazydesc_off.txt` | a GX-queue record for every GXSetVtxDesc/GXClearVtxDesc call (no lazy vertex descriptor) |
| `cr_libm_off.txt` | translated sin/cos/sinf/cosf/sqrtf instead of the native ones |
| `cr_ptrext_off.txt` | old stack-promotion bodies (no pointer-extent promotion; button "Ptr extent") |
| `cr_inline_off.txt` | plain calls at the 5 particle-helper sites (no inline copies; button "Inline sites") |
| `cr_perfctr.txt` | ARM11 hardware counters on from boot (button "Perf counters"): a `# perf` line every 60 retraces with core 0's cycles, instructions (IPC), I-cache misses, D-cache reads and read/write misses, stall cycles (instruction fetch, data hazard), branch mispredicts and instruction micro-TLB / main TLB misses per frame; two events count per frame, so the five pairs take turns. `tools/perf_avg.py` averages them per scene. Citra only stubs the counters ("no frame counted") |
| `cr_runfast.txt` | set the VFP's flush-to-zero + default-NaN mode at boot (already on: the boot log's `# fpscr:` line shows the value) |
| `cr_stereo_force.txt` | "slider [sep] [conv]": force stereo 3D (Citra, whose slider cannot be scripted) |
| `cr_vsync_on.txt` | old pacing: wait for a vblank every frame |
| `cr_dvdasync_off.txt` | wait for every disc read to finish (old timing) |
| `cr_dvd_stalltest.txt` | every 64th music read sleeps 3 s (tests late reads) |
| `cr_card_off.txt` | no memory card |
| `cr_bench.txt` | MTX benchmark lines in the log at startup |
| `cr_fbdump.txt` | frame numbers, one per line: the framebuffer is saved as `cr_fb_<frame>.bmp` |
| `cr_audio_dump.txt` | also write the audio mix to `cr_audio.wav` |
| `cr_audio_rt_off.txt`, `cr_audio_rt_on.txt` | audio paced per retrace (autotest default) or by real time (interactive default) |
| `cr_memdump.txt` | retraces, ascending, one per line: a "Dump RAM" snapshot at each (the Citra side of a comparison) |
| `cr_ax_stall.txt` | "retrace ms [sleep]": inject a stall like a HOME/Rosalina pause (Citra tests) |
| `cr_pad_script.txt`, `cr_pad_replay.txt` | scripted input or replay of a logged run (autotests; fixed run length) |

Build variants (a second `.3dsx` for a hardware A/B; the bottom screen's build id names
the variant): `make LTO=1` (link-time optimisation), `make GENOPT=-O3` (extra flags for the
translated code only), `make FLOATOPS=0` (exact Gekko float rounding), `make HWCHECK=1` (guest loads check
for hardware-register addresses again; by default they read RAM directly) and `make HWCHECK=verify` (checked, and
every load that reaches a hardware address is logged as `# hwcheck load`; the boot line `# hwcheck:` names the
mode), and `make WRMAP=0` (guest stores write the write-tracking epoch themselves instead of marking a byte map
that is swept once per frame; boot line `# wrmap:`) and `make STORECHECK=1` (every translated store tests for
hardware addresses again; by default only the functions in gen/recomp_hw_*.c do, and `HWCHECK=verify` also lists
every store that reaches a hardware address as `# hwcheck store` lines). Switching any of these needs `make clean` first.
Translator variants are chosen when gen/ is generated: `RECOMP_PTR_EXTENT=dual|1|0` and
`RECOMP_INLINE_SITES=dual|1|0` (recomp.py; `dual`, the default, builds both variants into the one
`.3dsx` behind the "Ptr extent" / "Inline sites" buttons; `0` is the plain main code).

Citra tests: `tools/citra/replay.ps1 -Name battle|campaign|2v2` plays recorded input
deterministically (see `docs/performance.md`, "How performance is measured");
`tools/citra/compare_logs.py` compares two runs. `2v2` boots the story save in
`data/saves/2026-10-22_pre-2v2/` (copy it there from your own SD card) and plays into a 2v2
battle, the heaviest scene so far. `-Name boot -Save <dir> -RecLog <log>` replays a
hardware log's pad lines from a save (how the run 30 story event was replayed).

Profiling tools: `tools/time_avg.py` (per-scene `# time` averages, split by toggle), `tools/perf_avg.py` (per-scene
`# perf` hardware counter averages next to game ms), `tools/callcost.py` (the call-boundary instructions of every
translated function in a build, weighted by a `RECOMP_SITEPROF=1` replay's call counts; workflow in its docstring).

With the 3DS on Wi-Fi and Rosalina's debugger enabled, `tools/hw_dump.sh [ip]` attaches gdb
to the running game and saves thread registers, backtraces and memory to `logs/hwdump/`.

## How it works

- `tools/recomp/recomp.py` turns every PowerPC function into a C function
  `f_XXXXXXXX` (guest registers as C locals, big-endian memory helpers,
  register spills per call site from interprocedural summaries, jump tables as
  switches). Calls to SDK functions go to the native versions in `runtime/`.
- `runtime/gx.c` maps GX onto citro3d: TEV stages to PICA texenv, transform,
  lighting and texgen in a PICA vertex shader, textures decoded and cached,
  vertex and display-list caches, draw batching, and mid-frame GPU submits so
  the GPU works while the CPU builds the frame. On a New 3DS this all runs on a
  GX thread on core 2: the game thread only records each GX call and its inputs
  into a queue, and waits for the GX thread at `GXDrawDone`.
- DVD reads come from the extracted files on the SD card (on a reader thread);
  AX audio is mixed on the CPU and played through ndsp; the memory card is a
  folder of files.
- Dolphin running the same disc is the reference: any difference is a runtime
  bug, never fixed by rewriting game logic.

## Docs

- `docs/performance.md`: every performance change, what it measured on hardware or in
  Citra, what is on, what was tried and dropped, and the index of hardware runs.
- `docs/recomp_unknowns.md`: semantics decisions, assumptions, open questions,
  and the full write-up of each performance change.
- `docs/sdk_functions.md`: the SDK functions in `main.dol` (address, name,
  module) and which ones are hand-written.
- `CLAUDE.md`: project rules and the development status log.
