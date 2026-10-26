# Lazy vertex descriptor (experiment 2 of the 2v2 sequence), 2026-10-26

Branch claude/pensive-cannon-e4b2ed @5041ccb (on main ad99a97; not merged). Write-up: docs/recomp_unknowns.md
"Lazy vertex descriptor"; also docs/performance.md, CLAUDE.md Status, README flags table + buttons.

- Change (runtime/gx.c only, plus a menu.c button and a ppc.h extern): GXSetVtxDesc/GXClearVtxDesc only change gs.desc
  and set desc_dirty. desc_sync() runs before the two draw records (x_fifo, x_call_dl), which are the only gx.desc
  readers on the GX thread, and in vtx_size_g. It queues one x_set_desc record (26 bytes) only when gs.desc differs
  from desc_sent. Eager path = old code plus desc_sent bookkeeping, so the button can switch at any call.
- Flag sdmc:/cr_lazydesc_off.txt, button "Lazy vtx desc" (5th button). New log line every 300 game frames:
  `# gx queue frame <n>: per frame <records>, <words>; vertex descriptor lazy|eager: <calls>, <records>`.
- Result (Citra): 2v2 4.2 GX-queue records 7586 -> 6428 per frame (descriptor ~1200 -> ~80). Game: base 35.7,
  lazy 35.6, flag off 36.1 ms (2 runs each, identical). GX busy 38.3 -> 37.9, frame 45.7 -> 45.4. Battle 4.2 and
  campaign 8.3 are unchanged within 0.1 ms.
- Validation: 2v2/battle/campaign/boot, flag on and off, give compare_logs.py exit 0 vs base, with fb dumps identical.
- Default ON. It is small (below the 2% bar) but exact, and every 2v2 metric is equal or better. The flag-off path
  being 0.4 ms slower than base is not explained (guess: Citra code layout). The user decides whether to keep it.
  Not yet on hardware.
- Worktree state: data/ is a junction to the main checkout's data/; gen/ + .3dsx are this branch's build; logs in
  logs/perf/ (base-*, on-*, off-*, on2-2v2, off2-2v2; not committed).
- Next: #3 sin/cos memo study.
