# SPU envelopes: clean-room rewrite

`runtime/src/spu_env.inc` (ADSR and volume sweep) replaces code that `spu.c` itself described as ported
verbatim from another emulator. The project takes no Beetle/Mednafen or DuckStation code.

**Method.** The writer never saw the old code. It received `spu_env.h` (an interface in psx-spx's terms) and
psx-spx's SPU chapter, and checked its work against a compiled black box of the old behaviour, which it could only
run: a harness printed inputs where the two differed. It did not open the framework, any emulator's source or the
web, and could not inspect the object. Whoever reads this code to change it should keep working from psx-spx.

**Result.** 0 differences in every input of `spu_env_params` (134,217,728) and in 400 million random states for each
of `spu_adsr_tick` and `spu_sweep_tick`.

**Rules psx-spx does not state** (marked "observed" in the code, seen from the black box):

- The exponential decrease's division by 8000h rounds down.
- The exponential increase's threshold is bits 13 and 14 of the level both set: level >= 6000h, where psx-spx says >.
- `all_bits` is a mask: with a shift-only rate (7Ch) the step bits are ignored.
- The counter restarts from 0 on every step.
- Attack ends at the start of the tick after reaching 7FFFh, which already runs with Decay's parameters.
- Decay ends only on a step, and only strictly below the sustain level. Release has no exit.

**Differs from psx-spx's text, kept for exactness.** In sweep mode the level stays in 0..7FFFh of the phase's domain,
where psx-spx describes the volume passing through zero. Worth checking on hardware some day, as a fidelity fix.
