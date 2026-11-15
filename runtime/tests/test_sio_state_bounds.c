/* SIO savestate load: trailing hold/multitap fields out of range (MEM-24, MEM-2).
 * The card cursors, slots, pad state, device and bus owner are covered by the
 * "snapshot" scenario of test_sio_card_irq_integrity.c; this adds the tail
 * block (hold flag, multitap latch, response length), the legacy layouts, wrong
 * lengths, and a byte-exact round trip. Same link recipe as that test.
 */
#include "../src/sio.c"
#include <stdio.h>
#include <stdlib.h>

uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
static int checks, failures;

uint64_t psx_get_cycle_count(void) { return 0; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a; return 0; }
uint32_t psx_read_word(uint32_t a) { (void)a; return 0; }
uint32_t memory_get_sr(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void starvation_ring_record(uint8_t a, uint8_t b, uint8_t c, uint16_t d, uint16_t e,
    int f, int g, int h, int i, int j, uint8_t k, uint32_t l,
    uint8_t m, uint8_t n, uint8_t o, uint8_t p, int q) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h; (void)i; (void)j; (void)k; (void)l;
    (void)m; (void)n; (void)o; (void)p; (void)q;
}
void card_read_summary_record(uint8_t a, uint8_t b, uint16_t c, uint8_t d,
    uint8_t e, const uint8_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
void card_data_writes_arm(uint8_t a, uint16_t b, uint8_t c, uint8_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
int memcard_is_present(int slot) { (void)slot; return 0; }
int memcard_read_sector(int slot, int sector, uint8_t *buf) {
    (void)slot; (void)sector; memset(buf, 0, 128); return 0;
}
int memcard_write_sector(int slot, int sector, const uint8_t *buf) {
    (void)slot; (void)sector; (void)buf; return 0;
}
void memcard_flush(int slot) { (void)slot; }

static void check(const char *name, int ok) {
    checks++; if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

int main(void) {
    const uint32_t n = sio_snapshot_bytes();
    const uint32_t mtap = SIO_SNAP_MTAP_BYTES;
    const uint32_t resp_len_at = 10u + PSX_MAX_PLAYERS + 1u + 4u + 4u + 8u;
    uint8_t *good = malloc(n), *bad = malloc(n), *again = malloc(n);
    /* Offsets in the tail block (see sio_snapshot_tail_ok). */
    const uint32_t hold_at = n - mtap - 1u, m = n - mtap;
    struct { const char *name; uint32_t at; uint8_t val; } cases[] = {
        { "refuses hold flag 2",             hold_at,      2 },
        { "refuses multitap address 0",      m + 0,        0 },
        { "refuses multitap address 5",      m + 0,        5 },
        { "refuses next_mode past enum",     m + 1,        (uint8_t)(MTAP_NEXT_GARBAGE + 1) },
        { "refuses req_this 2",              m + 3,        2 },
        { "refuses returned past enum",      m + 5,        (uint8_t)(MTAP_NEXT_GARBAGE + 1) },
        { "refuses active logical pad",      m + 7,        (uint8_t)PSX_MAX_PLAYERS },
        { "refuses response length 35",      resp_len_at,  PAD_RESPONSE_MAX + 1 },
    };

    sio_init();
    sio_snapshot_write(good);
    check("good state validates", sio_snapshot_validate(good, n));
    check("good state loads", sio_snapshot_read(good, n));
    sio_snapshot_write(again);
    check("good state round-trips byte for byte", !memcmp(again, good, n));

    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        memcpy(bad, good, n);
        bad[cases[k].at] = cases[k].val;
        check(cases[k].name, !sio_snapshot_validate(bad, n) && !sio_snapshot_read(bad, n));
        sio_snapshot_write(again);
        check("refused state leaves the SIO unchanged", !memcmp(again, good, n));
    }

    check("refuses length + 1", !sio_snapshot_validate(good, n + 1u));
    check("refuses length 0", !sio_snapshot_validate(good, 0));
    check("refuses NULL", !sio_snapshot_validate(NULL, n));
    check("refuses length n - 1", !sio_snapshot_read(good, n - 1u));
    sio_snapshot_write(again);
    check("refused lengths leave the SIO unchanged", !memcmp(again, good, n));

    /* Layout without the multitap block: its last byte is the hold flag. */
    memcpy(bad, good, n);
    bad[n - mtap - 1u] = 2;
    check("legacy layout refuses hold flag 2", !sio_snapshot_read(bad, n - mtap));
    /* A legacy bulk response (len > 8) is aborted on load, not fed truncated. */
    memcpy(bad, good, n);
    bad[resp_len_at] = PAD_RESPONSE_MAX;
    check("legacy layout loads a bulk response length",
          sio_snapshot_read(bad, n - mtap));
    sio_snapshot_write(again);
    check("legacy bulk response length is aborted to 0", again[resp_len_at] == 0);

    free(good); free(bad); free(again);
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
