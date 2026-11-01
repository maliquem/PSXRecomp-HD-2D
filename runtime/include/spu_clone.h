/* spu_clone.h - the two host-only SPU clones (spu_clone.inc): spus_ (effects,
 * the guest's clock) and spum_ (music, its own clock: spum_set_clock). The
 * same calls as spu.h; only audio_music.c uses them. */
#ifndef SPU_CLONE_H
#define SPU_CLONE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_CLONE_DECL(P)                                              \
    void     P##init(void);                                            \
    void     P##catch_up(void);                                        \
    uint32_t P##output_available(void);                                \
    uint32_t P##output_pop(int16_t *out_stereo, uint32_t max_frames);  \
    void     P##output_flush(void);                                    \
    void     P##write(uint32_t addr, uint32_t value);                  \
    void     P##dma_write(uint32_t word);                              \
    uint32_t P##dma_read(void);                                        \
    uint32_t P##snapshot_bytes(void);                                  \
    int      P##snapshot_read(const uint8_t *p, uint32_t len);                uint8_t *P##get_ram_ptr(void);
SPU_CLONE_DECL(spus_)
SPU_CLONE_DECL(spum_)
#undef SPU_CLONE_DECL
void spum_set_clock(uint64_t cycle);

#ifdef __cplusplus
}
#endif

#endif
