/* GP0(E1h) bit 11 (texture disable) must not reach GPUSTAT bit 15 unless
 * GP1(09h) allowed it. The runtime does not model GP1(09h) (it halts), so the
 * allow switch is always in its reset state and bit 15 must read 0.
 * GCC -O2 -fwhole-program removes unrelated renderer dependencies. */
#include "../src/gpu.c"

uint32_t vblank_cycles = 263000u;
uint32_t interrupts_get_cycles_since_vblank(void) { return 0u; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
int interrupts_raster_gpu_status(uint32_t *bits) { (void)bits; return 0; }

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; \
} } while (0)

static void draw_mode(uint32_t word) {
    gp0_cmd_buf[0] = word;
    gp0_exec_draw_mode();
}

int main(void) {
    /* Synthetic draw mode: texpage X=Ah Y=1, dither on, bit 11 set. */
    draw_mode(0xE1000A1Au);
    uint32_t stat = gpu_read_gpustat();
    CHECK((stat & 0x07FFu) == 0x021Au);   /* bits 0-10 copy E1 bits 0-10 */
    CHECK(((stat >> 15) & 1u) == 0u);     /* bit 15 stays clear */
    CHECK(texture_disable == 0u);

    /* All of bits 0-11 set: bits 0-10 follow, bit 15 still clear. */
    draw_mode(0xE1000FFFu);
    stat = gpu_read_gpustat();
    CHECK((stat & 0x07FFu) == 0x07FFu);
    CHECK(((stat >> 15) & 1u) == 0u);

    /* Bit 11 clear again: no change to bit 15. */
    draw_mode(0xE1000000u);
    stat = gpu_read_gpustat();
    CHECK((stat & 0x87FFu) == 0u);

    /* A saved GP0 cursor outside gp0_cmd_buf, or an unknown GP0 state, is
     * refused by the load's validate pass. */
    {
        static const int32_t bad[][3] = {
            { GP0_COLLECTING, 16, 20 }, { GP0_COLLECTING, -4000, 4 },
            { GP0_COLLECTING, 4, 4 }, { GP0_IDLE, 3, 2 }, { GP0_POLYLINE_SHADED + 1, 0, 0 },
        };
        const uint32_t n = gpu_snapshot_bytes();
        uint8_t *wire = malloc(n);
        CHECK(wire != NULL);
        gp0_state = GP0_COLLECTING;
        gp0_words_collected = 1;
        gp0_words_needed = 4;
        gpu_snapshot_write(wire);
        CHECK(gpu_snapshot_validate(wire, n));
        for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
            gp0_state = (Gp0State)bad[k][0];
            gp0_words_collected = bad[k][1];
            gp0_words_needed = bad[k][2];
            gpu_snapshot_write(wire);
            CHECK(!gpu_snapshot_validate(wire, n));
        }
        gp0_state = GP0_IDLE;
        gp0_words_collected = gp0_words_needed = 0;
        free(wire);
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: GP0(E1h) bit 11 does not set GPUSTAT bit 15 without GP1(09h); bad GP0 cursors refused");
    return 0;
}

void gr_set_dither(int enabled) { (void)enabled; }
