# Guest registers in C locals (`[recompiler] gpr_locals`, perf plan B2)

Status: exact, no measured gain without PGO (branch `perf/b2`, 2026-11-02). Off unless
the title turns it on.

## Why

Every generated instruction reads and writes `cpu->gpr[n]` in memory. The compiler
cannot keep a guest register in a host register across any out-of-line call, since
the callee could look at `cpu`, and almost every guest store is such a call
(`psx_store_*` is noinline). So each write is a store and each read after a call is
a reload. The DuckStation recompiler allocates host registers to guest registers;
this mode gets the same effect from the C compiler: the registers a function uses
become C locals whose address is never taken, so clang keeps them in registers.

## The rule that keeps it exact

`cpu->gpr` must hold the guest's registers whenever anything other than the
function's own inline code can look at them. So:

- **Entry.** Every register the function references is loaded into its local at the
  top, before the CPS entry switch (any entry, top or continuation, starts from
  `cpu->gpr`).
- **OUT** (store the locals that may differ from `cpu->gpr`) before every exit
  (`return`) and before every out-of-line call that can reach code which reads
  guest registers, runs device service (VBlank callbacks, plugins, guest calls
  from plugins), polls the debug server or savestates, or longjmps.
- **IN** (reload every referenced local) after every call that can write guest
  registers: the interrupt check's slow path (handler, idle skip's progress
  register), the precise slice, mod hooks and function-entry hooks, syscall /
  break / unknown dispatch, direct function calls, `gte_execute` (writes $s2).

The inline helpers with a cold out-of-line path (I-cache tag check, cycle charge,
block-edge interrupt check, RAM/scratchpad loads, stores) get macro forms in
`runtime/include/psx_gpr_locals.h` that run OUT (and IN where needed) only inside
their cold branch, deciding the fast case exactly as the inline helper does. The
fast cases are pure inline code with no calls.

Callees proven never to touch `cpu->gpr`, never to run device service and never
to longjmp take no sync: the PGXP engine (`pgxp.cpp` has no `cpu->` access), the
GTE data/control register accessors, `psx_mult_latency_*`, the widescreen cull
compares, the store fast paths (`psx_store_*_try`).

Every other call keeps the plain OUT before / IN after, which never costs more
memory traffic than today (today every write is a store and every read after a
call is a load).

## Dirty sets

OUT stores only the registers that may have been written since `cpu->gpr` was last
in step. The generator splits the body at its block labels, collects the writes and
the gotos of each block, and runs a forward union to a fixpoint (a block can be
reached by fallthrough from the block above it as well as by gotos). A block whose
incoming set is larger than `gpr_locals_flush_max` registers flushes it on entry
(fast path), which bounds the size of every cold-path OUT.

## Off switch

The generated text only uses macros (`PSX_R(n)`, `PSX_B2_*`). Built without
`PSX_GPR_LOCALS` they expand to today's code exactly, so the same generated sources
build both ways (A/B). Overlay DLLs (captured areas) are generated without the pass.

## What does not see the locals

Other threads reading `cpu->gpr` mid-function (freeze heartbeat dumps) may see
values up to one sync point old. Diagnostics only.

## Status (2026-11-02)

BoF4: 1113 of 1119 functions localized (the 6 left name no register).
`gpr_locals_flush_max = 4`: 1382 block entry stores. Static machine code of two
shards (03, 05; no PGO, no LTO; cold paths included), off -> on:

| | off | on |
|---|---|---|
| instructions | 274k | 447k |
| memory reads | 78k | 124k |
| memory writes | 8.7k | 54k |
| stack memory operands | 4.9k | 29k |

In the hot path the locals do stay in host registers (a small function keeps
$sp and $ra in rbx/rbp and drops their cpu->gpr loads and stores), but the
code grows: every cold path carries its stores, and with the Windows x64 ABI
(7 callee-saved registers) and calls everywhere the compiler spills more. The
net effect is not known until it is measured. Liveness-limited IN lists saved
~1.5% of reads; flush_max 2 vs 4 trades entry stores for smaller cold paths
(418k vs 447k instructions).

Measured on BoF4 (2026-11-02, no PGO on either side, area cache complete):

- **Exact.** `mode_check`, 8 scenes, effects on: frame by frame identical for 630
  frames each (cycles, RAM/MMIO/scratchpad writes), sound identical, shots 0 px
  (the campfire flickers by wall clock, also between runs of one exe).
- **No speed gain.** Turbo, 3 alternating rounds, medians off -> on: title +1.8%,
  field -2.5%, dungeon +0.3%, night +4.1%, campfire -0.6%, battle -0.8%, magic
  -1.3%, menu -3.3%: inside the run-to-run noise. Game thread CPU at 1x equal.

The hot-path savings are paid back by the larger code (cold paths carry their
stores; more spills). Area DLLs are generated without the pass. Not tried yet:
PGO on both sides (it moves cold blocks out of the hot code, which is the cost
here).

Test folder: bof4-b2 `build-test/b2` (both exes from the same generated code:
`BreathOfFire4_b2on.exe`, `BreathOfFire4_b2off.exe` built with
`-DPSX_GPR_LOCALS_OFF`). Codegen hash 0xf844c460: the area cache starts cold
and the test states need restamping from a state the new exe saves.

## Proof required before it is on by default

Frame-by-frame identical over 630 frames in the 8 scenes with effects on, identical
sound, cold boot 0 px, every save loads (hash change: restamp), plugins exercised
(shadows' anchors, 16:9 funnels, mouse aim, difficulty hooks).
