/* CDS-1: a command that does not move the drive must not drop a SeekL/SeekP
 * in flight (SEEK stuck, INT2 lost), while a stale Pause INT2 is still
 * cancelled by the next command (MOHU DsRead case in exec_command). */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static int seek_failures;
#define SEEK_CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++seek_failures; \
} } while (0)

static void clear_irq(void) { response_clear(); ack(); }
static void run_past_deadline(void) { psx_cycle_count += 100000000; process_pending(0); }

static void setloc(uint8_t m, uint8_t s, uint8_t f) {
    param_fifo[0] = m; param_fifo[1] = s; param_fifo[2] = f; param_count = 3;
    exec_command(0x02);
}

int main(void) {
    const uint8_t seeks[] = {0x15, 0x16};
    const uint8_t neutral[] = {0x01, 0x0C, 0x0E, 0x10, 0x11, 0x13};
    for (unsigned s = 0; s < sizeof seeks; ++s) {
        for (unsigned n = 0; n < sizeof neutral; ++n) {
            setup();
            exec_command(seeks[s]);
            SEEK_CHECK(irq_flag == CDIRQ_ACK && (stat_reg & CDSTAT_SEEK));
            clear_irq();
            if (neutral[n] == 0x0E) { param_fifo[0] = 0x80; param_count = 1; }
            exec_command(neutral[n]);   /* lands before the seek deadline */
            /* GetlocL answers INT5 80h while seeking (psx-spx); the seek still lands. */
            SEEK_CHECK(irq_flag == (neutral[n] == 0x10 ? CDIRQ_ERROR : CDIRQ_ACK));
            if (neutral[n] == 0x01) SEEK_CHECK(response_fifo[0] & CDSTAT_SEEK);
            clear_irq();
            run_past_deadline();
            SEEK_CHECK(irq_flag == CDIRQ_COMPLETE);
            SEEK_CHECK(!(stat_reg & CDSTAT_SEEK) && (stat_reg & CDSTAT_READ));
            SEEK_CHECK(!pending.pending);
        }

        /* A Setloc accepted mid-seek keeps its target pending for the read. */
        setup();
        exec_command(seeks[s]);
        clear_irq();
        setloc(0x00, 0x04, 0x00);
        const int target = s_setloc_lba;
        clear_irq();
        run_past_deadline();
        SEEK_CHECK(irq_flag == CDIRQ_COMPLETE && !(stat_reg & CDSTAT_SEEK));
        SEEK_CHECK(setloc_pending && s_setloc_lba == target);

        /* Setloc before the seek is consumed by it, as before. */
        setup();
        setloc(0x00, 0x04, 0x00);
        clear_irq();
        exec_command(seeks[s]);
        clear_irq();
        run_past_deadline();
        SEEK_CHECK(irq_flag == CDIRQ_COMPLETE && !setloc_pending);

        /* A read moves the drive: it still supersedes the seek. */
        setup();
        exec_command(seeks[s]);
        clear_irq();
        exec_command(0x06);
        SEEK_CHECK(!pending.pending && !(stat_reg & CDSTAT_SEEK));
    }

    /* Stale Pause: Setmode/Setloc/GetStat still cancel its INT2. */
    const uint8_t after_pause[] = {0x01, 0x02, 0x0E};
    for (unsigned n = 0; n < sizeof after_pause; ++n) {
        setup();
        exec_command(0x09);
        clear_irq();
        if (after_pause[n] == 0x02) setloc(0x00, 0x04, 0x00);
        else {
            if (after_pause[n] == 0x0E) { param_fifo[0] = 0x80; param_count = 1; }
            exec_command(after_pause[n]);
        }
        SEEK_CHECK(!pending.pending);
        clear_irq();
        run_past_deadline();
        SEEK_CHECK(irq_flag == 0);
    }

    if (seek_failures) return 1;
    puts("PASS: seek survives drive-neutral commands; stale Pause INT2 still cancelled");
    return 0;
}
