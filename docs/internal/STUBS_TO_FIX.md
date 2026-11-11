# Stubs to Fix

Audit date: 2026-04-24. Rechecked 2026-11-11 (study B6): every item below is
implemented. No open stubs are listed. Add a new item here when one is found,
with the file, what real hardware does instead, and the impact.

The CLAUDE.md Phase 5 gate (S3, S4, S5) is met. BoF4, FM2 and WA2 play their
videos with sound through these paths, and their golden runs cover them.

## Resolved

- ~~S1: GPU shaded line uses flat color~~. `gp0_exec_shaded_line()` in
  `runtime/src/gpu.c` reads C0 and C1 and draws through `gr_draw_shaded_line`.
- ~~S2: GPU polyline crashes~~. GP0 0x48-0x4F and 0x58-0x5F run a polyline
  state machine (`GP0_POLYLINE_MONO` / `GP0_POLYLINE_SHADED` in `gpu.c`) until
  the terminator word.
- ~~S3: MDEC decoder is a no-op~~. `runtime/src/mdec.c` decodes macroblocks,
  fed and drained by DMA ch0/ch1 in `runtime/src/dma.c`.
- ~~S4: SPU has no audio synthesis~~. `runtime/src/spu.c` models the 24 ADPCM
  voices, envelopes, noise, sweep, IRQ and the reverb engine (clean room), and
  renders audio through `spu_render`. The points the documentation leaves open
  are marked inline as `DOCUMENTED-GAP`.
- ~~S5: DMA channels 0, 1, 3, 4 unimplemented~~. `dma.c` handles MDEC (ch0,
  ch1), CDROM (ch3) and SPU (ch4) transfers. Open and tracked in
  `ACCURACY_BURNDOWN.md`: the DMA bus cycle-steal is not modeled.
- ~~DMA ch2 GPU->RAM writes zeros~~. Fixed 2026-04-24 (4054dc1).
- ~~GPU raw texture mode uses color==0x808080 check~~. Fixed 2026-04-24.
