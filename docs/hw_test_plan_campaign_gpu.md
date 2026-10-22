# Hardware test plan: campaign GPU (2026-10-22)

Build: branch `claude/campaign-gpu-test` (main 9719c55 + cull, texture, vertex shader and
bottom-menu branches). Copy `custom-robo-recomp.3dsx` to the SD card as usual. Do not put any of
the new flag files on the SD: every A/B is switched with the bottom-screen buttons.

## Buttons (bottom screen, each shows ON / OFF)

| button | ON means | expected effect |
|---|---|---|
| cull | GXSetCullMode honoured (new) | back faces not drawn: less fill and triangle setup |
| mipmaps | mip chains uploaded (new) | none expected: Citra found the game uses no mipmaps |
| tex formats | lossless 16-bit texture formats (new) | small: ~2% fewer texture bytes |
| vsh single | ONE shader for every draw (OLD behaviour) | OFF = 14 shader variants (new), shorter vertex programs |
| GX thread / audio realtime / gpukick | as before | leave ON / as usual |

Every switch is logged and applies at the next frame. `tools/time_avg.py` drops any 60-retrace
window that contains a switch, so wait about 2 s after each tap.

## Steps (one session, campaign scene 8.3, play normally)

Stay in each state for about 30 s of campaign play (at least 10 `# time` windows):

1. All new features on (default: cull ON, mipmaps ON, tex formats ON, vsh single OFF).
2. cull OFF. Afterwards cull ON again.
3. vsh single ON. Afterwards OFF again.
4. tex formats OFF. Afterwards ON again.
5. Everything old: cull OFF, tex formats OFF, vsh single ON (about the run 25 behaviour).
6. Back to step 1 for another 30 s (checks for drift from play content).
7. Optional: 30 s of a Vs. battle in step 1 and step 5 states.

Look at: no missing geometry with cull ON (holes in walls/robots = wrong winding; note where), and
no texture colour changes with tex formats ON.

Known open item (Citra): in the battle framebuffer dumps (r2100/2700/3300), the combined build differs
from both the vsh-only and the cull-only builds. The hexagon markers over the robots (e.g. the "TM" one)
show a textured face where those builds showed a solid colour, and some edge lines differ. The cause is not
found yet (an interaction of cull with the texture changes is the guess, unverified), and it has not been
checked against Dolphin. In battle, toggle cull and tex formats and say which state matches Dolphin/your
memory of the game. Campaign dumps are byte-identical to the vsh build.

## Afterwards

Copy sd:/cr_autotest.log to logs/cr_autotest_2026-10-2x_hw-run26-campaign-gpu.log in the main
checkout, then `PYTHONUTF8=1 python tools/time_avg.py <log>`: it prints one row per scene and flag state
(flags column: cull, vsh, mip, t16; windows containing a switch are dropped). For each state, compare the
campaign 8.3 values: gpu ms, gx wait ms, fps. Run 25 reference: 44.4 fps, gpu 18.9, gx wait 16.5.
