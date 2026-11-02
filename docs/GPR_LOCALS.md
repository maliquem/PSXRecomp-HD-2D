# Guest registers in C locals (`[recompiler] gpr_locals`, perf plan B2)

Status: in progress (branch `perf/b2`, 2026-11-02). Off unless the title turns it on.

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

## Proof required before it is on by default

Frame-by-frame identical over 630 frames in the 8 scenes with effects on, identical
sound, cold boot 0 px, every save loads (hash change: restamp), plugins exercised
(shadows' anchors, 16:9 funnels, mouse aim, difficulty hooks).
