/* psx_gpr_locals.h — [recompiler] gpr_locals (docs/GPR_LOCALS.md).
 *
 * Generated functions written with the pass name a guest register as PSX_R(n)
 * and call the inline helpers through the PSX_B2_* forms below, giving each
 * the stores (OUT: PSX_B2_ST(n)...) and reloads (IN: PSX_B2_LD(n)...) the
 * generator worked out for that point. With PSX_GPR_LOCALS on, PSX_R(n) is a C
 * local and the forms take the helper's common case inline, with no call, and
 * run OUT (and IN) before the helper only in its uncommon case, which then
 * runs exactly as it always did. The common case decides nothing new: each
 * *_quiet test below is the helper's own fast path, taken only when it would
 * call nothing out of line, and writing nothing when it says no.
 *
 * Without PSX_GPR_LOCALS (or in overlay DLLs, cosim, or without block cycles)
 * every form expands to the original text: the same generated sources build
 * both ways. */
#ifndef PSX_GPR_LOCALS_H
#define PSX_GPR_LOCALS_H

#include <stdint.h>
#include <string.h>
#include "psx_runtime.h"
#include "psx_icache.h"
#include "psx_cyc.h"
#include "psx_memory.h"

#if !defined(PSX_GPR_LOCALS) && !defined(PSX_GPR_LOCALS_OFF)
#define PSX_GPR_LOCALS 1
#endif

#if defined(PSX_GPR_LOCALS) && PSX_GPR_LOCALS && !defined(PSX_GPR_LOCALS_OFF) && \
    !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM) && defined(PSX_ENABLE_BLOCK_CYCLES) && \
    (defined(__clang__) || defined(__GNUC__))
#define PSX_B2_ON 1
#endif

#ifdef PSX_B2_ON

#ifdef __cplusplus
extern "C" {
#endif

/* memory.c: the store's direct path (psx_store_word's first half) with no
 * call out of line. 1: stored. 0: not the direct case, nothing done; the
 * caller writes through cpu->write_*. 2: the store pointer is not the plain
 * one or an overlay's cycles are pending: the caller runs psx_store_*. */
int psx_store_word_quiet(CPUState* cpu, uint32_t addr, uint32_t val);
int psx_store_half_quiet(CPUState* cpu, uint32_t addr, uint16_t val);
int psx_store_byte_quiet(CPUState* cpu, uint32_t addr, uint8_t val);

/* psx_cyc_charge_edge's common case (psx_edge.h): 1 added to the batch, 0 nothing. */
static inline int psx_cyc_charge_quiet(uint32_t cycles) {
    const uint32_t prior = g_psx_cyc_batch, sum = prior + cycles;
    if (__builtin_expect((g_psx_cyc_gate == 0u) & (sum >= prior), 1)) {
        g_psx_cyc_batch = sum;
        return 1;
    }
    return 0;
}

/* psx_check_interrupts_at_inline with nothing called out of line. */
static inline int psx_edge_quiet(CPUState* cpu, uint32_t resume_pc) {
    return psx_cyc_batch_flush_quiet() && psx_edge_irq_fast(cpu, resume_pc);
}

/* psx_blk_load_*_edge's main-RAM and scratchpad paths (psx_edge.h). */
#define PSX_B2_LOAD_QUIET(T, name, size)                                          \
    static inline __attribute__((always_inline)) int name(uint32_t packed, uint32_t addr, T* out) { \
        if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {           \
            const uint32_t phys = addr & 0x1FFFFFFFu;                             \
            if (phys < 0x00800000u) {                                             \
                if (!psx_cyc_charge_quiet((packed & 3u) + 5u)) return 0;          \
                memcpy(out, g_psx_ram + (phys & g_psx_ram_mask), sizeof(T));      \
                return 1;                                                         \
            }                                                                     \
            if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - size) { \
                if (!psx_cyc_charge_quiet((packed >> 8) & 0xFFu)) return 0;       \
                memcpy(out, g_psx_scratch + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(T)); \
                return 1;                                                         \
            }                                                                     \
        }                                                                         \
        return 0;                                                                 \
    }
