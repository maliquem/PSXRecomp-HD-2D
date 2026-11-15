/* White-box coverage of psx_rewind_poll / do_load (EST-4): a staged rewind
 * waits out a mod callback, a host continuation and frozen guest time (the
 * same gates as savestate_poll), and a snap refused after apply (resume PC
 * not dispatchable) puts the pre-load machine back, resyncs the host
 * clock/IRQ anchors and keeps the history. Ring ops are stubbed here, so only
 * the rbengine header is needed. */
#ifndef PSX_HAS_RBENGINE_SNAP
#define PSX_HAS_RBENGINE_SNAP 1
#endif
#include <assert.h>
#include "../src/psx_rewind.c"

int g_psx_guest_time_frozen;
static int mod_active;
static unsigned host_depth;
int psx_mod_function_entry_active(void) { return mod_active; }
unsigned psx_snapshot_host_call_depth(void) { return host_depth; }

int psx_hle_scheduler_enabled(void) { return 1; }
int psx_netplay_active(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) { (void)pc; return 1; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }
int gpu_display_is_depth24(void) { return 0; }
int mdec_recently_active(uint32_t f) { (void)f; return 0; }
int cdrom_xa_stream_active(void) { return 0; }
void gpu_get_display_info(GpuDisplayInfo *out) { (void)out; assert(0); }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo *di, uint32_t x, uint32_t y) {
    (void)di; (void)x; (void)y; assert(0); return 0;
}
const char *host_keymap_label(HostKeymapAction a, char *out, size_t cap) {
    (void)a; (void)cap; out[0] = '\0'; return out;
}
void host_osd_push(const char *msg, int ms) { (void)msg; (void)ms; }

static int irq_resyncs, cycle_resyncs, applies, drops, resumed, accel, loaded;
static uint32_t resumed_pc;
void interrupts_resync_after_restore(void) { ++irq_resyncs; }
void psx_cycles_resync_after_restore(struct CPUState *cpu) { (void)cpu; ++cycle_resyncs; }
void cdrom_accelerate_after_savestate(void) { ++accel; }
void psx_frontend_on_savestate_loaded(void) { ++loaded; }
void psx_scheduler_resume_at(uint32_t pc) { ++resumed; resumed_pc = pc; }

/* Pre-load machine is 'R' (pc 0x80010000); a good snap 'G' resumes at
 * 0x80020000; a bad snap 'B' restores pc 0 (not dispatchable). */
static uint8_t snap_byte;
static int rollback_ok = 1;
int boot_state_save_buffer_raw(const CPUState *cpu, uint32_t bios, uint32_t entry,
                               uint8_t **data, size_t *size) {
    (void)cpu; (void)bios; (void)entry;
    if (!rollback_ok) return 0;
    *data = (uint8_t *)malloc(1);
    (*data)[0] = 'R';
    *size = 1;
    return 1;
}
int boot_state_load_buffer(const uint8_t *data, size_t size, uint32_t bios,
                           uint32_t entry, CPUState *cpu) {
    (void)size; (void)bios; (void)entry;
    ++applies;
    cpu->pc = data[0] == 'R' ? 0x80010000u : data[0] == 'G' ? 0x80020000u : 0u;
    return 1;
}
RbeSnapRing *rbe_snap_ring_create(uint32_t depth) { (void)depth; assert(0); return NULL; }
void rbe_snap_ring_destroy(RbeSnapRing *r) { (void)r; }
void rbe_snap_ring_clear(RbeSnapRing *r) { (void)r; }
int rbe_snap_ring_store(RbeSnapRing *r, uint32_t t, uint8_t *d, size_t n) {
    (void)r; (void)t; (void)n; free(d); return 1;
}
const uint8_t *rbe_snap_ring_peek(const RbeSnapRing *r, uint32_t t, size_t *n) {
    (void)r; (void)t; *n = 1; return &snap_byte;
}
uint32_t rbe_snap_ring_drop_after(RbeSnapRing *r, uint32_t t) { (void)r; (void)t; ++drops; return 0; }

static void stage(CPUState *cpu, uint8_t snap) {
    irq_resyncs = cycle_resyncs = applies = drops = resumed = accel = loaded = 0;
    snap_byte = snap;
    cpu->pc = 0x80010000u;
    s_frame = 40;
    s_load_tick = 30;
    s_load_pending = 1;
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    s_enabled = 1;
    s_configured = 1;
    s_ring = (RbeSnapRing *)&snap_byte; /* opaque; every ring op is stubbed */

    /* Each continuation gate keeps the load staged and the machine untouched. */
    stage(&cpu, 'G');
    mod_active = 1;
    psx_rewind_poll(&cpu, 0x80010000u);
    mod_active = 0;
    host_depth = 1;
    psx_rewind_poll(&cpu, 0x80010000u);
    host_depth = 0;
    g_psx_guest_time_frozen = 1;
    psx_rewind_poll(&cpu, 0x80010000u);
    g_psx_guest_time_frozen = 0;
    assert(s_load_pending && applies == 0 && cpu.pc == 0x80010000u);

    /* Gates clear: the staged load runs and resumes at the snap's PC. */
    psx_rewind_poll(&cpu, 0x80010000u);
    assert(!s_load_pending && applies == 1 && drops == 1);
    assert(resumed == 1 && resumed_pc == 0x80020000u && s_frame == 30);
    assert(cycle_resyncs == 1 && irq_resyncs == 1 && accel == 1 && loaded == 1);

    /* Applied, then refused (pc 0): rollback re-applies, resyncs, keeps history. */
    stage(&cpu, 'B');
    psx_rewind_poll(&cpu, 0x80010000u);
    assert(!s_load_pending && applies == 2 && cpu.pc == 0x80010000u);
    assert(cycle_resyncs == 1 && irq_resyncs == 1);
    assert(drops == 0 && resumed == 0 && accel == 0 && loaded == 0 && s_frame == 40);

    /* No rollback snapshot: refuse before touching the machine. */
    stage(&cpu, 'G');
    rollback_ok = 0;
    psx_rewind_poll(&cpu, 0x80010000u);
    assert(!s_load_pending && applies == 0 && cpu.pc == 0x80010000u);
    assert(drops == 0 && resumed == 0);

    puts("rewind load gate tests passed");
    return 0;
}
