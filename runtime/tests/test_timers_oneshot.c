/* CPU-8: a one-shot timer IRQ (mode bit 6 = 0) fires once and keeps bits 4/5
 * readable as written; the next mode write re-arms it. The internal "spent"
 * state rides in the 48-byte .pst timer section (mode word), so it also has
 * to survive a snapshot round-trip and pass the pass-1 range check. */
#include "timers.h"
#include <stdint.h>
#include <stdio.h>

uint32_t i_stat;
static int s_raises[3];

void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)bit;
    if (detail < 3) s_raises[detail]++;
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

#define T2_COUNT  (TIMER_BASE + 0x20)
#define T2_MODE   (TIMER_BASE + 0x24)
#define T2_TARGET (TIMER_BASE + 0x28)
#define IRQ_T2    (1u << 6)

static void arm_t2(uint32_t mode, uint16_t target) {
    timers_write(T2_TARGET, target);
    timers_write(T2_MODE, mode);       /* sysclk source, counter -> 0 */
    s_raises[2] = 0;
}

int main(void) {
    uint16_t c[3], t[3]; uint32_t m[3], f[3]; int32_t l[3];

    timers_init();

    /* One-shot on target: one IRQ, bit 4 still reads 1, target flag set. */
    arm_t2(0x0010, 100);
    timers_advance(100);
    CHECK(s_raises[2] == 1);
    uint32_t mode = timers_read(T2_MODE);
    CHECK(mode & 0x0010);
    CHECK(mode & 0x0800);
    CHECK(!(mode & 0x80000000u));      /* internal bit never guest-visible */
    timers_advance(0x10000);           /* reaches target again */
    CHECK(s_raises[2] == 1);
    CHECK(timers_read(T2_MODE) & 0x0800);  /* flag still raised, no IRQ */
    CHECK(timers_cycles_to_irq(IRQ_T2) == 0xFFFFFFFFu);

    /* Snapshot round-trip keeps the spent state. */
    timers_get_snapshot(c, m, t, l, f);
    CHECK(timers_snapshot_mode_ok(m[2]));
    timers_init();
    timers_set_snapshot(c, m, t, l, f);
    s_raises[2] = 0;
    timers_advance(0x20000);
    CHECK(s_raises[2] == 0);

    /* Mode write re-arms. */
    arm_t2(0x0010, 100);
    CHECK(timers_cycles_to_irq(IRQ_T2) == 100u);
    timers_advance(100);
    CHECK(s_raises[2] == 1);

    /* One-shot with target and overflow: only the first condition fires. */
    arm_t2(0x0030, 100);
    timers_advance(0x10000);
    CHECK(s_raises[2] == 1);
    mode = timers_read(T2_MODE);
    CHECK((mode & 0x0030) == 0x0030);
    CHECK((mode & 0x1800) == 0x1800);

    /* Repeat mode keeps firing. */
    arm_t2(0x0050, 100);
    timers_advance(100);
    timers_advance(0x10000);
    CHECK(s_raises[2] == 2);

    /* A state from an older build (no spent bit, armed) fires once. */
    timers_init();
    for (int i = 0; i < 3; i++) { c[i] = 0; m[i] = 0; t[i] = 0; l[i] = 0; f[i] = 0; }
    m[2] = 0x0410; t[2] = 50;
    timers_set_snapshot(c, m, t, l, f);
    s_raises[2] = 0;
    timers_advance(50);
    timers_advance(0x10000);
    CHECK(s_raises[2] == 1);

    /* Pass-1 range check. */
    CHECK(timers_snapshot_mode_ok(0x1FFFu));
    CHECK(timers_snapshot_mode_ok(0x80001FFFu));
    CHECK(!timers_snapshot_mode_ok(0x2000u));
    CHECK(!timers_snapshot_mode_ok(0x40000000u));

    if (s_fail) return 1;
    printf("timers_oneshot_test: ok\n");
    return 0;
}
