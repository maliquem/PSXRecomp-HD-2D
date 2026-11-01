/*
 * present_phase.h — presents held to the screen's phase.
 *
 * On a screen at a whole multiple of the guest rate (a 119.88 Hz panel under a
 * 59.94 Hz game) vsync does not own the cadence: the wall-clock pacer does, and
 * a frame used to be swapped the moment the renderer finished it. That moment
 * wanders by more than one refresh (the render thread takes 3-16 ms), so a
 * 30 fps picture showed 3, 4 or 5 refreshes instead of 4, ~1.4 times a second.
 *
 * Here every real present is swapped at a fixed point of the screen's refresh
 * grid, read from the display's own vblanks (a thread waiting on them, Windows:
 * D3DKMTWaitForVerticalBlankEvent):
 *   - ref: the pacer deadline the guest frame was released at (main.cpp);
 *   - R: how long after ref frames are ready (90th percentile, last 2 s);
 *   - the swap waits for the first refresh middle at or after ref + R, with the
 *     swap interval at 1, so it shows at the next vblank whatever the
 *     compositor's latch point: every frame lands on the same refresh of its
 *     slot, and a frame up to half a refresh later than R still does;
 *   - the pacer is nudged (at most 0.3% of a period a frame) so that ref + R
 *     sits half a refresh before that middle: the guest clock phase-locked to
 *     the screen, R free to wander by half a refresh without moving a frame.
 * Off by PSX_PRESENT_PHASE=0; inactive (nothing changes) without vblank times,
 * on screens that are not a multiple >= 2 of the guest rate, in the turbo and
 * under frame generation (which has its own pacing).
 */
#ifndef PRESENT_PHASE_H
#define PRESENT_PHASE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Composing thread, right before a real (not generated) swap. `ref` is the
 * pacer deadline in SDL performance-counter ticks (0: unknown), `native_window`
 * the HWND on Windows. Returns 1 when the swap should run with interval 1
 * (the hold ran or is locked on), 0 to swap as before. May sleep. */
int present_phase_hold(uint64_t ref, void *native_window);

/* Emulation thread, before each paced wait: ticks to add to the pacer's next
 * deadline (phase lock), 0 while inactive. period_ticks: the guest period. */
int64_t present_phase_correction(uint64_t period_ticks);

/* Measurement (PSX_PRESENT_PHASE_PROBE=1, the renderer waits for the GPU after
 * each real swap): the swap's and the GPU's end, in performance-counter ticks.
 * The refresh each frame lands on is the first vblank after both; the gaps
 * between them, in refreshes, go to present_phase_json ("gaps"). Works with
 * the hold off too (the vblank thread starts here). */
void present_phase_note_done(uint64_t swap, uint64_t done, void *native_window);

/* 1 while the hold is engaged (the last present went through it). */
int present_phase_active(void);

/* JSON object body for the debug server (no braces). */
int present_phase_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PRESENT_PHASE_H */
