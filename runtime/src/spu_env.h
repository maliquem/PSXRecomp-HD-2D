/* spu_env.h: the SPU's volume envelopes (ADSR and volume sweep). Interface only.
 *
 * Implemented in spu_env.inc, a clean-room rewrite from psx-spx (docs/SPU_ENV_CLEAN_ROOM.md).
 * Everything runs once per 44100 Hz sample, for one voice or one volume register.
 *
 * Terms are psx-spx's, chapter "Sound Processing Unit (SPU)", section "Envelope Operation depending
 * on Shift/Step/Mode/Direction", and the ADSR and volume register descriptions.
 */
#ifndef SPU_ENV_H
#define SPU_ENV_H
#include <stdint.h>

enum { SPU_ENV_ATTACK = 0, SPU_ENV_DECAY = 1, SPU_ENV_SUSTAIN = 2, SPU_ENV_RELEASE = 3 };

/* The step and the counter increment of one tick.
 *   rate        the 7-bit rate field: Shift in bits 2-6, Step in bits 0-1. Rates with only a shift field
 *               (decay: 4 bits, release: 5 bits) arrive already shifted left by 2.
 *   all_bits    the value of `rate` with every bit of its fields set: 0x7F, or 0x7C for those 5-bit
 *               rates. psx-spx's ALL_BITS.
 *   exponential, decreasing   the mode and direction bits of the phase.
 *   invert_step psx-spx's "Decreasing XOR PhaseNegative".
 *   level       the current AdsrLevel.
 * Output in *step (AdsrStep) and *counter_inc (CounterIncrement). */
static inline void spu_env_params(uint8_t rate, uint8_t all_bits, int exponential, int decreasing,
                                  int invert_step, int16_t level, int *step, int *counter_inc);

/* One tick of a voice's ADSR envelope.
 *   *level    0..0x7FFF on entry
 *   *counter  psx-spx's Counter (only its low 16 bits matter)
 *   *phase    SPU_ENV_*; any other value: nothing happens
 *   regs      the voice's ADSR registers: 1F801C08h in bits 0-15, 1F801C0Ah in bits 16-31 */
static inline void spu_adsr_tick(uint16_t *level, uint32_t *counter, uint8_t *phase, uint32_t regs);

/* One tick of a volume register in sweep mode (bit 15 of `reg` is set; the caller checks).
 *   *level    the live volume, a signed 16-bit value
 *   *counter  as above
 *   reg       the volume register. psx-spx describes bits 12-14 and the 7-bit rate.
 * The envelope works on a value in 0..0x7FFF in the phase's domain (negative phase: the level
 * mirrored); a level on the wrong side of zero counts as 0. The result is written back in the same
 * domain, kept in 0..0x7FFF. */
static inline void spu_sweep_tick(int16_t *level, uint32_t *counter, uint16_t reg);

#endif
