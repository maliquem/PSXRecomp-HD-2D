/* After a true depletion the bridge rebuilds its target instead of resuming
 * with one tiny block, and one episode counts as one underrun. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    rab_bridge b;
    rab_config c;
    rab_config_defaults(&c);
    c.source_rate = c.host_rate = 44100;
    c.target_ms = 80;
    assert(!rab_init(&b, &c));
    int16_t pcm[8820], out[19200];
    for (int i = 0; i < 8820; i++) pcm[i] = 12000;
    rab_push(&b, pcm, 4410);
    rab_pull(&b, out, 441);
    assert(b.primed);
    rab_pull(&b, out, 9600);
    assert(!b.primed);
    uint64_t under = b.stats.underrun_events;
    assert(under == 1);
    rab_pull(&b, out, 441);
    assert(b.stats.underrun_events == under);
    rab_push(&b, pcm, 882);
    rab_pull(&b, out, 441);
    assert(!b.primed);
    rab_push(&b, pcm, 3528);
    rab_pull(&b, out, 441);
    assert(b.primed);
    rab_free(&b);
    puts("PASS: audio depletion re-primes once");
    return 0;
}
