/* EST-20: the section checks in boot_state.c itself (timer mode, section
 * lengths) refuse a state without touching the machine, although CPU, the
 * first section, would already be applied by a single pass; plus a
 * save -> load -> save round trip that must reproduce the bytes. Device
 * validators are doubled; each device's own invalid-state test pins its
 * validator. Same stubs as test_boot_state_handoff.c, with the timer registers
 * held in the test. */
#include "boot_state.c"
#include "fntrace.c"
#include <assert.h>

static uint8_t ram[2*1024*1024], spad[1024], spuram[512*1024];
static uint16_t vram[1024*512];
uint32_t i_stat, i_mask, g_psx_icache_tv[1024];
uint64_t psx_cycle_count;
uint32_t g_psx_gcs_frac;
uint32_t psx_guest_cycle_scale_config(void) { return 1; }
void psx_guest_cycle_scale_snapshot(uint32_t out[3]) { memset(out,0,12); }
void psx_guest_cycle_scale_restore(const uint32_t in[3]) { (void)in; }
uint8_t* memory_get_ram_ptr(void) { return ram; }
uint32_t memory_get_ram_bytes(void) { return sizeof ram; }
uint8_t* memory_get_scratchpad_ptr(void) { return spad; }
int psx_ram_8mb_active(void) { return 0; }
void memory_clear_low_boot_scratch(void) {}
void dirty_ram_clear_image_baseline(void) {}
void cdrom_notify_game_started(void) {}
uint32_t dirty_ram_get_bitmap_word_count(void) { return 16; }
uint32_t dirty_ram_get_bitmap_word(uint32_t i) { (void)i; return 0; }
void dirty_ram_set_bitmap_words(const uint32_t* p,uint32_t n) { (void)p; (void)n; }
void overlay_watch_invalidate_after_ram_restore(void) {}
void psx_kernel_bless_note_range(uint32_t p,uint32_t n) { (void)p; (void)n; }
void gte_canonicalize_cpu_state(CPUState* cpu) { cpu->gpr[0]=0; }
static uint32_t csv_live = 42;
uint32_t interrupts_get_cycles_since_vblank(void) { return csv_live; }
void interrupts_set_cycles_since_vblank(uint32_t n) { csv_live = n; }
static uint16_t t_counter[3], t_target[3];
static uint32_t t_mode[3], t_frac[3];
static int32_t t_irq[3];
void timers_get_snapshot(uint16_t c[3],uint32_t m[3],uint16_t t[3],int32_t irq[3],uint32_t f[3]) {
    memcpy(c,t_counter,6); memcpy(m,t_mode,12); memcpy(t,t_target,6); memcpy(irq,t_irq,12); memcpy(f,t_frac,12);
}
void timers_set_snapshot(const uint16_t c[3],const uint32_t m[3],const uint16_t t[3],const int32_t irq[3],const uint32_t f[3]) {
    memcpy(t_counter,c,6); memcpy(t_mode,m,12); memcpy(t_target,t,6); memcpy(t_irq,irq,12); memcpy(t_frac,f,12);
}
/* Mirrors timers.c (bits 0-12 plus the one-shot-spent bit); test_timers_oneshot pins the real one. */
int timers_snapshot_mode_ok(uint32_t mode) { return (mode & ~(0x1FFFu | 0x80000000u)) == 0; }
#define MODULE(name) uint32_t name##_snapshot_bytes(void){return 4;} \
    void name##_snapshot_write(uint8_t* p){memset(p,0,4);} \
    int name##_snapshot_read(const uint8_t* p,uint32_t n){(void)p;return n==4;}
