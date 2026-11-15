/* psx_mod_host_write_ram / psx_mod_spu_upload / psx_mod_psyq_load_image span
 * checks (MEM-21): addresses outside KUSEG/KSEG0/KSEG1 must be refused, not
 * folded into RAM by the 29-bit mask. Includes the .c with stubs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/mod_guest_services.c"

uint32_t g_psx_ram_size = 0x200000u;
CPUState *debug_cpu_ptr;
static uint8_t s_ram[0x200000];
static int s_spu_words;

int psx_mod_host_output_scope(void) { return 0; }
uint32_t psx_mod_read_word(uint32_t a) { (void)a; return 0; }
void psx_dispatch_call(CPUState *c, uint32_t t, uint32_t r) { (void)c; (void)t; (void)r; }
void psx_snapshot_host_call_begin(void) {}
void psx_snapshot_host_call_end(void) {}
int psx_cycle_uncharged_begin(PsxCycleFreeze *s, uint64_t b) { (void)s; (void)b; return 0; }
int psx_cycle_uncharged_end(const PsxCycleFreeze *s) { (void)s; return 1; }
uint64_t psx_cycle_uncharged_counted(void) { return 0; }
int dma_host_cdrom_write(uint32_t d, const uint8_t *s, uint32_t b, int l) { (void)d; (void)s; (void)b; (void)l; return 0; }
void psx_host_write_byte(uint32_t a, uint8_t v) { s_ram[a & 0x1FFFFFu] = v; }
void psx_host_write_word(uint32_t a, uint32_t v) { memcpy(&s_ram[a & 0x1FFFFFu], &v, 4); }
int spu_register_sample_bank(uint32_t b, const void *p, uint32_t n) { (void)b; (void)p; (void)n; return 0; }
int spu_bind_next_voice_bank(unsigned v, uint32_t b) { (void)v; (void)b; return 0; }
int spu_private_voice_play(unsigned s, uint32_t b, const uint16_t r[8], unsigned m) { (void)s; (void)b; (void)r; (void)m; return 0; }
void spu_private_voice_stop(uint32_t m) { (void)m; }
int spu_private_voice_volume(unsigned s, uint16_t l, uint16_t r) { (void)s; (void)l; (void)r; return 0; }
uint32_t spu_private_voice_active(void) { return 0; }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
uint16_t spu_ctrl_read(void) { return 0; }
void spu_dma_write(uint32_t w) { (void)w; s_spu_words++; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
void gpu_write_gp1(uint32_t v) { (void)v; }
void gpu_write_gp0(uint32_t v) { (void)v; }
void gpu_set_overdraw(int x, int y) { (void)x; (void)y; }
void gpu_set_overdraw_share(int o) { (void)o; }
int gpu_set_overdraw_extra(int x, int y) { (void)x; (void)y; return 0; }

int main(void) {
    uint32_t d[4] = {1, 2, 3, 4};
    /* valid mirrors of RAM */
    assert(psx_mod_host_write_ram(0x00001000u, d, 16));
    assert(psx_mod_host_write_ram(0x80001000u, d, 16));
    assert(psx_mod_host_write_ram(0xA0001000u, d, 16));
    assert(psx_mod_host_write_ram(0x80000000u, d, 0));
    assert(psx_mod_host_write_ram(0x801FFFF0u, d, 16));
    /* the aliases the 29-bit mask used to let through */
    assert(!psx_mod_host_write_ram(0x20001000u, d, 16));
    assert(!psx_mod_host_write_ram(0x40001000u, d, 16));
    assert(!psx_mod_host_write_ram(0x60001000u, d, 16));
    assert(!psx_mod_host_write_ram(0xC0001000u, d, 16));
    /* MMIO, BIOS, scratchpad, span past the end, sum overflow */
    assert(!psx_mod_host_write_ram(0xBF801000u, d, 4));
    assert(!psx_mod_host_write_ram(0x1F801000u, d, 4));
    assert(!psx_mod_host_write_ram(0xBFC00000u, d, 4));
    assert(!psx_mod_host_write_ram(0x801FFFF8u, d, 16));
    assert(!psx_mod_host_write_ram(0x80001000u, d, 0xFFFFFFFFu));
    assert(!psx_mod_host_write_ram(0x80001000u, NULL, 4));
    assert(!psx_mod_spu_upload(0, 0x20001000u, 16, 0));
    assert(psx_mod_spu_upload(0, 0x80001000u, 16, 0) && s_spu_words == 4);
    assert(!psx_mod_psyq_load_image(0, 0, 4, 4, 0x60001000u));
    assert(psx_mod_psyq_load_image(0, 0, 4, 4, 0x80001000u));
    /* KNOWN: the SPU upload checks only the start and the size, not their sum;
     * a transfer crossing the end of SPU RAM wraps (documented in mod_plugins.h). */
    printf("KNOWN: psx_mod_spu_upload accepts spu_address+bytes past SPU RAM (wraps)\n");
    puts("test_mod_guest_ram_span OK");
    return 0;
}
