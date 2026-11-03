# Lean core (`[recompiler] lean`)

Status: in progress on `perf/lean` (2026-11-02). Exactness and speed not measured yet.

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

## Proof (to do)

Frame prints, sound and photos identical to the B2 exe in the 8 scenes (`tools/mode_check.py`
of the game), cold boot, the user's saves after a restamp (the generator's hash moved).