PSX_B2_LOAD_QUIET(uint32_t, psx_b2_load_word_quiet, 4u)
PSX_B2_LOAD_QUIET(uint16_t, psx_b2_load_half_quiet, 2u)
PSX_B2_LOAD_QUIET(uint8_t,  psx_b2_load_byte_quiet, 1u)
#undef PSX_B2_LOAD_QUIET

static inline __attribute__((always_inline)) int psx_b2_lwc2_quiet(uint32_t packed, uint32_t addr,
                                                                   uint32_t* out) {
    if (g_ls_mode == 0 && !g_ds_recording && addr < 0xC0000000u) {
        const uint32_t phys = addr & 0x1FFFFFFFu;
        if (phys < 0x00800000u) {
            if (!psx_cyc_charge_quiet((packed & 3u) + 4u)) return 0;
            memcpy(out, g_psx_ram + (phys & g_psx_ram_mask), sizeof(*out));
            return 1;
        }
        if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - 4u) {
            memcpy(out, g_psx_scratch + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(*out));
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#define PSX_R(n)        _psx_r##n
#define PSX_B2_DECL(n)  uint32_t _psx_r##n = cpu->gpr[n];
#define PSX_B2_ST(n)    cpu->gpr[n] = _psx_r##n;
#define PSX_B2_SX(n)    (cpu->gpr[n] = _psx_r##n),
#define PSX_B2_LD(n)    _psx_r##n = cpu->gpr[n];

#define PSX_B2_ICACHE(addr, OUT) do {                                            \
        const uint32_t _b2a = (addr);                                             \
        if (__builtin_expect(g_psx_icache_tv[(_b2a & 0xFFCu) >> 2] != _b2a, 0)) { \
            OUT psx_icache_fetch_miss(cpu, _b2a);                                 \
        }                                                                         \
    } while (0)
#define PSX_B2_CHARGE(n, OUT) do {                                                \
        const uint32_t _b2c = (n);                                                \
        if (!psx_cyc_charge_quiet(_b2c)) { OUT psx_cyc_charge_edge_slow(_b2c); }  \
    } while (0)
#define PSX_B2_EDGE(pc, OUT, IN) do {                                             \
        const uint32_t _b2p = (pc);                                               \
        if (__builtin_expect(!psx_edge_quiet(cpu, _b2p), 0)) {                    \
            OUT psx_check_interrupts_at_inline(cpu, _b2p); IN                     \
        }                                                                         \
    } while (0)
#define PSX_B2_SLICE(addr, n, se, OUT, IN) do {                                   \
        if (__builtin_expect(g_psx_precise_slice != 0, 0)) {                      \
            OUT if (psx_slice_block_impl(cpu, (addr), (n), (se))) return; IN      \
        }                                                                         \
    } while (0)
#define PSX_B2_LOADW(packed, addr, rt, mask, OUT) ({                              \
        const uint32_t _b2a = (addr); uint32_t _b2v;                              \
        if (__builtin_expect(!psx_b2_load_word_quiet((packed), _b2a, &_b2v), 0)) { \
            OUT _b2v = psx_blk_load_word_edge((packed), cpu, _b2a, (rt), (mask)); \
        }                                                                         \
        _b2v; })
#define PSX_B2_LOADH(packed, addr, rt, mask, OUT) ({                              \
        const uint32_t _b2a = (addr); uint16_t _b2v;                              \
        if (__builtin_expect(!psx_b2_load_half_quiet((packed), _b2a, &_b2v), 0)) { \
            OUT _b2v = psx_blk_load_half_edge((packed), cpu, _b2a, (rt), (mask)); \
        }                                                                         \
        _b2v; })
#define PSX_B2_LOADB(packed, addr, rt, mask, OUT) ({                              \
        const uint32_t _b2a = (addr); uint8_t _b2v;                               \
        if (__builtin_expect(!psx_b2_load_byte_quiet((packed), _b2a, &_b2v), 0)) { \
            OUT _b2v = psx_blk_load_byte_edge((packed), cpu, _b2a, (rt), (mask)); \
        }                                                                         \
        _b2v; })
