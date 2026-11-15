/* Rare GP0/GP1 commands (GPU-15): GP0(1Fh) sizing, GP1(02h) ack, GP1(10h..1Fh)
 * info mirrors. Same build recipe as test_gpu_texture_disable_status.c: the
 * fixture has no psx_fatal_halt, gr_vram_read or renderer, so only static helpers
 * that link without them are called (gp0_command_word_count, gp1_ack_irq1,
 * gp1_get_info, gp0_irq_request, gpu_read_gpustat) and GPUREAD is read from gpuread_latch.
 * GP1(09h) and GP1(20h+) hit psx_fatal_halt in gpu.c, so they are not
 * exercised here, and the dispatch itself is not linkable, so GP0(1Fh) is
 * tested through gp0_irq_request; the lines marked KNOWN
 * state today's behaviour that differs from psx-spx or is not covered. The
 * software renderer's texture modulation is not reachable from this build. */
#include "../src/gpu.c"

uint32_t vblank_cycles = 263000u;
uint32_t interrupts_get_cycles_since_vblank(void) { return 0u; }
static uint32_t raised_bit = 0xFFu;
void psx_irq_raise(uint32_t bit, uint32_t detail) { raised_bit = bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
int interrupts_raster_gpu_status(uint32_t *bits) { (void)bits; return 0; }

static int failures, known;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; \
} } while (0)
#define KNOWN(name, c) do { if (!(c)) { printf("KNOWN %s\n", name); known++; } } while (0)

int main(void) {
    /* GP0(1Fh) is a one-word command, like GP0(00h) and GP0(01h). */
    CHECK(gp0_command_word_count(0x1F) == 1);
    CHECK(gp0_command_word_count(0x00) == 1);
    CHECK(gp0_command_word_count(0x01) == 1);

    /* GP0(1Fh) sets the IRQ1 flag and raises I_STAT bit 1 (IRQ_GPU). */
    irq1_flag = 0;
    gp0_irq_request();
    CHECK(irq1_flag == 1u);
    CHECK(raised_bit == 1u);
    CHECK(((gpu_read_gpustat() >> 24) & 1u) == 1u);
    gp1_ack_irq1();
    CHECK(irq1_flag == 0u);

    /* IRQ1 flag shows in GPUSTAT bit 24 and GP1(02h) clears it. */
    irq1_flag = 1;
    CHECK(((gpu_read_gpustat() >> 24) & 1u) == 1u);
    gp1_ack_irq1();
    CHECK(irq1_flag == 0u);
    CHECK(((gpu_read_gpustat() >> 24) & 1u) == 0u);

    /* GP1(10h..1Fh): only the low 4 bits pick the index, so 1Xh mirrors 0Xh. */
    texture_window_value = 0x000ABCDEu;
    draw_area_left = 5; draw_area_top = 7;
    draw_area_right = 319; draw_area_bottom = 239;
    draw_offset_x = -3; draw_offset_y = 9;
    const uint32_t stat0 = gpu_read_gpustat();
    for (uint32_t hi = 0x10u; hi <= 0x1Fu; hi++) {
        const uint32_t base = hi << 24;
        gpuread_latch = 0xA5A5A5A5u;
        gp1_get_info(base | 0x2u);
        CHECK(gpuread_latch == 0x000ABCDEu);
        gp1_get_info(base | 0x3u);
        CHECK(gpuread_latch == (5u | (7u << 10)));
        gp1_get_info(base | 0x4u);
        CHECK(gpuread_latch == (319u | (239u << 10)));
        gp1_get_info(base | 0x5u);
        CHECK(gpuread_latch == ((0x7FDu) | (9u << 11)));
        gp1_get_info(base | 0x7u);
        CHECK(gpuread_latch == 2u);
        gp1_get_info(base | 0x8u);
        CHECK(gpuread_latch == 0u);
    }
    CHECK(gpu_read_gpustat() == stat0);   /* info requests do not touch GPUSTAT */

    /* Indices 0, 1, 6, 9..15 leave the latch alone today. What the hardware
     * returns for them is unknown to this test, so it is not asserted. */
    {
        static const uint32_t idx[] = { 0, 1, 6, 9, 10, 11, 12, 13, 14, 15 };
        for (unsigned k = 0; k < sizeof idx / sizeof idx[0]; k++) {
            gpuread_latch = 0xC0FFEE01u;
            gp1_get_info(0x10000000u | idx[k]);
            CHECK(gpuread_latch == 0xC0FFEE01u);
        }
    }

    /* GPU-4: a primitive the turbo drops still latches a textured polygon's
     * texpage (GPUSTAT bits 0-8, psx-spx TPage bits 0-8 = GP0(E1h) bits 0-8). */
    set_tpage_from_poly(0);
    s_draw_skip = 0;
    gp0_cmd_buf[4] = 0x01A50000u;
    CHECK(draw_skip_prim(0x24) == 0);                 /* no turbo: dispatch decides */
    CHECK((gpu_read_gpustat() & 0x1FFu) == 0u);
    s_draw_skip = 1;
    CHECK(draw_skip_prim(0x24) == 1);                 /* textured tri: word 4 */
    CHECK((gpu_read_gpustat() & 0x1FFu) == 0x1A5u);
    gp0_cmd_buf[5] = 0x00930000u;
    CHECK(draw_skip_prim(0x3E) == 1);                 /* shaded textured quad: word 5 */
    CHECK((gpu_read_gpustat() & 0x1FFu) == 0x093u);
    gp0_cmd_buf[4] = gp0_cmd_buf[5] = 0x01FF0000u;
    CHECK(draw_skip_prim(0x28) == 1);                 /* untextured quad */
    CHECK(draw_skip_prim(0x64) == 1);                 /* rectangle: no texpage word */
    CHECK((gpu_read_gpustat() & 0x1FFu) == 0x093u);
    CHECK(draw_skip_prim(0xE1) == 0);                 /* not a primitive */
    s_draw_skip = 0;

    /* Not exercised (cannot link in this build): GP1(09h) and GP1(20h-3Fh)
     * reach psx_fatal_halt in gpu_write_gp1. */
    KNOWN("GP1(09h) and GP1(20h-3Fh): fatal halt in gpu_write_gp1 (not exercised)", 0);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: GP0(1Fh) size, GP1(02h) ack, GP1(10h-1Fh) info; %d KNOWN\n", known);
    return 0;
}

void gr_set_dither(int enabled) { (void)enabled; }
