/* psx_icache_core.h: the R3000A instruction cache's tag store. Interface only.
 *
 * Implemented in psx_icache_core.inc, a clean-room rewrite from psx-spx (docs/ICACHE_CLEAN_ROOM.md).
 *
 * Terms are psx-spx's: memorycontrol.md (FFFE0130h, BCC), memorymap.md (the caches and the memory
 * regions) and cpuspecifications.md (COP0 SR, IsC).
 *
 * The cache is modelled for its timing only: there are no instruction words here, just tags.
 *
 * `tags` is 1024 words, one for each instruction word the 4 KB cache holds: the word at cache offset
 * X (bits 2-11 of an address) has tags[X >> 2]. These facts are fixed, because compiled code and
 * savestates depend on them:
 *   - A fetch of the word-aligned address A hits exactly when tags[(A & 0xFFC) >> 2] == A. The whole
 *     address is the tag, as the CPU presented it, segment bits included.
 *   - At reset every word is 1, which no word-aligned address can equal.
 *   - So a word that is not valid must hold something that differs from every aligned address. Exactly
 *     what it holds matters too (savestates are compared byte by byte): the black box shows it.
 */
#ifndef PSX_ICACHE_CORE_H
#define PSX_ICACHE_CORE_H
#include <stdint.h>

/* Bits of FFFE0130h, psx-spx's names. */
enum {
    ICACHE_BIU_LOCK = 0x001, ICACHE_BIU_INV = 0x002, ICACHE_BIU_TAG = 0x004,
    ICACHE_BIU_DS = 0x080, ICACHE_BIU_IS1 = 0x800
};

/* One instruction fetch.
 *   addr      the word-aligned virtual address
 *   uncached  nonzero when addr is in a region the cache does not serve
 *   *cycles   the extra cycles this fetch takes: 0 on a hit
 * Returns 1 on a miss, 0 on a hit. Updates `tags` as the hardware would. */
static inline int icache_fetch(uint32_t *tags, uint32_t addr, int uncached, uint32_t *cycles);

/* A CPU store made while COP0 SR.IsC is set: the store goes to the cache, not to memory.
 *   biu    the current value of FFFE0130h
 *   addr   the store's address, any alignment
 *   value  the stored register value, with the byte lanes a store of this address uses left as the
 *          register has them
 * Only the tags matter here: a store that would write an instruction word leaves `tags` as it is. */
static inline void icache_tag_store(uint32_t *tags, uint32_t biu, uint32_t addr, uint32_t value);

#endif
