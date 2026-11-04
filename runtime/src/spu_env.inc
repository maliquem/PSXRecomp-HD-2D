/* spu_env.inc: the SPU's volume envelopes (ADSR and volume sweep).
 *
 * Source: psx-spx, "Envelope Operation depending on Shift/Step/Mode/Direction" and the ADSR and
 * volume register descriptions. Comments marked "observed" are rules psx-spx does not state; they
 * were observed from the reference through black-box comparison. */

static inline void spu_env_params(uint8_t rate, uint8_t all_bits, int exponential, int decreasing,
                                  int invert_step, int16_t level, int *step, int *counter_inc) {
    int shift = rate >> 2;
    int step_value = rate & 3;
    int adsr_step = 7 - step_value;
    int counter_increment;

    if (invert_step)
        adsr_step = ~adsr_step;
    adsr_step *= 1 << (shift < 11 ? 11 - shift : 0);
    counter_increment = 0x8000 >> (shift > 11 ? shift - 11 : 0);

    /* Observed: the "fake exponential" threshold is bits 13 and 14 of the level both set. In
     * 0..7FFFh that means level >= 6000h, so 6000h itself already counts. */
    if (exponential && !decreasing && (level & 0x6000) == 0x6000) {
        if (shift < 10) {
            adsr_step /= 4;
        } else if (shift >= 11) {
            counter_increment /= 4;
        } else {
            adsr_step /= 2;
            counter_increment /= 2;
        }
    } else if (exponential && decreasing) {
        /* Observed: the division by 8000h rounds down (arithmetic shift), not toward zero. */
        adsr_step = (adsr_step * level) >> 15;
    }

    /* Observed: all_bits works as a mask, so with a 5-bit rate (7Ch) the step field is ignored. */
    if ((rate & all_bits) != all_bits && counter_increment < 1)
        counter_increment = 1;

    *step = adsr_step;
    *counter_inc = counter_increment;
}

static inline void spu_adsr_tick(uint16_t *level, uint32_t *counter, uint8_t *phase, uint32_t regs) {
    uint8_t rate, all_bits = 0x7F;
    int exponential, decreasing, step, counter_inc, new_level;

    /* Observed: reaching 7FFFh does not end Attack on that tick. The next tick starts by moving to
     * Decay and already runs with Decay's parameters. */
    if (*phase == SPU_ENV_ATTACK && *level == 0x7FFF)
        *phase = SPU_ENV_DECAY;

    switch (*phase) {
    case SPU_ENV_ATTACK:
        rate = (uint8_t)((regs >> 8) & 0x7F);
        exponential = (regs >> 15) & 1;
        decreasing = 0;
        break;
    case SPU_ENV_DECAY:
        rate = (uint8_t)(((regs >> 4) & 0x0F) << 2);
        all_bits = 0x7C;
        exponential = 1;
        decreasing = 1;
        break;
    case SPU_ENV_SUSTAIN:
        rate = (uint8_t)((regs >> 22) & 0x7F);
        exponential = (regs >> 31) & 1;
        decreasing = (regs >> 30) & 1;
        break;
    case SPU_ENV_RELEASE:
        rate = (uint8_t)(((regs >> 16) & 0x1F) << 2);
        all_bits = 0x7C;
        exponential = (regs >> 21) & 1;
        decreasing = 1;
        break;
    default:
        return;
    }

    spu_env_params(rate, all_bits, exponential, decreasing, decreasing, (int16_t)*level, &step, &counter_inc);

    *counter += (uint32_t)counter_inc;
    if ((*counter & 0x8000) == 0)
        return;
    /* Observed: the counter restarts from 0 on every step. */
    *counter = 0;

    new_level = *level + step;
    if (decreasing) {
        if (new_level < 0)
            new_level = 0;
    } else if (new_level > 0x7FFF) {
        new_level = 0x7FFF;
    }
    *level = (uint16_t)new_level;

    /* Observed: Decay ends only on a step, and only once the level is below the sustain level;
     * landing exactly on it stays in Decay. Release has no exit: it stays in Release at level 0. */
    if (*phase == SPU_ENV_DECAY && new_level < (int)(((regs & 0x0F) + 1) * 0x800))
        *phase = SPU_ENV_SUSTAIN;
}

static inline void spu_sweep_tick(int16_t *level, uint32_t *counter, uint16_t reg) {
    int exponential = (reg >> 14) & 1;
    int decreasing = (reg >> 13) & 1;
    int phase_negative = (reg >> 12) & 1;
    int domain_level = phase_negative ? -(int)*level : *level;
    int step, counter_inc;

    /* spu_env.h, not psx-spx: the phase only mirrors the level into 0..7FFFh, so the step is
     * inverted by the direction alone. Observed: -8000h in a negative phase counts as 7FFFh. */
    if (domain_level < 0)
        domain_level = 0;
    if (domain_level > 0x7FFF)
        domain_level = 0x7FFF;

    spu_env_params((uint8_t)(reg & 0x7F), 0x7F, exponential, decreasing, decreasing, (int16_t)domain_level,
                   &step, &counter_inc);

    *counter += (uint32_t)counter_inc;
    if ((*counter & 0x8000) == 0)
        return;
    /* Observed: the counter restarts from 0 on every step. */
    *counter = 0;

    domain_level += step;
    if (domain_level < 0)
        domain_level = 0;
    if (domain_level > 0x7FFF)
        domain_level = 0x7FFF;
    *level = (int16_t)(phase_negative ? -domain_level : domain_level);
}
