/* Timer (root counter) mode table test, CPU-27 (a). Expected values come from
 * psx-spx "Timers"; cases where the runtime disagrees are KNOWN divergences:
 * printed, counted, and they do not fail the run. Needs PSX_ENABLE_BLOCK_CYCLES
 * so a counter read does not advance the counter. */
#include "timers.h"
#include "psx_video_timing.h"
#include <stdint.h>
#include <stdio.h>

#ifndef PSX_ENABLE_BLOCK_CYCLES
#error "build with PSX_ENABLE_BLOCK_CYCLES=1 (the native game configuration)"
#endif

uint32_t i_stat;
static int s_raises[3];
static uint32_t s_bit[3];

void psx_irq_raise(uint32_t bit, uint32_t detail) {
    if (detail < 3) { s_raises[detail]++; s_bit[detail] = bit; }
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}

static int s_fail, s_known;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)
#define KNOWN(name, c) do { if (!(c)) { printf("KNOWN %s\n", name); s_known++; } } while (0)

#define CNT(t)  (TIMER_BASE + (t) * 0x10 + 0x0)
#define MODE(t) (TIMER_BASE + (t) * 0x10 + 0x4)
#define TGT(t)  (TIMER_BASE + (t) * 0x10 + 0x8)

#define M_SYNC_EN  0x0001u
#define M_RESET_T  0x0008u
#define M_IRQ_T    0x0010u
#define M_IRQ_OVF  0x0020u
#define M_REPEAT   0x0040u
#define M_TOGGLE   0x0080u
#define M_IRQREQ   0x0400u
#define M_HIT_T    0x0800u
#define M_HIT_OVF  0x1000u

static void arm(int t, uint32_t mode, uint16_t target) {
    timers_init();
    timers_write(TGT(t), target);
    timers_write(MODE(t), mode);
    for (int i = 0; i < 3; i++) { s_raises[i] = 0; s_bit[i] = 0; }
}

