# Lean core (`[recompiler] lean`)

Status: on `perf/lean`, not merged. Exact (golden, 8 scenes). Measured 2026-11-03 with PGO + ThinLTO on
every side: the threshold thesis below did not hold. See "Measured" at the end.

## Why

After the cycle-5 work, every exact change to the generated code's hot path measured 0-5% on
its own (BoF4, `docs/PERF_RELATORIO.md` of the game). The study of 2026-11-02 (game repo,
`docs/PLANO_NUCLEO_ENXUTO.md`) reads that as a threshold effect: a generated block still calls
out of line every one or two guest instructions (the stores' quiet entry in memory.c, the PGXP
hooks, the slow paths), and with one call in the block the C compiler cannot keep guest state in
host registers (Win64 keeps 7 across a call). `gpr_locals` (docs/GPR_LOCALS.md) moved the guest
registers into C locals and measured 0: the calls were still there, so the locals were spilled.

`lean` is the next layer on top of `gpr_locals`: it takes the remaining calls of the common path
out, all at once, keeping every observable state the same.

## What it changes

The `gpr_locals` pass (recompiler/src/gpr_locals.cpp) writes the `PSX_LN_*` forms
(runtime/include/psx_gpr_locals.h) instead of `PSX_B2_*`:

- **Cycles in a function local.** `PSX_LN_CHARGE(n)` and the loads' access cycles add to the
  function's `_psx_lc` instead of the global batch (`g_psx_cyc_batch`). `PSX_LN_PUB()` hands
  `_psx_lc` to the batch before anything that can see the clock: every slow path, an I-cache
  miss, an edge, a slice, every call (`func_*`, GTE, mult/div stalls, hooks), every return,
  falling off the end, and the block flush (`psx_cyc_bb_defer_flush`). The charge gate
  (`g_psx_cyc_gate`) only changes inside such calls, so a charge that finds it lowered is one
  the B2 form would have batched too; with it raised the charge goes down the slow path as
  before. The batch therefore holds, at every point anything reads it, what the per-charge
  adds put there. The one difference: a 32-bit overflow of the batch inside one run would
  take the slow path for the run's sum instead of one charge (the batch is flushed at every
  edge; it never comes near).
