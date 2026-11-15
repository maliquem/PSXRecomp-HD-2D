/* White-box coverage of savestate_poll's refused-load rollback (EST-16):
 * a load refused after apply restores the pre-load machine AND resyncs the
 * host clock/IRQ anchors; the fast-poll flag always mirrors "save or load still staged". */
#include <assert.h>
#include "../src/savestate.c"

void psx_frontend_on_savestate_refused(int load, int slot, const char* reason) {
    (void)load; (void)slot; (void)reason;
}
int psx_netplay_active(void) { return 0; }
int psx_netplay_is_host(void) { return 1; }
int psx_hle_scheduler_enabled(void) { return 1; }
int g_psx_guest_time_frozen;
int psx_mod_function_entry_active(void) { return 0; }
unsigned psx_snapshot_host_call_depth(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) { (void)pc; return 1; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }

static int irq_resyncs, cycle_resyncs, applies, notifies;
static int stage_save_on_notify;
void interrupts_resync_after_restore(void) { ++irq_resyncs; }
void psx_cycles_resync_after_restore(struct CPUState* cpu) { (void)cpu; ++cycle_resyncs; }
void cdrom_accelerate_after_savestate(void) { assert(0); }
void psx_scheduler_resume_at(uint32_t pc) { (void)pc; assert(0); }
void psx_frontend_on_savestate_loaded(void) { assert(0); }
void psx_frontend_on_savestate_notify(int load, int slot, int ok) {
    (void)slot;
    assert(load && !ok);
    ++notifies;
    if (stage_save_on_notify) assert(savestate_request_save_protocol(4));
}
void gpu_get_display_info(GpuDisplayInfo* out) { (void)out; assert(0); }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* out, uint32_t x, uint32_t y) {
    (void)out; (void)x; (void)y; assert(0); return 0;
}
int boot_state_save(const CPUState* cpu, uint32_t bios, uint32_t entry, const char* path) {
    (void)cpu; (void)bios; (void)entry; (void)path; assert(0); return 0;
}
int boot_state_load(const char* path, uint32_t bios, uint32_t entry, CPUState* cpu) {
    (void)path; (void)bios; (void)entry; (void)cpu; assert(0); return 0;
}
/* The snapshot is the byte 'R'; the refused blob is 'B' (restores pc 0). */
int boot_state_load_buffer(const uint8_t* data, size_t size, uint32_t bios,
                            uint32_t entry, CPUState* cpu) {
    (void)size; (void)bios; (void)entry;
    ++applies;
    cpu->pc = data[0] == 'R' ? 0x80010000u : 0u;
    return 1;
}
int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios, uint32_t entry,
                                uint8_t** data, size_t* size) {
    (void)cpu; (void)bios; (void)entry;
    *data = (uint8_t*)malloc(1);
    (*data)[0] = 'R';
    *size = 1;
    return 1;
}
int boot_state_check_buffer(const uint8_t* data, size_t size, uint32_t bios,
                             uint32_t entry, char* reason, size_t cap) {
    (void)data; (void)size; (void)bios; (void)entry; (void)reason; (void)cap;
    assert(0); return 0;
}

static void refused_load(CPUState* cpu, int stage_save) {
    const uint8_t blob = 'B';
    irq_resyncs = cycle_resyncs = applies = notifies = 0;
    stage_save_on_notify = stage_save;
    cpu->pc = 0x80010000u;
    assert(savestate_request_load_blob_protocol(&blob, 1));
    savestate_poll(cpu, 0x80010000u);
    assert(notifies == 1 && savestate_take_load_failed());
    assert(cpu->pc == 0x80010000u);   /* pre-load machine kept */
    assert(!s_load_blob);
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    s_configured = 1;

    /* Applied, then refused (pc 0): rollback re-applies and resyncs. */
    refused_load(&cpu, 0);
    assert(applies == 2);
    assert(cycle_resyncs == 1 && irq_resyncs == 1);
    assert(!g_psx_savestate_pending && !savestate_pending());

    /* A save staged from the failure callback keeps the fast-poll flag. */
    refused_load(&cpu, 1);
    assert(s_save_pending == 4 && savestate_pending());
    assert(g_psx_savestate_pending);
    savestate_set_title_blocked(1);
    assert(!g_psx_savestate_pending);

    puts("savestate poll rollback tests passed");
    return 0;
}