#define PSX_B2_LWC2(packed, addr, OUT) ({                                         \
        const uint32_t _b2a = (addr); uint32_t _b2v;                              \
        if (__builtin_expect(!psx_b2_lwc2_quiet((packed), _b2a, &_b2v), 0)) {     \
            OUT _b2v = psx_blk_lwc2_read_edge((packed), cpu, _b2a);               \
        }                                                                         \
        _b2v; })
#define PSX_B2_STORE_(T, kind, addr, val, OUT) do {                               \
        const uint32_t _b2a = (addr); const T _b2v = (val);                       \
        const int _b2k = psx_store_##kind##_quiet(cpu, _b2a, _b2v);               \
        if (__builtin_expect(_b2k != 1, 0)) {                                     \
            OUT if (_b2k == 0) cpu->write_##kind(_b2a, _b2v);                     \
            else psx_store_##kind(cpu, _b2a, _b2v);                               \
        }                                                                         \
    } while (0)
#define PSX_B2_STOREW(addr, val, OUT) PSX_B2_STORE_(uint32_t, word, addr, val, OUT)
#define PSX_B2_STOREH(addr, val, OUT) PSX_B2_STORE_(uint16_t, half, addr, val, OUT)
#define PSX_B2_STOREB(addr, val, OUT) PSX_B2_STORE_(uint8_t,  byte, addr, val, OUT)

/* [recompiler] lean (docs/LEAN_CORE.md): the forms the pass writes as PSX_LN_*
 * in a lean body. They keep the block's cycles in the function's local
 * _psx_lc instead of the global batch, and take plain stores inline, so the
 * common path of a block calls nothing out of line. _psx_lc is handed to the
 * batch (PSX_LN_PUB) before anything that can see the clock: every slow path,
 * an I-cache miss, an edge, a slice, every call and return, and the block
 * flush. The batch then holds what the per-charge adds would have put there;
 * the charge gate (g_psx_cyc_gate) only changes inside such calls, so a
 * charge that finds it lowered is one the B2 form would have batched too.
 * PSX_LEAN_OFF builds the same generated text as the B2 forms. */
#if !defined(PSX_LEAN_OFF)
#define PSX_LN_ON 1

#ifdef __cplusplus
extern "C" {
#endif
void psx_write_word(uint32_t addr, uint32_t val);
void psx_write_half(uint32_t addr, uint16_t val);
void psx_write_byte(uint32_t addr, uint8_t val);
extern void (*g_overlay_flush_pending_cycles)(void);
extern uint32_t g_psx_mem_gate;
extern int g_psx_track_last_store;
extern uint32_t g_guest_last_store_addr, g_guest_last_store_val;
/* memory.c's RAM and scratchpad by name (g_psx_ram / g_psx_scratch point at
 * them): a store through these cannot touch any other global, so the compiler
 * keeps the mode words and the gate it has read across guest stores. */
extern uint8_t g_psx_ram_arr[];
extern uint8_t g_psx_scratch_arr[];
/* psx_cycles.c: the B2 forms' uncommon paths, one call each. */
uint32_t psx_ln_load_word_cold(uint32_t packed, CPUState* cpu, uint32_t addr);
uint16_t psx_ln_load_half_cold(uint32_t packed, CPUState* cpu, uint32_t addr);
uint8_t  psx_ln_load_byte_cold(uint32_t packed, CPUState* cpu, uint32_t addr);
uint32_t psx_ln_lwc2_cold(uint32_t packed, CPUState* cpu, uint32_t addr);
void psx_ln_store_word_cold(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_ln_store_half_cold(CPUState* cpu, uint32_t addr, uint16_t val);
void psx_ln_store_byte_cold(CPUState* cpu, uint32_t addr, uint8_t val);
void psx_ln_edge_cold(CPUState* cpu, uint32_t resume_pc);
#ifdef __cplusplus
}
#endif

/* The deferred cycles into the batch (psx_cyc_charge_quiet with the sum, its
 * slow path for the whole sum when the gate is up). */
static inline __attribute__((always_inline)) void psx_ln_publish(uint32_t n) {
    const uint32_t prior = g_psx_cyc_batch, sum = prior + n;
    if (__builtin_expect((g_psx_cyc_gate == 0u) & (sum >= prior), 1)) g_psx_cyc_batch = sum;
    else psx_cyc_charge_edge_slow(n);
}

/* memory.c psx_store_fast_ptr's answer without the call: main RAM on a page
 * the table marks plain (psx_memory.h g_psx_store_page), or the scratchpad;
 * NULL sends the store down the B2 form, which decides as before. */
static inline __attribute__((always_inline)) uint8_t* psx_ln_store_ptr(const CPUState* cpu,
                                                                       uint32_t addr, uint32_t size) {
    if (addr >= 0xC0000000u || (addr & (size - 1u))) return NULL;
    if (g_psx_mem_gate | (cpu->cop0[12] & 0x10000u)) return NULL;
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        const uint32_t off = phys & g_psx_ram_mask;
        const uint32_t kind = g_psx_store_page[off >> PSX_STORE_PAGE_SHIFT];
        if (kind == 1u ||
            (kind == 2u && ((g_psx_text_line_bitmap[off >> 11] >> ((off >> 6) & 31u)) & 1u)))
            return g_psx_ram_arr + off;
        return NULL;
    }
    const uint32_t so = phys - (uint32_t)PSX_SCRATCH_PHYS;
    if (so <= (uint32_t)PSX_SCRATCH_BYTES - size) return g_psx_scratch_arr + so;
    return NULL;
}

