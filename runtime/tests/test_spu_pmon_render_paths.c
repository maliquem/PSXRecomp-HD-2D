/* With PMON chained, the voice-by-voice render (spu_render_voices) and the
 * frame-by-frame render (forced by SPUCNT.6) give the same samples. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"

uint64_t s_frame_count;
uint64_t psx_get_cycle_count(void) { return 0; }
void audio_trace_pcm(int t, const int16_t *p, int n) {(void)t;(void)p;(void)n;}
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) {(void)k;(void)a;(void)b;}
void psx_irq_raise(uint32_t b, uint32_t d) {(void)b;(void)d;}
uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {(void)p;(void)n;return c;}
bool spu_shadow_enabled(void) { return false; }
bool spu_shadow_active(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *p, int n) {(void)p;(void)n;}
int g_golden_on;
void golden_spu_block(const int16_t *s, uint32_t n) {(void)s;(void)n;}

static void stage(unsigned i, uint16_t rate, uint32_t addr) {
    SpuVoice *v = &voices[i];
    v->active = 1;
    v->sample_idx = SPU_BLOCK_SAMPLES;   /* decode on the first frame */
    v->env_level = 0x7FFF;
    v->adsr_phase = ADSR_SUSTAIN;
    v->cur_addr = addr;
    spu_regs[i * 8 + 0] = 0x3FFF;
    spu_regs[i * 8 + 1] = 0x2000;
    spu_regs[i * 8 + 2] = rate;
}

static int run(uint16_t ctrl, int16_t *out, int frames, uint32_t pmon) {
    spu_init();
    /* Pseudo-random ADPCM over 0x2000..0x6000, looping blocks. */
    uint32_t x = 12345;
    for (uint32_t a = 0x2000; a < 0x6000; a += 16) {
        x = x * 1103515245u + 12345u;
        spu_ram[a] = (uint8_t)((x >> 16) & 0x3F);  /* shift and filter */
        spu_ram[a + 1] = (a % 0x800 == 0) ? 4 : (a % 0x800 == 0x7F0) ? 3 : 0;  /* LOOP_START, END+REPEAT */
        for (int k = 2; k < 16; k++) { x = x * 1103515245u + 12345u; spu_ram[a + k] = (uint8_t)(x >> 24); }
    }
    const uint16_t rates[6] = {0x0800, 0x1000, 0x2C00, 0x3FFF, 0x5000, 0x1234};
    for (unsigned i = 0; i < 6; i++) stage(i, rates[i], 0x2000 + i * 0x800);
    spu_regs[reg_index(0x1F801D90u)] = (uint16_t)pmon;
    spu_regs[reg_index(0x1F801D80u)] = 0x3FFF;
    spu_regs[reg_index(0x1F801D82u)] = 0x3FFF;
    spu_regs[reg_index(0x1F801DA4u)] = 0x0010;   /* IRQ address in the capture area */
    spu_regs[reg_index(0x1F801DAAu)] = ctrl;
    int16_t *p = out;
    for (int done = 0; done < frames; ) {
        int n = frames - done > 300 ? 300 : frames - done;
        spu_render_block(p, n);
        p += 2 * n; done += n;
    }
    return (int)g_spu_blocks_by_voice;
}

int main(void) {
    enum { F = 6000 };
    static int16_t a[2 * F], b[2 * F];
    int fails = 0;
    const uint32_t masks[3] = {0, 0x3Eu, 0x2Au};
    for (int m = 0; m < 3; m++) {
        g_spu_blocks_by_voice = 0;
        int va = run(0x8000, a, F, masks[m]);
        g_spu_blocks_by_voice = 0;
        int vb = run(0x8040, b, F, masks[m]);
        int diff = 0;
        for (int i = 0; i < 2 * F; i++) diff += a[i] != b[i];
        long nz = 0;
        for (int i = 0; i < 2 * F; i++) nz += a[i] != 0;
        printf("pmon 0x%02X: voice-major blocks %d / frame-major blocks %d, %d of %d samples differ, %ld nonzero\n",
               masks[m], va, vb, diff, 2 * F, nz);
        if (diff || va == 0 || vb != 0) fails++;
    }
    puts(fails ? "FAIL" : "PASS: both render paths agree with PMON");
    return fails;
}
