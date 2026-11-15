/* ram_trampoline.h — pure decoder for the small jump trampolines the BIOS
 * writes into RAM (A0/B0/C0 vectors and the 0x500+ dispatch table), used by
 * psx_unknown_dispatch. No memory, CPUState or MMIO access: the caller reads
 * the words and applies the result. */
#ifndef RAM_TRAMPOLINE_H
#define RAM_TRAMPOLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSX_TRAMP_NEED_WORDS = -1, /* read words up to out->need_words, call again */
    PSX_TRAMP_NONE       = 0,
    PSX_TRAMP_J          = 1,  /* j target */
    PSX_TRAMP_JR         = 2,  /* jr rs */
    PSX_TRAMP_LI_JR      = 3,  /* addiu rN,$zero,imm / jr rN / [addiu rT,$zero,imm2] */
    PSX_TRAMP_LUI_JR     = 4,  /* lui rN / addiu|ori rN,rN / jr rN */
    PSX_TRAMP_TABLE      = 5   /* lui/addiu|ori/sll/add|addu/lw/[nop]/jr: target is the table slot */
};

#define PSX_TRAMPOLINE_MAX_WORDS 7

typedef struct {
    int      kind;       /* PSX_TRAMP_*; same value the decoder returns */
    uint32_t target;     /* final PC; for PSX_TRAMP_TABLE the slot address to read the PC from */
    int      ds_reg;     /* delay-slot register write (LI_JR only), -1 if none */
    uint32_t ds_val;
    int      need_words; /* with PSX_TRAMP_NEED_WORDS: total words required */
} psx_trampoline_t;

/* w[0..n_words-1] are the words at addr, addr+4, ...; n_words >= 3. Returns
 * PSX_TRAMP_NEED_WORDS when the decision needs more words, so the caller can
 * keep the reads lazy and in address order. */
int psx_trampoline_decode(uint32_t addr, const uint32_t *w, int n_words,
                          const uint32_t gpr[32], psx_trampoline_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RAM_TRAMPOLINE_H */