MODULE(gpu) MODULE(spu) MODULE(cdrom) MODULE(dma) MODULE(sio) MODULE(mdec) MODULE(spu_cd) MODULE(memory_ctrl)
#define VALIDATE(name) int name##_snapshot_validate(const uint8_t* p,uint32_t n){(void)p;return n==4;}
VALIDATE(gpu) VALIDATE(spu) VALIDATE(cdrom) VALIDATE(dma) VALIDATE(sio) VALIDATE(mdec) VALIDATE(spu_cd) VALIDATE(memory_ctrl)
int cdrom_snapshot_disc_matches(const uint8_t* p,uint32_t n){(void)p;(void)n;return 1;}
void spu_catch_up(void){}
void psx_devices_service_to_now(void){}
uint8_t* spu_get_ram_ptr(void){return spuram;}
uint32_t spu_get_ram_bytes(void){return sizeof spuram;}
uint32_t psx_mod_memory_snapshot_bytes(void){return 0;}
uint32_t psx_mod_memory_layout_cookie(void){return 0;}
void psx_mod_memory_snapshot_write(uint8_t* p){(void)p;}
int psx_mod_memory_snapshot_read(const uint8_t* p,uint32_t n){(void)p;return n==0;}
int psx_mod_memory_snapshot_validate(const uint8_t* p,uint32_t n){(void)p;return n==0;}
int gpu_vram_dirty_tracking(void){return 0;}
uint32_t gpu_vram_dirty_row_count(void){return 0;}
const uint64_t* gpu_vram_dirty_mask(void){static const uint64_t mask[8]={0};return mask;}
void gpu_vram_dirty_clear(void){}
void gr_vram_transfer_in(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;memcpy(vram,p,(size_t)w*h*2);}
void gr_vram_transfer_out(int x,int y,int w,int h,uint16_t* p){(void)x;(void)y;memcpy(p,vram,(size_t)w*h*2);}
int g_spu_cd_restored;

#define BIOS 123u
#define ENTRY 0x801b0000u

/* Everything the apply pass writes that this test can see. */
typedef struct { uint32_t pc, r4, i_stat, i_mask, csv, mode[3]; uint8_t ram0; uint64_t cyc; int started; } Seen;
static Seen seen_now(const CPUState* c) {
    Seen s;
    memset(&s, 0, sizeof s);
    s.pc = c->pc; s.r4 = c->gpr[4]; s.i_stat = i_stat; s.i_mask = i_mask; s.csv = csv_live;
    for (int i = 0; i < 3; i++) s.mode[i] = t_mode[i];
    s.ram0 = ram[0x100]; s.cyc = psx_cycle_count; s.started = fntrace_is_game_started();
    return s;
}
static void set_live(CPUState* c, uint32_t salt) {
    c->pc = 0x80050000u + salt; c->gpr[4] = 0x1000u + salt;
    i_stat = 0x11u + salt; i_mask = 0x22u + salt; csv_live = 7u + salt;
    for (int i = 0; i < 3; i++) {
        t_counter[i] = (uint16_t)(i + salt); t_mode[i] = 0x1FFu + salt;
        t_target[i] = (uint16_t)(9 + i); t_irq[i] = (int32_t)i; t_frac[i] = salt;
    }
    ram[0x100] = (uint8_t)(0xA0u + salt); psx_cycle_count = 1000u + salt;
    fntrace_restore_game_started((int)(salt & 1u));
}
/* Payload offset of a section in a raw file; the header holds the count at +28. */
static size_t find_section(const uint8_t* f, uint32_t tag, uint64_t* len_out) {
    PstR r; uint32_t count = 0;
    pst_r_init(&r, f + 28, 4); assert(pst_r_u32(&r, &count));
    size_t cur = BOOT_STATE_HEADER_WIRE_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t t = 0, pad = 0; uint64_t len = 0;
        pst_r_init(&r, f + cur, 16);
        assert(pst_r_u32(&r, &t) && pst_r_u32(&r, &pad) && pst_r_u64(&r, &len));
        if (t == tag) { if (len_out) *len_out = len; return cur + 16; }
        cur += 16 + (size_t)len;
    }
    assert(!"section not found"); return 0;
}
/* Copy of the file with one section's payload longer by `extra` zero bytes. */
static uint8_t* grow_section(const uint8_t* f, size_t len, uint32_t tag, uint32_t extra, size_t* out_len) {
    uint64_t sl; size_t p = find_section(f, tag, &sl), end = p + (size_t)sl;
    uint8_t* g = malloc(len + extra); assert(g);
    memcpy(g, f, end); memset(g + end, 0, extra); memcpy(g + end + extra, f + end, len - end);
    PstW w; pst_w_init(&w, g + p - 8, 8); assert(pst_w_u64(&w, sl + extra));
    *out_len = len + extra; return g;
}
static void assert_refused(const uint8_t* f, size_t len, CPUState* cpu, const char* what) {
    set_live(cpu, 5);
    Seen before = seen_now(cpu);
    if (boot_state_load_buffer(f, len, BIOS, ENTRY, cpu)) { fprintf(stderr, "accepted: %s\n", what); abort(); }
    Seen after = seen_now(cpu);
    if (memcmp(&before, &after, sizeof before)) { fprintf(stderr, "machine changed by refused load: %s\n", what); abort(); }
}

