# custom-robo-recomp

Static recompilation of Custom Robo (GameCube, USA, GXCE01) for the New 3DS.

The game's own code is translated from your disc's `main.dol` into C by the
translator in `tools/recomp/`. The GameCube SDK layer (graphics, disc, pad, OS,
audio, memory card, video) is rewritten by hand for the 3DS in `runtime/`, on
libctru and citro3d. No game files are included: bring your own disc.

## Status (2026-10-21)

Playable on a New 3DS from boot to the title, menus, campaign and Vs. battle,
with sound, memory card saves, the attract movie and the HOME button.

Frame rates on hardware (New 3DS, averages of the in-game timing log, hardware
run 24 with the GX thread on core 2; before it, in brackets):

| Scene | fps | Game speed |
|---|---|---|
| Title, menus | 49-60 | 82-100% |
| Vs. battle | ~48 (~35) | ~80% (~58%) |
| Campaign | ~42 (~30) | ~71% (~50%) |
| Attract movie | ~9 | ~30% |

The game runs one logic step per frame, so game speed is fps / 60: at 48 fps a
battle plays in slow motion at 80% of real time. 100% needs a steady 60 fps.
The movie is measured against its own 30 fps (3446 frames in ~115 s); it was
not played in run 24.

Where the time goes now: Vs. battle is CPU-bound (the translated game code is
~19 of a ~21 ms frame; the GX work runs in parallel on core 2). Campaign is
GPU-bound: the GPU needs 16-21 ms per frame there, so the GX thread gains
little in campaign.

Known problems:

- Audio stutters whenever the game runs below 60 fps: audio is produced per game
  frame instead of in real time (fix in progress).
- Music can repeat a 1-2 s segment when the SD card stalls for several seconds
  (under investigation).
- The attract movie is slow: its JPEG frames are decoded on the CPU.
- Missing graphics features: EFB-to-texture copies, back-face culling,
  lines/points, specular (COLOR1) lighting, post-transform texture matrices,
  anti-aliasing. See the GX sections of `docs/recomp_unknowns.md`.
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
the build and any flag files present; its last line always shows the two A/B
toggles as they are now (`ZL gpukick ON   tap gx thread ON`).

## Controls

| 3DS | GameCube |
|---|---|
| Circle pad | Control stick |
| C-stick (New 3DS) | C stick |
| D-pad, A, B, X, Y, L, R, Start | same |
| ZR | Z |
| Hold SELECT | input goes to controller 2 (Vs. mode) |
| ZL | toggles mid-frame GPU submit (A/B testing) |
| Tap the bottom screen | toggles the GX thread (A/B testing; New 3DS only) |

## Test flags

Empty files in the SD card root change one behaviour each, so a single build
can be compared with a feature on and off:

| File | Effect |
|---|---|
| `cr_gxthread_off.txt` | start with the GX thread off: GX work on the game thread (a tap toggles it) |
| `cr_gxthread_fliptest.txt` | switch the GX thread on/off every 300 retraces (tests the switch) |
| `cr_gpukick_off.txt` | start with mid-frame GPU submit off (ZL toggles it) |
| `cr_batch_off.txt` | no draw batching |
| `cr_dlcache_off.txt` | no display-list cache |
| `cr_dlclean_off.txt` | always re-hash display lists and textures (no write tracking) |
| `cr_mtx_off.txt` | translated MTX functions instead of the native ones |
| `cr_vsync_on.txt` | old pacing: wait for a vblank every frame |
| `cr_dvdasync_off.txt` | wait for every disc read to finish (old timing) |
| `cr_dvd_stalltest.txt` | every 64th music read sleeps 3 s (tests late reads) |
| `cr_card_off.txt` | no memory card |
| `cr_bench.txt` | MTX benchmark lines in the log at startup |
| `cr_fbdump.txt` | frame numbers, one per line: the framebuffer is saved as `cr_fb_<frame>.bmp` |
| `cr_audio_dump.txt` | also write the audio mix to `cr_audio.wav` |
| `cr_pad_script.txt`, `cr_pad_replay.txt` | scripted input or replay of a logged run (autotests; fixed run length) |

Citra autotests: `tools/citra/autotest.ps1`.

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

- `docs/recomp_unknowns.md`: semantics decisions, assumptions, open questions,
  and every performance change with its hardware and Citra measurements.
- `docs/sdk_functions.md`: the SDK functions in `main.dol` (address, name,
  module) and which ones are hand-written.
- `CLAUDE.md`: project rules and the development status log.