#define PSX_LN_LOAD_(T, name, size, c_ram, c_scr)                                 \
    static inline __attribute__((always_inline)) int name(uint32_t packed, uint32_t addr, T* out, \
                                                          uint32_t* lc) {         \
        if ((g_ls_mode | g_ds_recording | g_psx_cyc_gate) == 0 && addr < 0xC0000000u) { \
            const uint32_t phys = addr & 0x1FFFFFFFu;                             \
            if (phys < 0x00800000u) {                                             \
                *lc += (c_ram);                                                   \
                memcpy(out, g_psx_ram_arr + (phys & g_psx_ram_mask), sizeof(T));  \
                return 1;                                                         \
            }                                                                     \
            if (phys - (uint32_t)PSX_SCRATCH_PHYS <= (uint32_t)PSX_SCRATCH_BYTES - size) { \
                *lc += (c_scr);                                                   \
                memcpy(out, g_psx_scratch_arr + (phys - (uint32_t)PSX_SCRATCH_PHYS), sizeof(T)); \
                return 1;                                                         \
            }                                                                     \
        }                                                                         \
        return 0;                                                                 \
    }
PSX_LN_LOAD_(uint32_t, psx_ln_load_word, 4u, (packed & 3u) + 5u, (packed >> 8) & 0xFFu)
PSX_LN_LOAD_(uint16_t, psx_ln_load_half, 2u, (packed & 3u) + 5u, (packed >> 8) & 0xFFu)
PSX_LN_LOAD_(uint8_t,  psx_ln_load_byte, 1u, (packed & 3u) + 5u, (packed >> 8) & 0xFFu)
PSX_LN_LOAD_(uint32_t, psx_ln_lwc2,      4u, (packed & 3u) + 4u, 0u)
#undef PSX_LN_LOAD_

#define PSX_LN_DECL     uint32_t _psx_lc = 0u;
#define PSX_LN_PUB()    do { if (_psx_lc) { psx_ln_publish(_psx_lc); _psx_lc = 0u; } } while (0)
#define PSX_LN_PUBX     (_psx_lc ? (psx_ln_publish(_psx_lc), _psx_lc = 0u) : 0u),

