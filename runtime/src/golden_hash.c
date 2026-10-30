/* Golden verification hashes — see include/golden_hash.h. */
#include "golden_hash.h"
#include "cpu_state.h"
#include "psx_memory.h"

#include <stdlib.h>
#include <string.h>

extern uint8_t *g_psx_ram;
extern uint8_t *g_psx_scratch;
extern uint32_t g_psx_ram_mask;
extern uint64_t psx_get_cycle_count(void);

int g_golden_on = 0;
static int s_arm_on_load = 0;

#define GOLDEN_SEED 0xCBF29CE484222325ull
#define GOLDEN_MUL  0x100000001B3ull
#define GOLDEN_CAP  16384u

static uint64_t s_gpu = GOLDEN_SEED, s_spu = GOLDEN_SEED;
static uint32_t s_gpu_n, s_spu_n, s_frame;
static GoldenEntry *s_ring;
static uint64_t s_total;

static inline uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * GOLDEN_MUL; }

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = GOLDEN_SEED;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = mix(h, v); }
    for (; i < n; i++) h = mix(h, p[i]);
    return h;
}

void golden_gpu_word(uint32_t word, int port) {
    s_gpu = mix(s_gpu, ((uint64_t)(uint32_t)port << 32) | word);
    s_gpu_n++;
}

void golden_spu_block(const int16_t *stereo, uint32_t frames) {
    s_spu = mix(s_spu, hash_bytes((const uint8_t *)stereo, (size_t)frames * 4u));
    s_spu_n += frames;
}

static void reset_frame_hashes(void) {
    s_gpu = GOLDEN_SEED; s_spu = GOLDEN_SEED; s_gpu_n = 0; s_spu_n = 0;
}

void golden_arm(int on, int on_load) {
    if (on_load) { s_arm_on_load = 1; g_golden_on = 0; return; }
    g_golden_on = on ? 1 : 0;
    if (on) {
        if (!s_ring) s_ring = (GoldenEntry *)calloc(GOLDEN_CAP, sizeof *s_ring);
        s_total = 0; s_frame = 0;
        reset_frame_hashes();
        if (!s_ring) g_golden_on = 0;
    }
}

void golden_note_savestate_loaded(void) {
    if (!s_arm_on_load) return;
    s_arm_on_load = 0;
    golden_arm(1, 0);
}

void golden_frame(const struct CPUState *cpu) {
    if (!g_golden_on || !s_ring) return;
    GoldenEntry *e = &s_ring[s_total % GOLDEN_CAP];
    e->frame = s_frame++;
    e->gpu_words = s_gpu_n;
    e->spu_samples = s_spu_n;
    e->pad = 0;
    e->cycles = psx_get_cycle_count();
    e->ram = g_psx_ram ? hash_bytes(g_psx_ram, (size_t)g_psx_ram_mask + 1u) : 0;
    e->scratch = g_psx_scratch ? hash_bytes(g_psx_scratch, 1024u) : 0;
    if (cpu) {
        uint64_t h = hash_bytes((const uint8_t *)cpu->gpr, sizeof cpu->gpr);
        h = mix(h, cpu->hi); h = mix(h, cpu->lo);
        e->cpu = h;
        e->gte = mix(hash_bytes((const uint8_t *)cpu->gte_data, sizeof cpu->gte_data),
                     hash_bytes((const uint8_t *)cpu->gte_ctrl, sizeof cpu->gte_ctrl));
    } else {
        e->cpu = e->gte = 0;
    }
    e->gpu = s_gpu;
    e->spu = s_spu;
    s_total++;
    reset_frame_hashes();
}

uint64_t golden_total(void) { return s_total; }

int golden_entries(uint32_t from_frame, uint32_t count, const GoldenEntry **ring, uint32_t *cap,
                   uint32_t *start) {
    (void)from_frame; (void)count;
    *ring = s_ring;
    *cap = GOLDEN_CAP;
    *start = s_total > GOLDEN_CAP ? (uint32_t)(s_total % GOLDEN_CAP) : 0u;
    return s_ring ? (int)(s_total < GOLDEN_CAP ? s_total : GOLDEN_CAP) : 0;
}
