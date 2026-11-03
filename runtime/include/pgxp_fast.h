/* pgxp_fast.h — the light memory mode's common load/store, inline in compiled code.
 *
 * With [video] pgxp_mem = "live" (pgxp.cpp, s_live_bits) most word loads and
 * stores touch no vertex: a load from a RAM slot with no live bit only resets
 * the register's shadow, and a store of a register with no tracked half into
 * such a slot does nothing. The hooks did that behind a call per access
 * (psx_pgxp_load / psx_pgxp_store, ~8% of the game thread with the 3D Fix on,
 * perf study). Here the generated code does those two cases itself, writing
 * the same state, and calls the hook for everything else, which decides from
 * the start exactly as before: an inactive engine, CPU mode, a marked slot, a
 * register with a tracked half, any address outside the RAM window, another
 * opcode. Outside CPU mode the ALU hook's register moves and LUI too.
 *
 * The engine keeps the state these read here instead of in its own statics
 * (pgxp.cpp aliases them). Not in overlay DLLs (they call the host's hook
 * table) nor in pgxp.cpp itself. Included last by psx_runtime.h, after
 * pgxp_hooks.h, whose PGXP_LOAD / PGXP_STORE it replaces. */
#ifndef PGXP_FAST_H
#define PGXP_FAST_H

#include <stdint.h>
#include "pgxp.h"
#include "psx_memory.h"
#include "mod_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PGXPValue {
    int32_t  x16, y16;   /* sub-pixel screen coords, 16.16                    */
    uint16_t z;          /* projected SZ depth (perspective source), 0 = none */
    uint16_t flags;
    uint32_t value;      /* the guest word this shadow describes              */
    uint32_t gen;        /* valid iff == the engine's generation              */
    PGXPProjection projection;
} PGXPValue;

typedef struct PGXPScalar {
    double   v;          /* precise value of the 32-bit register              */
    uint32_t value;      /* the register value it describes                   */
    uint32_t gen;
} PGXPScalar;

typedef struct PGXPFast {
    int        active;      /* the hooks' gate: enabled and not suppressed */
    int        mem_live;    /* the light memory mode */
    int        cpu_mode;
    uint32_t   gen;         /* the shadow generation */
    PGXPValue *ram;         /* RAM + mod regions shadow (lazily allocated) */
    uint8_t   *live_bits;   /* the light mode's slot bits */
} PGXPFast;
extern PGXPFast   g_pgxp_fast;
extern PGXPValue  g_pgxp_gpr[34];     /* 32 GPRs + HI + LO */
extern PGXPScalar g_pgxp_gpr_s[34];

#define PGXP_FAST_F_VXY 3u   /* PGXP_F_VX | PGXP_F_VY (pgxp.cpp) */

#if defined(PSX_PGXP) && PSX_PGXP && !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PGXP_ENGINE_IMPL)

/* pgxp.cpp's live-bit index of the first scratchpad slot (ck_index: the RAM,
 * GPU-DMA aperture and mod-memory slots come first). */
#define PGXP_FAST_SCRATCH_BIT0 ((PSX_MAIN_RAM_BACKING_BYTES >> 2) + (PSX_MOD_GPU_DMA_APERTURE_SIZE >> 2) + \
                                (PSX_MOD_MEMORY_SIZE >> 2))

/* The slot of a word access has no live bit (1), has one (0), or the hook
 * must decide (-1). pgxp.cpp pgxp_ptr + live_marked for the RAM window and the
 * scratchpad (its slots always exist, so only the bits decide). */
static inline int pgxp_fast_unmarked(uint32_t addr) {
    const uint32_t m = addr & 0x1FFFFFFFu;
    const uint8_t *bits = g_pgxp_fast.live_bits;
    if (m >= PSX_MAIN_RAM_WINDOW_BYTES) {
        if ((m & 0xFFFFFC00u) != 0x1F800000u || !bits) return -1;
        const uint32_t i = PGXP_FAST_SCRATCH_BIT0 + ((m & 0x3FCu) >> 2);
        return !((bits[i >> 3] >> (i & 7u)) & 1u);
    }
    if (!g_pgxp_fast.ram) return 1;              /* no slot: never marked */
    if (!bits) return -1;
    const uint32_t i = psx_ram_canonical_offset(m) >> 2;
    return !((bits[i >> 3] >> (i & 7u)) & 1u);
}

/* 1: done (psx_pgxp_load would have done exactly this), 0: call it. */
static inline int pgxp_fast_load(uint32_t instr, uint32_t addr, uint32_t value) {
    if (!g_pgxp_fast.active) return 1;
    const uint32_t rt = (instr >> 16) & 31u;
    if (rt == 0u) return 1;
    if ((instr >> 26) != 0x23u || !g_pgxp_fast.mem_live || pgxp_fast_unmarked(addr) != 1)
        return 0;
    g_pgxp_gpr_s[rt].gen = 0;                    /* memory carries no scalars */
    PGXPValue *d = &g_pgxp_gpr[rt];
    d->x16 = 0; d->y16 = 0; d->z = 0;
    d->flags = 0;
    d->value = value;
    d->gen = g_pgxp_fast.gen;
    return 1;
}

