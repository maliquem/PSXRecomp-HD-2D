/* audio_music.h - the music at its own tempo under the fast-forward.
 *
 * psx_mod_set_turbo_audio(2). The guest SPU keeps running as always; while the
 * fast-forward lasts, two host-only clones of it (spu_clone.inc) make what is
 * heard: the effects SPU, fed every guest access in step (spu_io_tap) but not
 * the music's notes, through the time compressor (audio_turbo.h); and the
 * music SPU, fed the same accesses late, at the wall clock's pace (any speed:
 * it follows the clock, not the multiplier), but not the effects' notes. A
 * key-on is the music's when the title's classifier says so of its voice
 * (psx_mod_set_music_voice_classifier). On release the music goes on from the
 * music SPU, late but whole (the guest's is further on: going back to it
 * would jump ahead in the song at every release); pressed again, the same.
 * A song change the title reports (psx_mod_music_changed), or the music too
 * far behind: during the fast-forward the music SPU takes the guest's
 * present with a fade out and in; after it, a crossfade to the guest's own
 * sound, and the clones stop. Host output only. */
#ifndef AUDIO_MUSIC_H
#define AUDIO_MUSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The title's voices of the music right now (bit v = voice v), read at each
 * key-on; NULL = mode 2 unavailable (mode 1 is used). */
void am_set_classifier(uint32_t (*music_voices)(void));
int  am_ready(void);
/* The title's song changed: the music SPU catches up (fade out, in). */
void am_music_changed(void);
/* Running (the fast-forward, trailing after it, or the crossfade out). */
int  am_active(void);
/* The pump's stream under mode 2: `live` the guest SPU's frames just made;
 * out gets what the host plays. `want`: the wall clock's frames (used while
 * compressing); ff: the fast-forward is on; on: mode 2 is selected. Returns
 * the frames in out. */
int  am_pump(const int16_t *live, int n_live, int16_t *out, int out_cap, int want, int ff, int on);
/* Stop at once (savestate load). */
void am_reset(void);
/* TCP audio_turbo: state (0 idle, 1 running, 2 crossfading out, 3 trailing), the music's
 * delay behind the guest (ms of game time), events queued, catch-ups, entries. */
void am_stats(int *state, double *lag_ms, uint32_t *queued, uint32_t *resyncs, uint32_t *entries);

#ifdef __cplusplus
}
#endif

#endif
