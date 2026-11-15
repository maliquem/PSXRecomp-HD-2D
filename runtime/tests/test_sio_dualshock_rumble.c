/* DualShock rumble protocol regression: 0x4D map negotiation must feed the
 * small/large motor bytes from later 0x42 polls to the frontend-facing state. */

#include "sio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pass-1 range check (declared extern by boot_state.c, not in sio.h). */
int sio_snapshot_validate(const uint8_t *p, uint32_t len);

uint32_t sio_snapshot_bytes(void);
void sio_snapshot_write(uint8_t *p);
int sio_snapshot_read(const uint8_t *p, uint32_t len);

uint32_t i_stat = 0;
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }

/* sio.c gates card-transfer deferral on `psx_get_cycle_count() < deadline`, so
 * a constant clock would defer forever. Advance monotonically. */
uint64_t psx_get_cycle_count(void) {
    static uint64_t t;
    return t += 64;
}
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) {
    (void)a; (void)v; (void)w;
}
void starvation_ring_record(uint8_t k, uint8_t tx, uint8_t rx, uint16_t c,
                            uint16_t st, uint8_t d, uint8_t ss, uint16_t ms,
                            uint8_t mc, uint16_t sec, uint8_t di, uint32_t f) {
    (void)k; (void)tx; (void)rx; (void)c; (void)st; (void)d; (void)ss;
    (void)ms; (void)mc; (void)sec; (void)di; (void)f;
}
void card_read_summary_record(uint8_t s, uint8_t c, uint16_t sec, uint8_t e,
                              uint8_t chk, uint8_t d0, uint8_t d1, uint32_t f) {
    (void)s; (void)c; (void)sec; (void)e; (void)chk; (void)d0; (void)d1; (void)f;
}
void card_data_writes_arm(uint8_t v, uint16_t s, uint8_t i, uint8_t sl) {
    (void)v; (void)s; (void)i; (void)sl;
}
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) {
    (void)k; (void)d; (void)a;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail; i_stat |= 1u << bit;
}
int memcard_is_present(int card) { (void)card; return 0; }
int memcard_read_sector(int card, int sector, uint8_t *buf) {
    (void)card; (void)sector; memset(buf, 0, 128); return -1;
}
int memcard_write_sector(int card, int sector, const uint8_t *buf) {
    (void)card; (void)sector; (void)buf; return -1;
}
void memcard_flush(int card) { (void)card; }

#define SIO_TX_DATA 0x1F801040u
#define SIO_RX_DATA 0x1F801040u
#define SIO_CTRL    0x1F80104Au
#define CTRL_TX_EN      (1u << 0)
#define CTRL_SELECT     (1u << 1)
#define CTRL_ACK        (1u << 4)
#define CTRL_ACK_IRQ_EN (1u << 12)
#define CTRL_SLOT       (1u << 13)

static int failures;