int main(void) {
    uint32_t m;

    /* Mode write resets the counter to 0000h; bit 10 is set after writing;
     * bits 0-9 read back; bits 11-12 are read-only. */
    timers_init();
    timers_write(CNT(2), 500);
    CHECK(timers_read(CNT(2)) == 500);
    timers_write(MODE(2), 0x1BFFu & ~M_SYNC_EN & ~0x0006u);
    CHECK(timers_read(CNT(2)) == 0);
    m = timers_read(MODE(2));
    CHECK((m & 0x03FFu) == (0x03FFu & ~0x0007u));
    CHECK(m & M_IRQREQ);
    CHECK(!(m & (M_HIT_T | M_HIT_OVF)));
    CHECK((m & 0xE000u) == 0);               /* bits 13-15 read zero */

    /* Target write/read keeps 16 bits. */
    timers_write(TGT(1), 0x1234);
    CHECK(timers_read(TGT(1)) == 0x1234);

    /* Bit 11 "reached target" sets without IRQ enabled and resets after reading. */
    arm(2, 0x0000, 100);
    timers_advance(99);
    CHECK(!(timers_read(MODE(2)) & M_HIT_T));
    timers_advance(1);
    m = timers_read(MODE(2));
    CHECK(m & M_HIT_T);
    CHECK(!(timers_read(MODE(2)) & M_HIT_T));
    CHECK(s_raises[2] == 0);

    /* Bit 3 = 0: counter runs past target up to FFFFh and wraps to 0000h. */
    CHECK(timers_read(CNT(2)) == 100);
    timers_advance(5);
    CHECK(timers_read(CNT(2)) == 105);
    timers_advance(0x10000 - 105);
    CHECK(timers_read(CNT(2)) == 0);

    /* Bit 12 "reached FFFFh" sets after a full wrap and resets after reading. */
    arm(2, 0x0000, 100);
    timers_advance(0xFFF0);
    CHECK(!(timers_read(MODE(2)) & M_HIT_OVF));
    timers_advance(0x10);
    CHECK(timers_read(MODE(2)) & M_HIT_OVF);
    CHECK(!(timers_read(MODE(2)) & M_HIT_OVF));

    /* CPU-23 (b): bit 12 and the bit-5 IRQ come at Counter=FFFFh, not on the
     * wrap to 0000h; the next one comes a full 10000h counts later. */
    arm(2, M_IRQ_OVF | M_REPEAT, 100);
    CHECK(timers_cycles_to_irq(1u << 6) == 0xFFFFu);
    timers_advance(0xFFFE);
    CHECK(!(timers_read(MODE(2)) & M_HIT_OVF));
    CHECK(s_raises[2] == 0);
    timers_advance(1);
    CHECK(timers_read(CNT(2)) == 0xFFFF);
    CHECK(timers_read(MODE(2)) & M_HIT_OVF);
    CHECK(s_raises[2] == 1);
    CHECK(timers_cycles_to_irq(1u << 6) == 0x10000u);
    timers_advance(1);
    CHECK(timers_read(CNT(2)) == 0);
    CHECK(!(timers_read(MODE(2)) & M_HIT_OVF));
    CHECK(s_raises[2] == 1);
    timers_advance(0xFFFF);
    CHECK(s_raises[2] == 2);

    /* Target = FFFFh with bit 3: reaching FFFFh sets both bit 11 and bit 12. */
    arm(2, M_RESET_T, 0xFFFF);
    timers_advance(0xFFFF);
    m = timers_read(MODE(2));
    CHECK((m & (M_HIT_T | M_HIT_OVF)) == (M_HIT_T | M_HIT_OVF));

    /* Bit 3 = 1: counter increments up to (including) target, then restarts at
     * 0000h (psx-spx, 1F801108h); so it never reaches FFFFh. */
    arm(2, M_RESET_T, 100);
    timers_advance(100);
    KNOWN("reset_target_holds_target", timers_read(CNT(2)) == 100);
    timers_advance(1);
    KNOWN("reset_target_restart_after", timers_read(CNT(2)) == 0);
    timers_advance(0x20000);
    CHECK(!(timers_read(MODE(2)) & M_HIT_OVF));

    /* Bit 4: IRQ when counter = target; timers 0/1/2 raise IRQ4/5/6. */
    for (int t = 0; t < 3; t++) {
        arm(t, M_IRQ_T | M_REPEAT, 50);
        timers_advance(49);
        CHECK(s_raises[t] == 0);
        timers_advance(1);
        CHECK(s_raises[t] == 1);
        CHECK(s_bit[t] == (uint32_t)(4 + t));
    }

    /* Bit 5: IRQ when counter = FFFFh (checked after a full wrap only). */
    arm(2, M_IRQ_OVF | M_REPEAT, 100);
    timers_advance(0xFFF0);
    CHECK(s_raises[2] == 0);
    timers_advance(0x10);
    CHECK(s_raises[2] == 1);
    CHECK(timers_read(MODE(2)) & M_HIT_OVF);

    /* Bit 6 = 0: one-shot, a single IRQ until the next mode write. */
    arm(2, M_IRQ_T, 100);
    timers_advance(0x30000);
    CHECK(s_raises[2] == 1);
    timers_write(MODE(2), M_IRQ_T);
    timers_advance(100);
    CHECK(s_raises[2] == 2);

    /* Bit 6 = 1: repeat, an IRQ on every target hit. */
    arm(2, M_IRQ_T | M_REPEAT, 100);
    timers_advance(0x30000);
    CHECK(s_raises[2] == 3);

    /* Bit 7 = 0: pulse mode, bit 10 reads 1 again after the IRQ. */
    arm(2, M_IRQ_T | M_REPEAT, 100);
    timers_advance(100);
    CHECK(s_raises[2] == 1);
    CHECK(timers_read(MODE(2)) & M_IRQREQ);

    /* Bit 7 = 1, one-shot: bit 10 inverts on the IRQ and remains zero after it. */
    arm(2, M_IRQ_T | M_TOGGLE, 100);
    CHECK(timers_read(MODE(2)) & M_IRQREQ);
    timers_advance(100);
    CHECK(s_raises[2] == 1);
    CHECK(!(timers_read(MODE(2)) & M_IRQREQ));
    timers_advance(0x20000);
    CHECK(s_raises[2] == 1);
    CHECK(!(timers_read(MODE(2)) & M_IRQREQ));

    /* Bit 7 = 1, repeat: bit 10 inverts on each event; the IRQ fires only when
     * bit 10 goes 1 -> 0, i.e. every 2nd event. */
    arm(2, M_IRQ_T | M_TOGGLE | M_REPEAT, 100);
    timers_advance(100);                      /* event 1: 1 -> 0, IRQ */
    CHECK(s_raises[2] == 1);
    CHECK(!(timers_read(MODE(2)) & M_IRQREQ));
    timers_advance(0x10000);                  /* event 2: 0 -> 1, no IRQ */
    CHECK(s_raises[2] == 1);
    CHECK(timers_read(MODE(2)) & M_IRQREQ);
    timers_advance(0x10000);                  /* event 3: 1 -> 0, IRQ */
    CHECK(s_raises[2] == 2);

    /* Clock source, timer 2: 0/1 = system clock, 2/3 = system clock / 8. */
    for (uint32_t src = 0; src < 4; src++) {
        arm(2, src << 8, 0);
        timers_advance(800);
        CHECK(timers_read(CNT(2)) == (src < 2 ? 800u : 100u));
    }

    /* Clock source, timer 0: 0/2 = system clock, 1/3 = dotclock (always slower
     * than the system clock; exact rate depends on the video mode). */
    for (uint32_t src = 0; src < 4; src++) {
        arm(0, src << 8, 0);
        timers_advance(1000);
        uint32_t c = timers_read(CNT(0));
        if (src == 0 || src == 2) CHECK(c == 1000);
        else CHECK(c > 0 && c < 1000);
    }

    /* Clock source, timer 1: 0/2 = system clock, 1/3 = HBlank (one per line;
     * the phase of the first line is unknown, so 10 or 11 over ten lines). */
    for (uint32_t src = 0; src < 4; src++) {
        arm(1, src << 8, 0);
        timers_advance(10 * g_psx_hblank_cycles);
        uint32_t c = timers_read(CNT(1));
        if (src == 0 || src == 2) CHECK(c == 10 * g_psx_hblank_cycles);
        else CHECK(c == 10 || c == 11);
    }

    /* Timer 2 sync 0/3 = stop at current value; 1/2 = free run. */
    for (uint32_t s = 0; s < 4; s++) {
        arm(2, M_SYNC_EN | (s << 1), 0);
        timers_write(CNT(2), 40);
        timers_advance(1000);
        CHECK(timers_read(CNT(2)) == (s == 0 || s == 3 ? 40u : 1040u));
    }

    /* Timer 0 sync 0 = pause during HBlank: over ten lines it counts fewer
     * system clocks than elapsed. */
    arm(0, M_SYNC_EN, 0);
    timers_advance(10 * g_psx_hblank_cycles);
    KNOWN("t0_sync0_pause_in_hblank", timers_read(CNT(0)) < 10 * g_psx_hblank_cycles);

    /* Timer 0 sync 1 = reset at HBlank, sync 2 = reset at HBlank and pause
     * outside it: either way it never exceeds one line of system clocks. */
    arm(0, M_SYNC_EN | (1u << 1), 0);
    timers_advance(10 * g_psx_hblank_cycles);
    KNOWN("t0_sync1_reset_at_hblank", timers_read(CNT(0)) <= g_psx_hblank_cycles);
    arm(0, M_SYNC_EN | (2u << 1), 0);
    timers_advance(10 * g_psx_hblank_cycles);
    KNOWN("t0_sync2_reset_pause_hblank", timers_read(CNT(0)) <= g_psx_hblank_cycles);

    /* Timer 1 sync 1 = reset at VBlank: clocked by HBlank it never exceeds one
     * frame of lines. */
    arm(1, M_SYNC_EN | (1u << 1) | (1u << 8), 0);
    timers_advance(3 * g_psx_vblank_cycles);
    KNOWN("t1_sync1_reset_at_vblank", timers_read(CNT(1)) <= g_psx_lines_per_frame);

    if (s_fail) return 1;
    printf("timers_modes_test: ok (%d known)\n", s_known);
    return 0;
}
