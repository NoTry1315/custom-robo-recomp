# sin/cos memo study (experiment 3 of the 2v2 sequence), 2026-10-26

Branch claude/frosty-hopper-d3fb2b (on main ad99a97; not merged). Docs only: docs/recomp_unknowns.md
"sin/cos memo study", docs/performance.md "Tried and not kept", CLAUDE.md Status. No runtime change, no flag,
no button (nothing to add to the README flags/buttons tables).

- Measured with a temporary probe in runtime/libm.c nat_sincos (patch: notes folder sincos-memo/libm-probe.patch,
  log sincos-memo/probe3-2v2.log). Citra 2v2 scene 4.2 per frame: ~2350 calls; paths tiny (x ~ 0) 1170, |x|<=pi/4
  680, rem_pio2 530; back-to-back same call ~5; sin->cos pairs on one argument ~1450 (0x8019fc14); repeats in the
  frame ~1520, from the previous frame only ~480; direct-mapped 256-entry hits ~1450 (tiny 980, small 300, rem 260).
- Cost (cr_bench, Citra ticks): sdk_sin tiny 768 ns, small 806, rem 1123-1194; simulated hit (hash + compare +
  register put) 734 ns. A hit only saves the arithmetic: ~30 / 70 / 430 ns by path.
- Estimate: ~0.1-0.15 ms of 36.7 ms 2v2 game (~0.3%); sincos pair ~0.06 ms. Below the 2% bar: not built.
  The probe's 0.5-0.9 ms estimate priced every repeat at the average call cost; most repeats are x = 0.
- Base replays of main (2v2 game 36.7 ms) in logs/perf/base-*.log of this worktree (not committed).
- Not run on hardware.
- Next: #4 selective inlining of the per-particle helpers.
