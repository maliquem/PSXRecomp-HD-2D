/* spu_clone_music.c - the fast-forward's music SPU (audio_music.c): spu.c once
 * more (spu_clone.inc), on a clock of its own that the replay moves at the
 * wall clock's pace, fed the guest's accesses late but not the effects' notes. */
#define SPUC(x) spum_##x
#include "spu_clone.inc"

static uint64_t s_mclock;
uint64_t spum_clock(void) { return s_mclock; }
void spum_set_clock(uint64_t cycle) { s_mclock = cycle; }