int main(void) {
    CPUState cpu = {0};
    fntrace_set_game_range(0x801b0000, 0);
    set_live(&cpu, 1);
    uint8_t *good = NULL, *again = NULL; size_t glen = 0, alen = 0;
    assert(boot_state_save_buffer_raw(&cpu, BIOS, ENTRY, &good, &glen));

    /* Round trip: load into a different machine, save again, same bytes. */
    set_live(&cpu, 4);
    assert(boot_state_load_buffer(good, glen, BIOS, ENTRY, &cpu));
    assert(cpu.pc == 0x80050001u && t_mode[2] == 0x1FFu + 1u && i_mask == 0x23u && csv_live == 8u);
    assert(boot_state_save_buffer_raw(&cpu, BIOS, ENTRY, &again, &alen));
    assert(alen == glen && !memcmp(good, again, glen));
    free(again);

    /* The timer section follows CPU in file order, so a bad timer mode is
     * found after a single-pass loader would already have applied CPU. */
    uint64_t tl; size_t tp = find_section(good, BS_SEC_TIMER, &tl);
    assert(find_section(good, BS_SEC_CPU, NULL) < tp && tl == TIMER_REGS_WIRE_BYTES);
    static const struct { const char* name; uint32_t timer; uint32_t mode; } bad_mode[] = {
        { "timer 2 mode bit 13", 2, 0x2000u }, { "timer 0 mode bit 30", 0, 0x40000000u },
        { "timer 1 mode bit 14", 1, 0x4000u },
    };
    for (unsigned k = 0; k < sizeof bad_mode / sizeof bad_mode[0]; k++) {
        uint8_t* f = malloc(glen); assert(f);
        memcpy(f, good, glen);
        PstW w; pst_w_init(&w, f + tp + 6 + 4 * bad_mode[k].timer, 4);
        assert(pst_w_u32(&w, bad_mode[k].mode));
        assert_refused(f, glen, &cpu, bad_mode[k].name);
        free(f);
    }
    /* Section lengths the apply pass would not accept. */
    static const struct { const char* name; uint32_t tag; uint32_t extra; } grow[] = {
        { "CPU section one word long", BS_SEC_CPU, 4 }, { "IRQ section 16 bytes", BS_SEC_IRQ, 4 },
        { "timer section one word long", BS_SEC_TIMER, 4 }, { "clock section 12 bytes", BS_SEC_CLOCK, 4 },
        { "dirty bitmap longer than live RAM", BS_SEC_DIRTY, 4 },
    };
    for (unsigned k = 0; k < sizeof grow / sizeof grow[0]; k++) {
        size_t flen; uint8_t* f = grow_section(good, glen, grow[k].tag, grow[k].extra, &flen);
        assert_refused(f, flen, &cpu, grow[k].name);
        free(f);
    }
    /* The refusals leave the loader usable. */
    set_live(&cpu, 4);
    assert(boot_state_load_buffer(good, glen, BIOS, ENTRY, &cpu) && cpu.pc == 0x80050001u);
    free(good);
    puts("boot state load: round trip and atomic refusal of bad sections passed");
    return 0;
}
