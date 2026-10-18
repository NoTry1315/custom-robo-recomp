# custom-robo-recomp

Static recompilation of Custom Robo (GameCube, GXCE01) to New 3DS.

## Rules

- **Never hand-port game logic.** All game code comes from main.dol through the
  translator. Only the SDK/hardware layer (GX, DVD, PAD, OS, AI/DSP, CARD,
  VI) is written by hand, in `runtime/`. If a fix means rewriting game
  behaviour, stop and ask: it means the runtime is wrong.
- **Oracle: Dolphin** running the same disc. A difference is a runtime bug.
- **Progress number:** boot stage reached (main loop -> logo -> title -> menu
  -> battle) and SDK functions replaced / total (`docs/sdk_functions.md`).

## Layout

- `tools/recomp/` PPC -> C translator (to write). Output to `gen/` (ignored).
- `runtime/` 3DS replacements for SDK functions, one file per SDK module.
- `tools/` carried over: `dol.py` (xref/dis/func), `ghidra/decompile.ps1`,
  `extract_disc.py`, and asset decoders (`sound.py`, `tpl.py`, `font.py`,
  `bmg.py`, `gxtex.py`) for the runtime to reuse. `decomp_names.tsv` = known
  function names.
- Reference only, do not copy code from: `../custom-robo-3ds` (old
  reimplementation; its CLAUDE.md has format and address notes).

## Phases (one per session)

1. SDK inventory -> `docs/sdk_functions.md` (address, name, module).
2. Translator: all of main.dol compiles for ARM.
3. Stub runtime: every SDK call logs; boots to main loop on Citra.
4. DVD, PAD, OS -> reaches title screen in the log.
5. GX -> PICA200 graphics.
6. Audio.
7. Fix pass vs Dolphin.

## Build / test

```
python tools/extract_disc.py "<disc>.iso" data/extracted   # once
docker run --rm -v "C:/Users/Alexander/source/repos/custom-robo-recomp:/proj" -w /proj mkw3ds-build:2026-10-09 make
```
Citra: `tools/citra/autotest.ps1 -Fast -Shots 0`, never take desktop focus.
Windows: run Python with `PYTHONUTF8=1`.

## Token rules

- Mechanical sweeps (SDK signatures, function boundaries) -> Sonnet/Haiku
  subagents; only conclusions come back.
- Logs, not screenshots. Grep decompiles, never dump whole functions.
- One phase per session; this file carries the state.
- Never commit disc data or game code; `gen/` is generated from the user's disc.
