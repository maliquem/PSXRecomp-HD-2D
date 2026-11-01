/* audio_turbo.h - the fast-forward's time compressor (WSOLA).
 *
 * Under a manual fast-forward the SPU makes N seconds of sound per second
 * of the wall clock (N any speed, varying with the scene). The host can play
 * one: the bridge's ring overflow used to drop the excess in blocks, a click
 * at every seam (~30 a second at 4x). This keeps the pitch and makes the
 * seams smooth: of the input it plays one grain per synthesis hop, each grain
 * placed where its waveform best continues the last one, overlapped with a
 * Hann window. The ratio is measured (frames in / frames out), never assumed,
 * so any speed works, and a speed that changes mid-run too.
 *
 * Every SPU frame goes through at_push, turbo or not; at_pull hands back the
 * input as it came (compress 0) or compressed to the frames asked for
 * (compress 1). The switch both ways is seamless: the grain overlaps add up
 * to the input itself at the edges. Host output only: the SPU, its state and
 * the guest never see any of it. */
#ifndef AUDIO_TURBO_H
#define AUDIO_TURBO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forget all input and state (savestate load, device change). */
void at_reset(void);
/* Stereo frames the SPU made, in order. */
void at_push(const int16_t *in, int frames);
/* Up to `want` frames into out (compress 1), or every frame held (compress
 * 0, `want` ignored past out_cap). Returns the frames written. */
int  at_pull(int16_t *out, int out_cap, int want, int compress);
/* Frames held, not yet played (latency the compressor adds). */
int  at_backlog(void);
/* Observability (TCP audio_turbo): grains made, ratio measured, frames
 * skipped by an overflow jump, the mode (0 direct, 1 compressing). */
void at_stats(uint64_t *grains, double *ratio, uint64_t *jumps, int *mode);

#ifdef __cplusplus
}
#endif

#endif
