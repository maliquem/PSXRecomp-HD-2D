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
/* Thumbnail source: a 128x96 display whose pixel encodes (tag, y, x). */
static uint32_t px_tag;
void gpu_get_display_info(GpuDisplayInfo *out) {
    memset(out, 0, sizeof(*out));
    out->width = RW_THUMB_W;
    out->height = RW_THUMB_H;
}
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo *di, uint32_t x, uint32_t y) {
    (void)di;
    return (px_tag << 16) | (y << 8) | x;
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

    /* Capture renders the thumbnail straight into its list slot (EST-O1):
     * contents follow the tick through eviction, a repeated tick reuses its
     * slot, and every pool slot stays reachable. */
    {
        RwThumb pool[2];
        RwThumb *ptrs[2] = { &pool[0], &pool[1] };
        uint32_t ticks[2];
        RwThumb *first;
        memset(pool, 0, sizeof(pool));
        s_thumb_pool = pool;
        s_thumbs = ptrs;
        s_ticks = ticks;
        s_depth = s_list_cap = 2;
        s_count = 0;
        s_sel = 0;

        for (uint32_t t = 10; t <= 11; t++) {
            s_frame = t;
            px_tag = t;
            assert(do_capture(&cpu, 0x80010000u) == 1);
        }
        assert(s_count == 2 && s_ticks[0] == 10 && s_ticks[1] == 11 && s_sel == 1);
        first = s_thumbs[0];
        assert(first->px[3 * RW_THUMB_W + 5] == (0xFF000000u | (10u << 16) | (3u << 8) | 5u));

        s_frame = 11;
        px_tag = 99;
        assert(do_capture(&cpu, 0x80010000u) == 1);
        assert(s_count == 2 && s_thumbs[0] == first && s_thumbs[1]->px[0] == (0xFF000000u | (99u << 16)));

        s_frame = 12; /* full: oldest (10) evicted, its slot is reused for 12 */
        px_tag = 12;
        assert(do_capture(&cpu, 0x80010000u) == 1);
        assert(s_count == 2 && s_ticks[0] == 11 && s_ticks[1] == 12);
        assert(s_thumbs[1] == first && s_thumbs[0] != s_thumbs[1]);
        assert(s_thumbs[0]->px[0] == (0xFF000000u | (99u << 16)));
        assert(s_thumbs[1]->px[7 * RW_THUMB_W + 1] == (0xFF000000u | (12u << 16) | (7u << 8) | 1u));
        s_thumb_pool = NULL;
        s_thumbs = NULL;
        s_ticks = NULL;
    }

    puts("rewind load gate tests passed");
    return 0;
}
