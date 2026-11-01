/* spu_clone_sfx.c - the fast-forward's effects SPU (audio_music.c): spu.c once
 * more (spu_clone.inc), on the guest's own clock, fed every guest access in
 * step but not the music's notes. */
#define SPUC(x) spus_##x
#include "spu_clone.inc"

#undef psx_get_cycle_count
uint64_t psx_get_cycle_count(void);
uint64_t spus_clock(void) { return psx_get_cycle_count(); }
