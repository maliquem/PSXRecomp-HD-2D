/* psx_icache.c — R3000A instruction-cache FETCH cost model.
 *
 * The cache itself is psx_icache_core.inc (clean-room, docs/ICACHE_CLEAN_ROOM.md).
 * This file connects it to the CPU: reset, the transactional shadow for lockstep,
 * and charging the cycles. The HIT path is inlined in psx_icache.h.
 */
#define PSX_ICACHE_IMPL   /* the real psx_icache_fetch, not the header's inline */
#include "psx_icache.h"
#include "cpu_state.h"
#include "psx_cycles.h"
#include "psx_instr_cost.h"   /* psx_fetch_uncached: shared with both emitters */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "psx_icache_core.h"
#include "psx_icache_core.inc"

int psx_icache_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("PSX_ICACHE");
        s = (e == NULL || e[0] == '\0') ? 1 : (e[0] != '0');
    }
    return s;
}

/* One tag per cached instruction word (psx_icache_core.h). */
uint32_t g_psx_icache_tv[1024];
int g_psx_icache_active = -1;

/* Whole-call shadow: a transactional view of the cache tags so an AOT/diff
 * shadow replay can evolve the cache from the recorded entry state and then
 * restore the live view (master's shadow machinery, retargeted at the global
 * tag array the interpreter fast path exports). */
static uint32_t s_icache_shadow_entry[1024];
static uint32_t s_icache_shadow_live[1024];
static int      s_icache_shadow_state = 0; /* 0 none, 1 recorded, 2 replay */

int psx_icache_shadow_record_begin(void) {
    if (s_icache_shadow_state != 0) return 0;
    memcpy(s_icache_shadow_entry, g_psx_icache_tv, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 1;
    return 1;
}

int psx_icache_shadow_replay_begin(void) {
    if (s_icache_shadow_state != 1) return 0;
    memcpy(s_icache_shadow_live, g_psx_icache_tv, sizeof g_psx_icache_tv);
    memcpy(g_psx_icache_tv, s_icache_shadow_entry, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 2;
    return 1;
}

void psx_icache_shadow_replay_end(void) {
    if (s_icache_shadow_state != 2) return;
    memcpy(g_psx_icache_tv, s_icache_shadow_live, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 0;
}

void psx_icache_shadow_abort(void) {
    if (s_icache_shadow_state == 2)
        memcpy(g_psx_icache_tv, s_icache_shadow_live, sizeof g_psx_icache_tv);
    s_icache_shadow_state = 0;
}

void psx_icache_reset(void) {
    g_psx_icache_active = psx_icache_enabled();
    for (int i = 0; i < 1024; i++) g_psx_icache_tv[i] = 0x1u;
}

uint64_t g_psx_icache_misses = 0;   /* TCP core_rates */

void psx_icache_fetch_miss(CPUState* cpu, uint32_t addr) {
    { extern int g_ls_replay_active;
      /* Ordinary lockstep replay must not perturb the shared cache. The whole-
       * call shadow owns a private transactional cache view and may evolve it. */
      if (g_ls_replay_active && s_icache_shadow_state != 2) return;
    }
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    uint32_t cycles;
    if (!icache_fetch(g_psx_icache_tv, addr, psx_fetch_uncached(addr), &cycles)) return;

    /* A miss stalls the fetch, which uses up the time a pending load was giving back. */
    g_psx_icache_misses++;
    cpu->read_absorb[cpu->read_absorb_which] = 0u;
    cpu->read_absorb_which = 0u;
    psx_cpu_charge(cycles);   /* CPU charge: [timing] guest_cycle_scale */
#else
    (void)cpu;
    (void)addr;
#endif
}

/* A store made while COP0 SR.IsC is set reaches the cache. This model keeps no
 * instruction words (compiled code supplies them), so only tag changes matter.
 * SR.SwC (swapped caches) is not modelled. */
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* Same gate as a refill: lockstep replay must not move the shared tags;
     * the whole-call shadow evolves its private view. */
    if (g_ls_replay_active && s_icache_shadow_state != 2) return;
    if (g_psx_icache_active < 0) g_psx_icache_active = psx_icache_enabled();
    if (!g_psx_icache_active) return;
    icache_tag_store(g_psx_icache_tv, biu, addr, value);
#else
    (void)biu;
    (void)addr;
    (void)value;
#endif
}

/* The addressable fetch (overlay DLLs' callback, older objects): the tag
 * compare first, as in psx_icache.h's inline; a hit changes nothing. */
void psx_icache_fetch(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_psx_icache_tv[(addr & 0xFFCu) >> 2] == addr) return;
#endif
    psx_icache_fetch_miss(cpu, addr);
}

void psx_icache_fetch_fn(CPUState* cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_psx_icache_tv[(addr & 0xFFCu) >> 2] == addr) return;
#endif
    psx_icache_fetch_miss(cpu, addr);
}