- **Plain stores inline.** `PSX_LN_STORE*` writes main RAM on a page the plain-store table
  marks (`g_psx_store_page`, perf plan G1a: memory.c fills an entry when its own short path
  finds the page plain; whoever starts watching a page clears it) and the scratchpad, with
  the same conditions as `psx_store_fast_ptr` (the CPU's own store pointer, no DLL cycles
  pending, the stores' gate lowered, no cache isolation, aligned) and the same side effects
  (`g_guest_store_count`, the last-store tracker). Anything else takes the B2 form, which
  decides as before (and fills the table).
- Loads: the B2 inline paths, with the charge into `_psx_lc` and the three mode words
  (`g_ls_mode`, `g_ds_recording`, `g_psx_cyc_gate`) tested together.
- **One cold call per site.** The uncommon path of a load, store or edge is one out-of-line
  call (`psx_ln_*_cold` in psx_cycles.c, each doing exactly the B2 form's inline text) instead
  of the B2 forms' inline copies (a second load path, three store calls).
- **RAM and scratchpad by name.** memory.c's arrays are exported as `g_psx_ram_arr` /
  `g_psx_scratch_arr` (`g_psx_ram` / `g_psx_scratch` still point at them). A store through the
  pointer could, for the compiler, change any global, so every mode word and the gate were read
  again after each guest store; a store into a named array cannot.

Static count (shard 03 of BoF4, no PGO, the hottest 3D function `func_80145D90`): memory reads
-10%, memory writes -10% against the B2 forms; call sites +10% (each site now has its one cold
call; the B2 store's common path was a call). A first version without the last two items had
+30% instructions and 2.5x the stack operands: the inlined slow paths were bigger than what
the lean path saved.

`-DPSX_LEAN_OFF` builds the same generated text with the B2 forms (A/B from one `generate`).
The option needs `gpr_locals = true` and `cpu_timing = "block"`; it applies to the executable's
functions, not to overlay DLLs.

## Complements (build and run options)

- `PSX_TARGET_ARCH` (CMake cache): `-march` for the runtime and the generated code, e.g.
  `x86-64-v3` (BMI2 shifts, LZCNT for the GTE's divide, AVX2). Integer results are the same;
  float results too (`-ffp-contract=off` stays, no reassociation). An exe built with it does
  not start on a CPU without those instructions.
- `PSX_GAME_THREAD_CPU=N` (environment): the game thread held on logical CPU N.
- Function order by profile: lld's COFF driver sorts sections by the call-graph profile by
  default when PGO left one in the objects; nothing to turn on.

## PGXP: the scratchpad on the inline path

`pgxp_fast.h` (not part of the execution identity) already took, for main RAM, the light memory
mode's two common cases inline: a word load from a slot with no live bit (reset the register's
shadow) and a store of an untracked register into one (nothing). Every word access to the
scratchpad still called the hook, and BoF4's hottest 3D function works on a scratchpad base.
`pgxp_fast_unmarked` now reads the scratchpad's live bits as well (index
`PGXP_FAST_SCRATCH_BIT0` + word, the same as pgxp.cpp's `ck_index`). pgxp.cpp is untouched.

## Proof

- BoF4, `tools/mode_check.py` of the game: 8 scenes x 630 frames, frame prints (cycles and hashes
  of every RAM / MMIO / scratchpad write) and sound identical to the B2 exe; the x86-64-v3 build
  identical to the lean one. Photos differ from run to run even for one exe (busy machine).
- Golden (`tools/golden.py` of the game: per-frame hashes of RAM, scratchpad, CPU and GTE
  registers, cycles, GPU words, SPU samples and the whole PGXP state): the lean forms and the
  scratchpad PGXP path identical to the B2 forms (`-DPSX_LEAN_OFF`) in 8 scenes x 600 frames.
- To do: cold boot; the user's saves after a restamp (the generator's hash moved).

## Build variants for the A/B (same generated code)

| define | what it builds |
|---|---|
| (none) | lean: B2 locals + lean forms |
| `-DPSX_LEAN_OFF` | the B2 forms (`gpr_locals` as measured on 2026-11-02) |
| `-DPSX_LEAN_NO_LOCALS` or `[recompiler] lean_no_locals = true` | the B2 and lean forms with the registers left in `cpu->gpr` (no locals, no sync code) |
| `-DPSX_GPR_LOCALS_OFF` | the original text: no forms at all |

Static count of the generated functions in the linked exes, all with PGO + ThinLTO (BoF4, every
`func_*`, cold code included):

| variant | x86 instructions | stack operands | calls |
|---|---|---|---|
| original | 2.23 M | 38 k | 200 k |
| B2 forms | 3.22 M | 183 k | 226 k |
| lean | 3.55 M | 233 k | 250 k |
| lean, no locals | 2.98 M | 28 k | 243 k |

The locals bring a lot of sync code on the cold paths (every slow path stores the dirty set): 5x
the stack operands of the original. Whether the hot paths gain more than the size costs is what
the measurement has to say; the no-locals variant is there in case they do not.

## Measured (BoF4, 2026-11-03, idle PC, PGO + ThinLTO on each exe, turbo without a cap)

Median of 3 rounds x 3 windows of 900 VBlanks, the frame counter read every 100 ms (reading it
every 5 ms over TCP takes 35-40% off the turbo: each query is served by the game thread).

| scene | original | lean | lean, no locals | no locals + x86-64-v3 |
|---|---|---|---|---|
| dungeon | 7.58x | +0.9% | +1.6..1.8% | 7.87x (+3.8%) |
| night | 7.12-7.23x | +1.5% | +1.6..4.4% | 7.52x (+5.6%) |
| magic | 11.5x | -1.1% | -0.3..+0.3% | +0.7% |
| battle | 16.0x | -0.5% | +0.1..0.6% | +0.7% |
| field | 16.7x | -1.2% | -0.9..+0.6% | -0.2% |

The B2 forms alone: -1.5..-3.2% (magic +4%, noisy). Thread affinity: +0.5%, noise. So the locals
hurt, the lean pieces give 0-2%, and the profile stays flat (generated code 23% of the time inside
the exe, PGXP 16, GTE 12, emulated GPU 10, cycles 7): there was no threshold to cross. Without PGO
the lean forms looked like +2..7%: judge generator changes only with PGO on both sides.
