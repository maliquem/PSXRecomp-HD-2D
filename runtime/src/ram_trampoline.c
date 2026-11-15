/* ram_trampoline.c — see header. Moved from psx_unknown_dispatch; the known
 * divergences from hardware (jr $zero accepted, dropped register writebacks,
 * loose JR field check) are kept on purpose and pinned by
 * test_unknown_dispatch_trampolines.c. */
#include "ram_trampoline.h"

static int found(psx_trampoline_t *out, int kind, uint32_t target) {
    out->kind = kind;
    out->target = target;
    return kind;
}

/* The delay slot runs before the jump lands (R3000), so a shape resolves only
 * when its slot is one this decoder can apply: nop, or addiu rT,$zero,imm. */
static int slot_ok(uint32_t ds, psx_trampoline_t *out) {
    if (ds == 0u) return 1;
    if ((ds & 0xFFE00000u) != 0x24000000u) return 0; /* ADDIU rT, $zero, imm */
    if (((ds >> 16) & 0x1F) != 0) { /* a write to $zero is dropped */
        out->ds_reg = (int)((ds >> 16) & 0x1F);
        out->ds_val = (uint32_t)(int32_t)(int16_t)(ds & 0xFFFF);
    }
    return 1;
}

static int need(psx_trampoline_t *out, int n) {
    out->need_words = n;
    return out->kind = PSX_TRAMP_NEED_WORDS;
}