/* 1: done (psx_pgxp_store would have returned without a change), 0: call it. */
static inline int pgxp_fast_store(uint32_t instr, uint32_t addr) {
    if (!g_pgxp_fast.active) return 1;
    if ((instr >> 26) != 0x2Bu || !g_pgxp_fast.mem_live || g_pgxp_fast.cpu_mode) return 0;
    const uint32_t rt = (instr >> 16) & 31u;
    if (rt != 0u) {
        const PGXPValue *s = &g_pgxp_gpr[rt];
        if (s->gen == g_pgxp_fast.gen && (s->flags & PGXP_FAST_F_VXY)) return 0;
    }
    return pgxp_fast_unmarked(addr) == 1;
}

/* The ALU hook outside CPU mode is pgxp.cpp alu_halves alone. Its register
 * moves (OR / ADD(U) / SUB(U) with $zero, ADDI(U) with 0) copy the source's
 * shadow and drop the halves the result no longer matches (pv_validate), or
 * reset; LUI writes both halves exact. Every other op calls the hook. */
static inline void pgxp_fast_move(uint32_t d, uint32_t s, uint32_t result) {
    PGXPValue *dst = &g_pgxp_gpr[d];
    if (s != 0u && g_pgxp_gpr[s].gen == g_pgxp_fast.gen) {
        PGXPValue v = g_pgxp_gpr[s];             /* d may be s */
        const uint32_t diff = v.value ^ result;
        if (diff) v.flags &= (uint16_t)~8u;      /* PGXP_F_PROJECTION */
        if ((v.flags & 1u) && (diff & 0x0000FFFFu)) v.flags &= (uint16_t)~(1u | 16u | 4u);
        if ((v.flags & 2u) && (diff & 0xFFFF0000u)) v.flags &= (uint16_t)~(2u | 32u | 4u);
        v.value = result;
        *dst = v;
        return;
    }
    dst->x16 = 0; dst->y16 = 0; dst->z = 0;
    dst->flags = 0;
    dst->value = result;
    dst->gen = g_pgxp_fast.gen;
}

/* 1: done (psx_pgxp_alu would have done exactly this), 0: call it. */
static inline int pgxp_fast_alu(uint32_t instr, uint32_t result) {
    if (!g_pgxp_fast.active) return 1;
    if (g_pgxp_fast.cpu_mode) return 0;          /* the scalar tier */
    const uint32_t op = instr >> 26, rs = (instr >> 21) & 31u, rt = (instr >> 16) & 31u;
    if (op == 0u) {
        const uint32_t funct = instr & 63u, rd = (instr >> 11) & 31u;
        if (funct == 0x11u || funct == 0x13u) return 0;   /* MTHI / MTLO */
        if (rd == 0u) return 1;
        if ((funct == 0x25u && (rs == 0u || rt == 0u)) ||
            (funct >= 0x20u && funct <= 0x23u && (rt == 0u || (funct < 0x22u && rs == 0u)))) {
            pgxp_fast_move(rd, rt == 0u ? rs : rt, result);
            return 1;
        }
        return 0;
    }
    if (rt == 0u) return 1;
    if (op == 0x0Fu) {                           /* LUI: both halves exact */
        PGXPValue *dst = &g_pgxp_gpr[rt];
        dst->x16 = 0;
        dst->y16 = ((int32_t)result >> 16) << 16;
        dst->z = 0;
        dst->flags = PGXP_FAST_F_VXY;
        dst->value = result;
        dst->gen = g_pgxp_fast.gen;
        return 1;
    }
    if ((op == 0x08u || op == 0x09u) && (instr & 0xFFFFu) == 0u) {
        pgxp_fast_move(rt, rs, result);
        return 1;
    }
    return 0;
}

#undef PGXP_LOAD
#undef PGXP_STORE
#undef PGXP_ALU_K
#define PGXP_LOAD(instr, addr, val) \
    do { if (PGXP_MEM_ON(instr) && !pgxp_fast_load((instr), (addr), (val))) \
             psx_pgxp_load(cpu, (instr), (addr), (val)); } while (0)
#define PGXP_STORE(instr, addr, val) \
    do { if (PGXP_MEM_ON(instr) && !pgxp_fast_store((instr), (addr))) \
             psx_pgxp_store(cpu, (instr), (addr), (val)); } while (0)
#define PGXP_ALU_K(k, instr, res, s1, s2) \
    do { if ((g_pgxp_alu_mask & (1u << (k))) && !pgxp_fast_alu((instr), (res))) \
             PGXP_ALU(instr, res, s1, s2); } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PGXP_FAST_H */
