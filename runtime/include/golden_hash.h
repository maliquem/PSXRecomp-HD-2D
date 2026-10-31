#ifndef GOLDEN_HASH_H
#define GOLDEN_HASH_H

/* Golden verification (game repo tools/golden.py): per emulated frame, hashes of the guest-visible
 * result — main RAM, scratchpad, CPU and GTE registers, the PS1 cycle count, every word sent to the
 * GPU (GP0/GP1, which fully determine what it draws) and every sample the SPU rendered — so two
 * builds can be proven to compute the same thing frame by frame. `pgxp` adds the 3D Fix engine
 * (pgxp.cpp): every precise vertex/depth answer it gave the GPU this frame, and at the frame end
 * its whole shadow state (registers, scalars, main RAM and scratchpad shadows), so a change to the
 * engine can be proven exact too. Off by default: armed over TCP
 * (`golden`), from the next savestate load so two runs line up on the same guest frame. While off
 * the hooks cost one test of g_golden_on. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

extern int g_golden_on;

typedef struct GoldenEntry {
    uint32_t frame;          /* frames since arming (0 = the first frame after the load) */
    uint32_t gpu_words;      /* GP0/GP1 words this frame */
    uint32_t spu_samples;    /* stereo frames rendered this frame */
    uint32_t pad;
    uint64_t cycles;         /* PS1 cycle count at the frame end */
    uint64_t ram, scratch, cpu, gte, gpu, spu;
    uint64_t pgxp;
} GoldenEntry;

void golden_gpu_word(uint32_t word, int port);
void golden_spu_block(const int16_t *stereo, uint32_t frames);
/* An answer the PGXP engine gave a consumer: pgxp.cpp calls through this pointer (defined there,
 * NULL while the recorder is off, so the engine's own tests link without the recorder). */
extern void (*g_golden_pgxp_note)(uint64_t a, uint64_t b);
/* pgxp.cpp: hash of the engine's whole shadow state. */
uint64_t pgxp_golden_hash(void);
/* Frame boundary (debug_server_record_frame). */
void golden_frame(const struct CPUState *cpu);
/* Savestate load completed: an armed-on-load recorder starts here. */
void golden_note_savestate_loaded(void);
void golden_arm(int on, int on_load);
uint64_t golden_total(void);
int golden_entries(uint32_t from_frame, uint32_t count, const GoldenEntry **ring, uint32_t *cap,
                   uint32_t *start);

#ifdef __cplusplus
}
#endif

#endif /* GOLDEN_HASH_H */
