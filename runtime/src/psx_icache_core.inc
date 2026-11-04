/* psx_icache_core.inc: the R3000A instruction cache's tag store, for timing only.
 *
 * Sources: psx-spx (memorymap.md "i-Cache", memorycontrol.md "FFFE0130h", cpuspecifications.md "SR")
 * and what a black box was seen to do. A rule seen only in the black box is marked "observed".
 * The hit test and the reset value are fixed by psx_icache_core.h.
 */
#include <stdint.h>
#include "psx_icache_core.h"

/* Writes the four tags of addr's line. Bit N of `valid` says word N of the line holds code. */
static inline void icache_set_line(uint32_t *tags, uint32_t addr, uint32_t valid) {
    uint32_t line_addr = addr & ~0xFu;
    uint32_t first = (addr & 0xFF0u) >> 2;
    for (uint32_t word = 0; word < 4; word++) {
        uint32_t word_addr = line_addr | (word << 2);
        /* observed: an invalid word holds its own address with bit 1 set, never the reset value. */
        tags[first + word] = (valid & (1u << word)) ? word_addr : (word_addr | 2u);
    }
}

static inline int icache_fetch(uint32_t *tags, uint32_t addr, int uncached, uint32_t *cycles) {
    /* The header's hit test. observed: it comes first, so `uncached` does not prevent a hit. */
    if (tags[(addr & 0xFFCu) >> 2] == addr) {
        *cycles = 0;
        return 0;
    }

    /* psx-spx: KSEG1 bypasses the cache, so the line stays as it is. observed: this costs 4 cycles,
     * in every region. */
    if (uncached) {
        *cycles = 4;
        return 1;
    }

    /* psx-spx: the fill runs from the accessed word to the end of the line, with no wrapping.
     * observed: the words before it end up invalid even if they were valid under this same tag,
     * and the cost is 3 cycles plus 1 for each word filled, in every region. */
    uint32_t entry = (addr >> 2) & 3u;
    icache_set_line(tags, addr, (0xFu << entry) & 0xFu);
    *cycles = 3u + (4u - entry);
    return 1;
}

static inline void icache_tag_store(uint32_t *tags, uint32_t biu, uint32_t addr, uint32_t value) {
    /* psx-spx: these stores reach the i-cache with IS1 set. observed: DS changes nothing here,
     * and neither does any other BIU bit, scratchpad addresses included. */
    if (!(biu & ICACHE_BIU_IS1))
        return;

    if (biu & ICACHE_BIU_TAG) {
        /* psx-spx: the low 4 data bits are the valid bits, and the tag's address comes from the
         * store's address. observed: TAG wins over LOCK and INV, and a store to a non-aligned
         * address acts as value 0 whatever the register holds. */
        icache_set_line(tags, addr, (addr & 3u) ? 0u : (value & 0xFu));
    } else if (biu & (ICACHE_BIU_LOCK | ICACHE_BIU_INV)) {
        /* observed: either bit invalidates the whole line, taking the store's address as its tag,
         * whatever the value and whatever the line held. */
        icache_set_line(tags, addr, 0u);
    }
    /* Otherwise the store writes an instruction word, which leaves the tags alone (header). */
}
