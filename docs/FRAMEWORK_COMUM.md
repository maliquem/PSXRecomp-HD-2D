# The common framework copy

Every game recomp keeps its own working copy of this framework, and every improvement or fix ends up in
one shared place, so all games stay at the same level of features and speed.

- **Common copy:** bare repository `K:/src/psxrecomp-comum.git`, branch `main`. Local only; nothing here
  goes to GitHub by itself. A mirror on the external drive (`G:/BreathOfFire4Recomp_dados/psxrecomp-comum.git`)
  follows it on every push (hook `post-receive`).
- **In each game:** the `psxrecomp` submodule has a remote called `comum`, and the game keeps its own branch.
- `main` only moves forward: no force pushes, no deletes (the repository refuses both).

## The rule

1. Work on your own branch, as always. Commit small.
2. When a change works in your game, bring it to the common copy the same day:

   ```sh
   git fetch comum
   git merge comum/main          # your branch now has everybody's work
   # rebuild your game, run your golden check (below)
   git push comum HEAD:main      # fast-forward only; if refused, fetch and merge again
   ```

3. When you start a session, or before any framework change: `git fetch comum` and merge `comum/main`.
4. Generator changes: if `psxrecomp-game --codegen-hash` moved, every game must regenerate, restamp its
   savestates (word at offset 16) and move its area cache to the new tag. Say so in the commit message.
5. Execution identity (`gte.cpp`, `pgxp.cpp`, `hle_gte.h`, `gte.h`, `cpu_state.h`, `pgxp.h`,
   `render_pass_projection.h`): one byte changes the cookie (offset 32) and old savestates are refused. Say so too.

## What belongs in the framework

- Anything a second game can use. Behaviour for one game stays in that game's plugin, or behind an option
  that is **off by default**. Never test for a game's id or addresses in the framework.
- With an option off, the emulation must stay exact: same golden hashes, same pictures.
- A real fidelity fix may change bytes for every game (ChangeTh returning 1 changed two TCB bytes in BoF4).
  Then explain which bytes and why in the commit, and show that cycles, CPU, GPU and sound did not move.
- Nothing from Beetle/Mednafen or DuckStation. Documentation first: psx-spx.

## Golden check

Each game has a `tools/golden.py`: per-frame hashes of RAM, scratchpad, CPU, GTE, cycles, GPU words, SPU samples
and the PGXP state, over fixed savestates. Run A/B of the same game on the old and new framework, with the same
area cache on both sides, ready, and with automatic compilation frozen (a half-filled cache changes the result).
Pictures are separate: golden does not see shaders; `tools/mode_check.py` compares photos and sound.

Run both sides back to back, from the same folder. Comparing against a reference saved earlier misleads: the
plugins' config files count (a perf tool rewrote BoF4's `bof4_cheats.txt` and the same exe stopped matching its
own reference from frame 0).

Reading the frame counter every few milliseconds over TCP takes 35-40% off the turbo. Read it every 100 ms
or slower in speed windows, and judge generator changes only with PGO on both sides.

## Starting a new game on it

```sh
git -c protocol.file.allow=always submodule add K:/src/psxrecomp-comum.git psxrecomp
git -C psxrecomp remote rename origin comum
bash psxrecomp/tools/ci/build_emitters.sh       # then check: build-recompiler/psxrecomp-game --codegen-hash
```

`game.toml`, proven on the existing games:

```toml
[recompiler]
cpu_timing = "block"          # block timing; needs the regen below
ram_fast_path = true
gpr_locals = true             # the pass behind lean
lean = true                   # cycles in a local, plain stores inline, one cold call per site
lean_no_locals = true         # registers stay in cpu->gpr: the locals alone cost speed

[runtime]
overlay_precompile = true

[video]
renderer = "sdl_gpu"
pgxp_alu = "moves"            # the light PGXP modes; "full" / "all" are the exact shadow
pgxp_mem = "live"
```

Any change under `[recompiler]` needs `generate`, then rebuilding, then the area cache. `idle_skip` needs an audit
per game (BoF4 yes, FM2 no).

CMake for the daily build: `-DPSX_DEBUG_TOOLS=ON -DPSX_PGXP_VARIANT=ON`. For the fast one add
`-DPSX_PGO=use -DPSX_RUNTIME_IPO=ON -DPSX_TARGET_ARCH=x86-64-v3` and a profile trained on your own code in
`<build>/pgo/default.profdata`. `x86-64-v3` builds do not start on CPUs without AVX2.

The overlay toolchain beside the exe does not refresh itself: when the generator's hash changes, copy
`psxrecomp-game.exe` and `runtime/include/overlay_codegen_hash.h` there, or automatic compilation fails on every
try and the game hitches.

## Parity log

Newest rows last. Hash `f6e8251e` and identity `effd3445` unless a row says otherwise.

| Date | What | Proof |
|---|---|---|
| 2026-11-03 | BoF4 and FM2 copies merged (`135cbc3d`, generator hash `f6e8251e`, identity `effd3445`) | BoF4 against its previous exe: cycles, CPU, GTE, GPU, SPU identical in 8 scenes x 600 frames; RAM differs only by ChangeTh's result in two TCBs; photos 0 px in 7 scenes (the campfire varies run to run); sound identical. FM2: golden identical in 8 scenes x 600 frames, PGXP included; 0 hitches over 100 ms in 150 s |
| 2026-11-04 | SPU envelopes rewritten clean-room (`spu_env.inc`, docs/SPU_ENV_CLEAN_ROOM.md) | Against the old code: 0 differences in 134M parameter inputs and 800M random states. BoF4 and FM2 golden identical in 8 scenes x 600 frames, sound included. Hash and identity unchanged |
| 2026-11-04 | Instruction cache rewritten clean-room (`psx_icache_core.inc`, docs/ICACHE_CLEAN_ROOM.md); stale overlay toolchain stops at the first try; two debug recorders follow the passive policy | 0 differences in 857M cases against the old code; BoF4 and FM2 golden identical in 8 scenes x 600 frames; stale toolchain tested both ways. Hash and identity unchanged |
| 2026-11-04 | Triangle edge math rewritten clean-room (`gpu_edge.inc`); 3 small bugs from static analysis fixed; oracle patches moved out of the tree | 0 differences in 146M cases; BoF4 and FM2 golden identical in 8 scenes x 600 frames |