#define PSX_LN_ICACHE(addr, OUT) do {                                            \
        const uint32_t _b2a = (addr);                                             \
        if (__builtin_expect(g_psx_icache_tv[(_b2a & 0xFFCu) >> 2] != _b2a, 0)) { \
            PSX_LN_PUB(); OUT psx_icache_fetch_miss(cpu, _b2a);                   \
        }                                                                         \
    } while (0)
#define PSX_LN_CHARGE(n, OUT) do {                                                \
        const uint32_t _b2c = (n);                                                \
        if (__builtin_expect(g_psx_cyc_gate == 0u, 1)) _psx_lc += _b2c;           \
        else { PSX_LN_PUB(); OUT psx_cyc_charge_edge_slow(_b2c); }                \
    } while (0)
#define PSX_LN_EDGE(pc, OUT, IN) do {                                             \
        const uint32_t _b2p = (pc);                                               \
        PSX_LN_PUB();                                                             \
        if (__builtin_expect(!psx_edge_quiet(cpu, _b2p), 0)) {                    \
            OUT psx_ln_edge_cold(cpu, _b2p); IN                                   \
        }                                                                         \
    } while (0)
#define PSX_LN_SLICE(addr, n, se, OUT, IN) do {                                   \
        if (__builtin_expect(g_psx_precise_slice != 0, 0)) {                      \
            PSX_LN_PUB(); OUT if (psx_slice_block_impl(cpu, (addr), (n), (se))) return; IN \
        }                                                                         \
    } while (0)
#define PSX_LN_LOAD_FORM(T, fast, slow, packed, addr, rt, mask, OUT) ({           \
        const uint32_t _b2a = (addr); T _b2v;                                     \
        if (__builtin_expect(!fast((packed), _b2a, &_b2v, &_psx_lc), 0)) {       \
            PSX_LN_PUB(); OUT _b2v = slow((packed), cpu, _b2a); (void)(rt); (void)(mask); \
        }                                                                         \
        _b2v; })
#define PSX_LN_LOADW(packed, addr, rt, mask, OUT) \
    PSX_LN_LOAD_FORM(uint32_t, psx_ln_load_word, psx_ln_load_word_cold, packed, addr, rt, mask, OUT)
#define PSX_LN_LOADH(packed, addr, rt, mask, OUT) \
    PSX_LN_LOAD_FORM(uint16_t, psx_ln_load_half, psx_ln_load_half_cold, packed, addr, rt, mask, OUT)
#define PSX_LN_LOADB(packed, addr, rt, mask, OUT) \
    PSX_LN_LOAD_FORM(uint8_t, psx_ln_load_byte, psx_ln_load_byte_cold, packed, addr, rt, mask, OUT)
#define PSX_LN_LWC2(packed, addr, OUT) ({                                         \
        const uint32_t _b2a = (addr); uint32_t _b2v;                              \
        if (__builtin_expect(!psx_ln_lwc2((packed), _b2a, &_b2v, &_psx_lc), 0)) { \
            PSX_LN_PUB(); OUT _b2v = psx_ln_lwc2_cold((packed), cpu, _b2a);      \
        }                                                                         \
        _b2v; })
/* A plain store: memory.c psx_store_try_* (count, last-store tracker, bytes). */
#define PSX_LN_STORE_(T, kind, size, track, addr, val, OUT) do {                  \
        const uint32_t _b2a = (addr); const T _b2v = (val);                       \
        uint8_t* _b2p = (cpu->write_##kind == psx_write_##kind && !g_overlay_flush_pending_cycles) \
                            ? psx_ln_store_ptr(cpu, _b2a, (size)) : NULL;         \
        if (__builtin_expect(_b2p != NULL, 1)) {                                  \
            g_guest_store_count++;                                                \
            track                                                                 \
            memcpy(_b2p, &_b2v, (size));                                          \
        } else {                                                                  \
            PSX_LN_PUB(); OUT psx_ln_store_##kind##_cold(cpu, _b2a, _b2v);        \
        }                                                                         \
    } while (0)
#define PSX_LN_TRACK_W                                                            \
    if (g_psx_track_last_store) { g_guest_last_store_addr = _b2a; g_guest_last_store_val = _b2v; }
