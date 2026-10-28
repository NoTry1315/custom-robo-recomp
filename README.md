# Custom Robo Recomp

Static recompilation of *Custom Robo* (GameCube, USA, GXCE01) for the New Nintendo 3DS.
The game's PowerPC code is translated from `main.dol` into C, then built as native
ARM code for 3DS. This is a homebrew research project, not an emulator and not a
game download.

## Project overview

[▶ Watch the project overview (MP4)](./media/project-overview.mp4)

The translator preserves the original game code; the project does not hand-port game
logic. The runtime implements the GameCube SDK and hardware services needed by the
game—graphics, disc reads, controller input, operating-system services, audio, video,
and memory-card saves—using 3DS libraries. Dolphin running the same disc is the
reference for runtime behavior.

## Current status

The game boots and is playable on New 3DS hardware through the title, menus, campaign,
and Vs. battles. Sound, saves, the HOME button, stereoscopic 3D, widescreen, and the
attract movie are implemented. It is still an active compatibility project, not a
finished, fully verified release: some scenes run below full speed and graphics and
other issues remain.

The following are measured hardware results from the latest documented runs for each
scene. Results can vary with the fight and route. The latest music-stream-prefetch work
has a feature-specific hardware test, but these performance figures are not a full
hardware qualification of the current repository snapshot.

| Scene | New 3DS result | Notes |
|---|---:|---|
| Title and menus | About 60 fps | Near full speed |
| Campaign | 59.2 fps | Scene 8.3 |
| 1-v-1 battle, 2D | 57.3 fps | Scene 4.2 |
| 1-v-1 battle, stereoscopic 3D | 51.4 fps | 3D slider raised |
| 2-v-2 battle | 47.3 fps | Scene 4.2; CPU-limited |
| Attract movie | 44.2 fps | About 74% of its normal 30 fps; frames are not skipped |

The game advances one logic step per frame, so scenes below 60 fps run in slow motion.
The New 3DS is the target; original/Old 3DS models are untested.

### Known limitations

- The 2-v-2 battle and stereoscopic scenes do not yet hold 60 fps.
- The attract movie remains slower than real time due to video decoding and disc-read
  wait.
- Some graphics are not fully reproduced, including EFB-to-texture copies and
  specular lighting. Widescreen has known 2D edge artifacts and has not been checked
  frame-by-frame on hardware.
- One hardware run showed a story text box with missing letters followed by a hang.
  It has not been reproduced; its cause is unknown.
- SD-card stalls have occurred. Music is prefetched to reduce mid-song audio issues,
  but stalls at the start of a song are still possible.

See [Known problems and implementation details](./README.detailed.md#known-problems),
[performance measurements](./docs/performance.md), and
[open questions](./docs/recomp_unknowns.md) for the full record.

## What you need

- A New Nintendo 3DS or New 2DS XL with custom firmware and the Homebrew Launcher.
- Your own legally obtained *Custom Robo* USA GameCube disc (GXCE01) and a way to
  extract its files. The repository does not include the game, a disc image, or
  extracted game data.
- The 3DS DSP firmware file `dspfirm.cdc`, which is not included.

This project is not affiliated with or endorsed by Nintendo, Noise, or HAL Laboratory.
All game names and trademarks belong to their respective owners.

## Build and install

The build requires the devkitPro/devkitARM toolchain and the Python tools used to
extract and translate the disc. The high-level steps are:

1. Extract the disc into `data/extracted/`.
2. Run `tools/recomp/recomp.py` to generate translated C under `gen/`.
3. Build the `.3dsx` with `make`.
4. Put the `.3dsx`, `dspfirm.cdc`, and the extracted system and game files on the SD
   card in the paths documented in the detailed instructions.

The generated game data and build products are local-only and are not committed.
There is not yet a packaged, official release download; see the full
[build and installation guide](./README.detailed.md#build) before attempting a build.

## Testing

Changes are checked with deterministic Citra replays and compared by SDK-call, scene,
controller-input, framebuffer, and audio logs where applicable. Hardware performance
measurements and hardware-only checks are recorded separately; Citra does not model
New 3DS GPU performance or ARM11 cache behavior. See
[how testing and performance measurement work](./docs/performance.md#how-performance-is-measured)
and the [hardware run index](./docs/performance.md#hardware-run-index).

## AI use

AI tools were used as development aids for implementation, investigation, debugging,
test tooling, and documentation. The project maintainer makes the engineering
decisions and validates changes with Citra and, where available, physical 3DS
hardware. The original game's logic is not AI-written or manually ported: it comes
from the user's disc and is translated by this project's recompilation tools.

## Credits

- *Custom Robo* was developed by Noise and published by Nintendo. The game code and
  assets are supplied by the user from their own disc.
- [Dolphin](https://dolphin-emu.org/) provides the reference behavior.
- [devkitPro](https://devkitpro.org/), including devkitARM, libctru, and citro3d,
  provides the 3DS build and runtime libraries.
- [Citra](https://github.com/citra-emu/citra) is used for deterministic emulation
  tests; [libjpeg-turbo](https://libjpeg-turbo.org/) decodes the attract movie.
- Technical references and prior art include
  [YAGCD](https://www.gc-forever.com/yagcd/),
  [3dbrew](https://3dbrew.org/),
  [mw3ds](https://github.com/3ds-conspiracy/mw3ds),
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp),
  [ReXGlue](https://mintlify.wiki/rexglue/rexglue-sdk/concepts/recompilation),
  [N64Recomp](https://github.com/N64Recomp/N64Recomp),
  [doldecomp/melee](https://github.com/doldecomp/melee),
  FFmpeg, fdlibm, and the earlier `custom-robo-3ds` project. Specific borrowed
  techniques, tools, formats, and references are documented in the
  [full credits](./README.detailed.md#credits).

## Further reading

- [Detailed README: setup, controls, runtime, test flags, and full credits](./README.detailed.md)
- [Performance history and hardware run index](./docs/performance.md)
- [Recompiler assumptions and known unknowns](./docs/recomp_unknowns.md)
- [Game SDK function inventory](./docs/sdk_functions.md)