int psx_trampoline_decode(uint32_t addr, const uint32_t *w, int n_words,
                          const uint32_t gpr[32], psx_trampoline_t *out) {
    uint32_t w0 = w[0];
    uint32_t w1 = w[1];
    uint32_t w2 = w[2];
    uint32_t op0 = (w0 >> 26) & 0x3F;
    uint32_t op1 = (w1 >> 26) & 0x3F;

    out->kind = PSX_TRAMP_NONE;
    out->target = 0;
    out->ds_reg = -1;
    out->ds_val = 0;
    out->need_words = n_words;

    /* Pattern 1: J target / slot */
    if (op0 == 2)
        return slot_ok(w1, out)
            ? found(out, PSX_TRAMP_J, (addr & 0xF0000000u) | ((w0 & 0x03FFFFFFu) << 2))
            : PSX_TRAMP_NONE;

    /* Pattern 2: JR rs / slot */
    if (op0 == 0 && (w0 & 0x3F) == 0x08)
        return slot_ok(w1, out) ? found(out, PSX_TRAMP_JR, gpr[(w0 >> 21) & 0x1F])
                                : PSX_TRAMP_NONE;

    /* Pattern 3: addiu rN, $zero, imm / jr rN (small address trampoline) */
    if (op0 == 0x09) { /* ADDIU */
        uint32_t rs0 = (w0 >> 21) & 0x1F;
        uint32_t rt0 = (w0 >> 16) & 0x1F;
        if (rs0 == 0) { /* addiu rN, $zero, imm = li rN, imm */
            int16_t imm = (int16_t)(w0 & 0xFFFF);
            uint32_t target = (uint32_t)(int32_t)imm;
            if ((w1 & 0xFC1FFFFF) == 0x00000008) { /* jr rs */
                uint32_t jr_rs = (w1 >> 21) & 0x1F;
                if (jr_rs == rt0) /* w2 is the delay slot */
                    return slot_ok(w2, out) ? found(out, PSX_TRAMP_LI_JR, target)
                                            : PSX_TRAMP_NONE;
            }
        }
    }

    /* Pattern 4: lui rN, hi / addiu|ori rN, rN, lo / jr rN / slot
     * All three instructions must be present. Without the jr check,
     * any function prologue that loads a constant via lui+ori would
     * be misidentified as a trampoline (see 0xBFC3DF90 incident). */
    if (op0 == 0x0F) { /* LUI */
        uint32_t rt0 = (w0 >> 16) & 0x1F;
        uint32_t hi_val = (w0 & 0xFFFF) << 16;
        uint32_t computed = 0;
        int have_target = 0;

        if (op1 == 0x09) { /* ADDIU */
            uint32_t rs1 = (w1 >> 21) & 0x1F;
            uint32_t rt1 = (w1 >> 16) & 0x1F;
            if (rs1 == rt0 && rt1 == rt0) {
                int16_t lo_val = (int16_t)(w1 & 0xFFFF);
                computed = hi_val + (uint32_t)(int32_t)lo_val;
                have_target = 1;
            }
        }
        if (!have_target && op1 == 0x0D) { /* ORI */
            uint32_t rs1 = (w1 >> 21) & 0x1F;
            uint32_t rt1 = (w1 >> 16) & 0x1F;
            if (rs1 == rt0 && rt1 == rt0) {
                computed = hi_val | (w1 & 0xFFFF);
                have_target = 1;
            }
        }
        /* Only resolve if w2 is jr rN targeting the same register. */
        if (have_target && (w2 & 0xFC1FFFFF) == 0x00000008) {
            uint32_t jr_rs = (w2 >> 21) & 0x1F;
            if (jr_rs == rt0) {
                if (n_words < 4) return need(out, 4);
                return slot_ok(w[3], out) ? found(out, PSX_TRAMP_LUI_JR, computed)
                                          : PSX_TRAMP_NONE;
            }
        }

        /* Pattern 5: BIOS vector dispatch table.
         * lui  rN, hi             w0
         * addiu rN, rN, lo        w1  (base = hi|lo)
         * sll  rM, rM, 2          w2  (index <<= 2)
         * addu rN, rN, rM         w3  (ptr = base + index*4)
         * lw   rN, 0(rN)          w4  (func = *ptr)
         * jr   rN                 w5  (or nop at w5, jr at w6)
         * slot                    w6  (w7 after the nop)
         * This is the A0/B0/C0 dispatch pattern the BIOS writes at
         * 0x500+.  rM holds the function number (set by the A0/B0/C0
         * trampoline delay slot before we get here). */
        if (have_target) {
            uint32_t w3, w4, w5;
            if (n_words < 6) return need(out, 6);
            w3 = w[3];
            w4 = w[4];
            w5 = w[5];

            /* w2: sll rM, rM, 2  (opcode 0, func 0, sa 2) */
            if ((w2 & 0xFC0007FF) == 0x00000080) { /* SLL with sa=2 */
                uint32_t idx_rt = (w2 >> 16) & 0x1F;
                uint32_t idx_rd = (w2 >> 11) & 0x1F;
                /* w3: add/addu rN, rN, rM */
                if ((w3 & 0xFC0007FE) == 0x00000020) { /* ADD or ADDU */
                    uint32_t a_rs = (w3 >> 21) & 0x1F;
                    uint32_t a_rt = (w3 >> 16) & 0x1F;
                    uint32_t a_rd = (w3 >> 11) & 0x1F;
                    if (a_rs == rt0 && a_rt == idx_rd && a_rd == rt0) {
                        /* w4: lw rN, 0(rN) */
                        if ((w4 & 0xFFFF0000) == (0x8C000000u | ((uint32_t)rt0 << 21) | ((uint32_t)rt0 << 16))) {
                            /* w5: jr rN — or nop then jr rN (load delay slot) */
                            int jr_at = w5 == 0x00000000u ? 6 : 5;
                            uint32_t jr_word;
                            if (n_words < jr_at + 2) return need(out, jr_at + 2);
                            jr_word = w[jr_at];
                            if ((jr_word & 0xFC1FFFFF) == 0x00000008 && ((jr_word >> 21) & 0x1F) == rt0 &&
                                slot_ok(w[jr_at + 1], out)) {
                                uint32_t index_val = gpr[idx_rt];
                                return found(out, PSX_TRAMP_TABLE, computed + (index_val << 2));
                            }
                        }
                    }
                }
            }
        }
    }
    return PSX_TRAMP_NONE;
}