#define EXPECT(label, expected, actual) do {                                  \
    unsigned e_ = (unsigned)(expected), a_ = (unsigned)(actual);              \
    if (e_ != a_) {                                                           \
        fprintf(stderr, "FAIL %s: expected=0x%X actual=0x%X\n",             \
                label, e_, a_);                                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t xchg(int slot, uint8_t tx) {
    const uint16_t ctrl = (uint16_t)(CTRL_TX_EN | CTRL_SELECT |
        CTRL_ACK_IRQ_EN | (slot ? CTRL_SLOT : 0));
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    const uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

static void finish_six_data_bytes(int slot, unsigned already_sent) {
    while (already_sent++ < 6) (void)xchg(slot, 0x00);
}

static void enter_config(int slot) {
    EXPECT("enter.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("enter.id", 0x73, xchg(slot, 0x43));
    EXPECT("enter.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, 0x01);
    finish_six_data_bytes(slot, 1);
}

static void set_rumble_map(int slot, const uint8_t map[6],
                           const uint8_t expected_old[6]) {
    EXPECT("map.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("map.id", 0xF3, xchg(slot, 0x4D));
    EXPECT("map.ack", 0x5A, xchg(slot, 0x00));
    for (int i = 0; i < 6; i++)
        EXPECT("map.echo", expected_old[i], xchg(slot, map[i]));
}

static void exit_config(int slot) {
    EXPECT("exit.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("exit.id", 0xF3, xchg(slot, 0x43));
    EXPECT("exit.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, 0x00);
    finish_six_data_bytes(slot, 1);
}

static void poll_with_motors(int slot, uint8_t small, uint8_t large) {
    EXPECT("poll.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("poll.id", 0x73, xchg(slot, 0x42));
    EXPECT("poll.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, small);
    (void)xchg(slot, large);
    finish_six_data_bytes(slot, 2);
}

int main(void) {
    static const uint8_t unassigned[6] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const uint8_t standard[6] = {
        0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF
    };
    uint8_t small = 0, large = 0;

    sio_init();
    sio_connect_pad(0);
    sio_set_pad_analog(0, 1, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(0, 1);

    enter_config(0);
    set_rumble_map(0, standard, unassigned);
    set_rumble_map(0, standard, standard);
    exit_config(0);

    poll_with_motors(0, 0x01, 0x80);
    sio_get_pad_rumble(0, &small, &large);
    EXPECT("rumble.small.on", 0x01, small);
    EXPECT("rumble.large.strength", 0x80, large);

    const uint32_t snapshot_len = sio_snapshot_bytes();
    uint8_t *snapshot = (uint8_t *)malloc(snapshot_len);
    if (!snapshot) {
        fprintf(stderr, "FAIL snapshot allocation\n");
        return 1;
    }
    sio_snapshot_write(snapshot);

    poll_with_motors(0, 0x00, 0x00);
    sio_get_pad_rumble(0, &small, &large);
    EXPECT("rumble.small.off", 0x00, small);
    EXPECT("rumble.large.off", 0x00, large);

    EXPECT("snapshot.current.read", 1,
           sio_snapshot_read(snapshot, snapshot_len));
    sio_get_pad_rumble(0, &small, &large);
    EXPECT("snapshot.current.small", 0x01, small);
    EXPECT("snapshot.current.large", 0x80, large);

    /* The present-hold window (seq u32, progress u64, armed u8) sits before
     * the 34-byte multitap block: round-trip it, refuse a non-flag armed
     * byte, and accept a state without it. */
    enum { MTAP = 34 };
    const uint32_t hold_end = snapshot_len - MTAP;
    {
        enum { HOLD = 13 };
        static const uint8_t hold[HOLD] = {0x78, 0x56, 0x34, 0x12,
                                           0x88, 0x77, 0x66, 0x55,
                                           0x44, 0x33, 0x22, 0x11, 0x01};
        uint8_t *again = (uint8_t *)malloc(snapshot_len);
        if (!again) return 1;
        memcpy(snapshot + hold_end - HOLD, hold, HOLD);
        EXPECT("snapshot.hold.read", 1,
               sio_snapshot_read(snapshot, snapshot_len));
        sio_snapshot_write(again);
        EXPECT("snapshot.hold.roundtrip", 0,
               memcmp(again + hold_end - HOLD, hold, HOLD));
        snapshot[hold_end - 1] = 2;
        EXPECT("snapshot.hold.armed-range", 0,
               sio_snapshot_validate(snapshot, snapshot_len));
        EXPECT("snapshot.pre-mtap.armed-range", 0,
               sio_snapshot_validate(snapshot, hold_end));
        snapshot[hold_end - 1] = 1;
        EXPECT("snapshot.pre-hold.read", 1,
               sio_snapshot_read(snapshot, hold_end - HOLD));
        sio_snapshot_write(again);
        EXPECT("snapshot.pre-hold.disarmed", 0, again[hold_end - 1]);
        EXPECT("snapshot.pre-hold.seq", 0,
               again[hold_end - HOLD] | again[hold_end - HOLD + 1]);

        /* Multitap block: addr, next_mode[2], req_this[2], returned[2],
         * active logical pad, response bytes 8..33. Round-trip it, refuse
         * out-of-range bytes, and reset it to Slot A when it is absent. */
        uint8_t mtap[MTAP];
        mtap[0] = 0x03; mtap[1] = 1; mtap[2] = 2; mtap[3] = 1; mtap[4] = 0;
        mtap[5] = 2; mtap[6] = 1; mtap[7] = 1;
        for (unsigned i = 8; i < MTAP; ++i) mtap[i] = (uint8_t)(0xA0u + i);
        memcpy(snapshot + hold_end, mtap, MTAP);
        EXPECT("snapshot.mtap.validate", 1,
               sio_snapshot_validate(snapshot, snapshot_len));
        EXPECT("snapshot.mtap.read", 1,
               sio_snapshot_read(snapshot, snapshot_len));
        sio_snapshot_write(again);
        EXPECT("snapshot.mtap.roundtrip", 0,
               memcmp(again + hold_end, mtap, MTAP));
        static const unsigned bad_at[] = {0, 0, 1, 3, 5, 7};
        static const uint8_t bad_val[] = {0x00, 0x05, 3, 2, 3, PSX_MAX_PLAYERS};
        for (unsigned k = 0; k < sizeof bad_at / sizeof bad_at[0]; ++k) {
            snapshot[hold_end + bad_at[k]] = bad_val[k];
            EXPECT("snapshot.mtap.range", 0,
                   sio_snapshot_validate(snapshot, snapshot_len));
            snapshot[hold_end + bad_at[k]] = mtap[bad_at[k]];
        }
        EXPECT("snapshot.pre-mtap.read", 1,
               sio_snapshot_read(snapshot, hold_end));
        sio_snapshot_write(again);
        EXPECT("snapshot.pre-mtap.addr", 0x01, again[hold_end]);
        EXPECT("snapshot.pre-mtap.modes", 0,
               again[hold_end + 1] | again[hold_end + 2] | again[hold_end + 3] |
               again[hold_end + 4] | again[hold_end + 5] | again[hold_end + 6]);
        EXPECT("snapshot.pre-mtap.logical", 0, again[hold_end + 7]);
        EXPECT("snapshot.pre-mtap.hold", 0,
               memcmp(again + hold_end - HOLD, hold, HOLD));
        free(again);
    }

    /* The pad-mode section (0x44 lock + device) and the JogCon rollback
     * fields each add four bytes for a two-seat build. Removing them restores
     * the preceding formats; removing all twenty-four bytes before the hold
     * block models a pre-rumble state. */
    EXPECT("snapshot.pre-pad-mode.read", 1,
           sio_snapshot_read(snapshot, hold_end - 13 - 4));
    EXPECT("snapshot.pre-jogcon.read", 1,
           sio_snapshot_read(snapshot, hold_end - 13 - 8));
    EXPECT("snapshot.pre-jogcon.small", 0x01,
           (sio_get_pad_rumble(0, &small, &large), small));
    EXPECT("snapshot.pre-jogcon.large", 0x80, large);
    EXPECT("snapshot.legacy.read", 1,
           sio_snapshot_read(snapshot, hold_end - 13 - 24));
    sio_get_pad_rumble(0, &small, &large);
    EXPECT("snapshot.legacy.small", 0x00, small);
    EXPECT("snapshot.legacy.large", 0x00, large);
    free(snapshot);

    poll_with_motors(0, 0x01, 0xFF);
    sio_set_pad_connected(0, 0);
    sio_get_pad_rumble(0, &small, &large);
    EXPECT("rumble.disconnect.small", 0x00, small);
    EXPECT("rumble.disconnect.large", 0x00, large);

    if (failures) {
        fprintf(stderr, "DualShock rumble protocol: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "DualShock rumble protocol: passed\n");
    return 0;
}