#define PSX_LN_TRACK_HB if (g_psx_track_last_store) g_guest_last_store_addr = 0xFFFFFFFFu;
#define PSX_LN_STOREW(addr, val, OUT) PSX_LN_STORE_(uint32_t, word, 4u, PSX_LN_TRACK_W, addr, val, OUT)
#define PSX_LN_STOREH(addr, val, OUT) PSX_LN_STORE_(uint16_t, half, 2u, PSX_LN_TRACK_HB, addr, val, OUT)
#define PSX_LN_STOREB(addr, val, OUT) PSX_LN_STORE_(uint8_t,  byte, 1u, PSX_LN_TRACK_HB, addr, val, OUT)
#endif  /* !PSX_LEAN_OFF */

#else  /* off: the original text */

#define PSX_R(n)        cpu->gpr[n]
#define PSX_B2_DECL(n)
#define PSX_B2_ST(n)
#define PSX_B2_SX(n)
#define PSX_B2_LD(n)
#define PSX_B2_ICACHE(addr, OUT)                  psx_icache_fetch(cpu, (addr))
#define PSX_B2_CHARGE(n, OUT)                     psx_cyc_charge(n)
#define PSX_B2_EDGE(pc, OUT, IN)                  psx_check_interrupts_at(cpu, (pc))
#define PSX_B2_SLICE(addr, n, se, OUT, IN) \
    do { if (psx_slice_block(cpu, (addr), (n), (se))) return; } while (0)
#define PSX_B2_LOADW(packed, addr, rt, mask, OUT) psx_blk_load_word((packed), cpu, (addr), (rt), (mask))
#define PSX_B2_LOADH(packed, addr, rt, mask, OUT) psx_blk_load_half((packed), cpu, (addr), (rt), (mask))
#define PSX_B2_LOADB(packed, addr, rt, mask, OUT) psx_blk_load_byte((packed), cpu, (addr), (rt), (mask))
#define PSX_B2_LWC2(packed, addr, OUT)            psx_blk_lwc2_read((packed), cpu, (addr))
#define PSX_B2_STOREW(addr, val, OUT)             psx_store_word(cpu, (addr), (val))
#define PSX_B2_STOREH(addr, val, OUT)             psx_store_half(cpu, (addr), (val))
#define PSX_B2_STOREB(addr, val, OUT)             psx_store_byte(cpu, (addr), (val))

#endif

/* Lean off (or B2 off): the PSX_LN_* forms are the B2 ones. */
#ifndef PSX_LN_ON
#define PSX_LN_DECL
#define PSX_LN_PUB()                               do { } while (0)
#define PSX_LN_PUBX
#define PSX_LN_ICACHE(addr, OUT)                   PSX_B2_ICACHE(addr, OUT)
#define PSX_LN_CHARGE(n, OUT)                      PSX_B2_CHARGE(n, OUT)
#define PSX_LN_EDGE(pc, OUT, IN)                   PSX_B2_EDGE(pc, OUT, IN)
#define PSX_LN_SLICE(addr, n, se, OUT, IN)         PSX_B2_SLICE(addr, n, se, OUT, IN)
#define PSX_LN_LOADW(packed, addr, rt, mask, OUT)  PSX_B2_LOADW(packed, addr, rt, mask, OUT)
#define PSX_LN_LOADH(packed, addr, rt, mask, OUT)  PSX_B2_LOADH(packed, addr, rt, mask, OUT)
#define PSX_LN_LOADB(packed, addr, rt, mask, OUT)  PSX_B2_LOADB(packed, addr, rt, mask, OUT)
#define PSX_LN_LWC2(packed, addr, OUT)             PSX_B2_LWC2(packed, addr, OUT)
#define PSX_LN_STOREW(addr, val, OUT)              PSX_B2_STOREW(addr, val, OUT)
#define PSX_LN_STOREH(addr, val, OUT)              PSX_B2_STOREH(addr, val, OUT)
#define PSX_LN_STOREB(addr, val, OUT)              PSX_B2_STOREB(addr, val, OUT)
#endif
#endif
