# Instruction cache: clean-room rewrite

`runtime/src/psx_icache_core.inc` replaces a model that `psx_icache.c` itself described as transcribed from another
emulator. The project takes no Beetle/Mednafen or DuckStation code. Same method as `SPU_ENV_CLEAN_ROOM.md`: the writer
had only psx-spx, the interface in `psx_icache_core.h` and a compiled black box of the old behaviour, which it could
run but not inspect. The writer is an AI model and cannot rule out having seen public emulator code in training, so
"nobody involved ever saw it" cannot be claimed as it could for a human team.

**Result.** 0 differences in 400 million random operations (fetches and stores, with the whole tag array compared
regularly) and in 457 million structured cases: 39 regions, every low BCC bit pattern, every line offset and 5 starting
line states. Five deliberate bugs were all caught.

**Rules psx-spx does not state** (marked "observed" in the code):

- An invalid word holds its own address with bit 1 set.
- A cached miss costs 3 cycles plus 1 per word filled; an uncached miss costs 4 and leaves the line alone.
- The hit test comes first, even for an uncached fetch.
- A miss rewrites all four tags: words before the entry word end up invalid, even if they were valid.
- TAG beats LOCK and INV. LOCK or INV alone invalidates the line, taking the store's address as its tag.
- A TAG store to a non-aligned address acts as value 0.

**Differs from psx-spx, kept so timing stays exact.** These are fidelity candidates, to check on real hardware. Changing
any of them moves every game's timing.

- psx-spx says fills with matching tags OR into the valid bits. Here the words before the entry word turn invalid.
- psx-spx says a tag match on an invalid word refills the whole line. Here it is a normal partial fill.
- psx-spx says tags are physical. Here the virtual address is the tag, so KUSEG and KSEG0 of one location do not match.
  Compiled code's inline hit test depends on this.
- psx-spx says KSEG1 bypasses the cache. A TAG store to a KSEG1 address still creates a valid tag here.
- psx-spx's 2-word refill size (IBLKSZ) is not modelled.
