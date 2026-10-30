/* psx_mem_gate.h — the stores' global gate (memory.c psx_store_fast_ptr).
 *
 * A guest store takes the short path (straight to RAM) only while none of these
 * is set: render pass, lockstep, data-shard recording, DMA, host store, parity
 * trace, card-data capture, debug write trace. Instead of testing the eight
 * per store, the short path tests g_psx_mem_gate: every writer that SETS one of
 * them raises the gate (PSX_MEM_GATE_RAISE); the short path, finding it raised,
 * looks at the eight and lowers it again once all are clear. Writers that only
 * clear need nothing. TCP core_rates counts mem_gate_stale: the device service
 * found the gate lowered while one of the eight was set (a writer that forgot
 * the raise). */
#ifndef PSX_MEM_GATE_H
#define PSX_MEM_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_mem_gate;
#define PSX_MEM_GATE_RAISE() ((void)(g_psx_mem_gate = 1u))

/* Device service: count a lowered gate with a flag set, and raise it
 * (psx_cycles.c; memory.c registers the flags' reader at memory_init). */
extern uint32_t (*g_psx_mem_gate_value)(void);
void psx_mem_gate_check(void);

#ifdef __cplusplus
}
#endif

#endif
