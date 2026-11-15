/* DMA savestate load with fields out of range (MEM-24, MEM-3).
 * Complements check_dma_snapshot_ranges in test_psx_ram_runtime_map.c (CD-ROM
 * async cursors): this covers all three async channels, the GPU list phase and
 * payload cursor, wrong lengths, and a byte-exact round trip of a non-zero
 * state. Fields the load accepts without a range check are reported as KNOWN
 * lines and never fail the test. Same link recipe as psx_ram_runtime_map_test.
 */
#include "psx_memory.h"
#include "mod_plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t *p);
extern int      dma_snapshot_validate(const uint8_t *p, uint32_t len);
extern int      dma_snapshot_read(const uint8_t *p, uint32_t len);

/* Wire layout (dma.c): 7 channels x 12, DPCR, DICR, 3 async x 18, GPU list 44. */
enum {
    CH_BASE = 0, ASYNC0 = 92, ASYNC_SZ = 18, GPU_LL = 92 + 3 * 18,
    A_TOTAL = 2, A_REMAIN = 6, A_START = 14,
    L_PHASE = 1, L_START = 4, L_CUR = 8, L_NEXT = 12, L_WORDS = 16, L_PAYLOAD = 20,
    L_MAXNODES = 32
};

static int checks, failures, known;

static void check(const char *name, int ok) {
    checks++; if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *good, *bad, *after;
static uint32_t n;

/* Corrupt one 32-bit field; the load must refuse it and change nothing. */
static void refuse32(const char *name, uint32_t at, uint32_t v) {
    memcpy(bad, good, n);
    put32(bad + at, v);
    check(name, !dma_snapshot_validate(bad, n) && !dma_snapshot_read(bad, n));
    dma_snapshot_write(after);
    check("refused state leaves the DMA unchanged", !memcmp(after, good, n));
}

/* Corrupt one 32-bit field the load does not range-check: report, never fail. */
static void known32(const char *field, uint32_t at, uint32_t v) {
    memcpy(bad, good, n);
    put32(bad + at, v);
    if (dma_snapshot_validate(bad, n)) {
        known++;
        printf("KNOWN dma_snapshot_validate accepts %s = 0x%08X unchecked\n", field, v);
    }
    /* Back to the good state so later cases start from it. */
    dma_snapshot_read(good, n);
}

int main(void) {
    static const char *const asyncs[3] = { "mdec_in", "mdec_out", "cdrom" };
    n = dma_snapshot_bytes();
    good = malloc(n); bad = malloc(n); after = malloc(n);

    psx_ram_reset_size_request();
    psx_ram_apply_size_request();

    /* Non-zero good state: in-RAM word-aligned starts, remaining <= total,
     * GPU list idle (no workspace callbacks), payload cursor inside the node. */
    dma_snapshot_write(good);
    for (int i = 0; i < 3; i++) {
        put32(good + ASYNC0 + i * ASYNC_SZ + A_TOTAL, 64u + (uint32_t)i);
        put32(good + ASYNC0 + i * ASYNC_SZ + A_REMAIN, 32u + (uint32_t)i);
        put32(good + ASYNC0 + i * ASYNC_SZ + A_START, 0x1000u + 0x100u * (uint32_t)i);
    }
    put32(good + GPU_LL + L_WORDS, 8u);
    put32(good + GPU_LL + L_PAYLOAD, 3u);
    for (uint32_t i = 0; i < 7u * 12u; i++) good[CH_BASE + i] = (uint8_t)(0x11u * (i + 1u));
    check("non-zero state validates", dma_snapshot_validate(good, n));
    check("non-zero state loads", dma_snapshot_read(good, n));
    dma_snapshot_write(after);
    check("non-zero state round-trips byte for byte", !memcmp(after, good, n));

    for (int i = 0; i < 3; i++) {
        const uint32_t base = ASYNC0 + (uint32_t)i * ASYNC_SZ;
        char label[64];
        snprintf(label, sizeof label, "refuses %s start past RAM", asyncs[i]);
        refuse32(label, base + A_START, 0xFFFFFFFCu);
        snprintf(label, sizeof label, "refuses %s unaligned start", asyncs[i]);
        refuse32(label, base + A_START, 0x1002u);
        snprintf(label, sizeof label, "refuses %s remaining above total", asyncs[i]);
        refuse32(label, base + A_REMAIN, 1000u);
    }

    check("refuses length + 1", !dma_snapshot_validate(good, n + 1u));
    check("refuses length n - 1", !dma_snapshot_read(good, n - 1u));
    check("refuses length 0", !dma_snapshot_validate(good, 0));
    check("refuses NULL", !dma_snapshot_validate(NULL, n));
    dma_snapshot_write(after);
    check("refused lengths leave the DMA unchanged", !memcmp(after, good, n));

    /* GPU list phase is one byte, payload cursor a word. */
    memcpy(bad, good, n);
    bad[GPU_LL + L_PHASE] = 0xFF;
    check("refuses GPU list phase 0xFF", !dma_snapshot_validate(bad, n) && !dma_snapshot_read(bad, n));
    refuse32("refuses GPU payload_index above word_count", GPU_LL + L_PAYLOAD, 9u);
    /* The walk writes max_nodes 0 or 0x40000 and word_count header >> 24. */
    refuse32("refuses GPU max_nodes above the walk limit", GPU_LL + L_MAXNODES, 0x40001u);
    refuse32("refuses GPU max_nodes 0xFFFFFFFF", GPU_LL + L_MAXNODES, 0xFFFFFFFFu);
    refuse32("refuses GPU word_count above a header's 8 bits", GPU_LL + L_WORDS, 0x100u);
    memcpy(bad, good, n);
    put32(bad + GPU_LL + L_MAXNODES, 0x40000u);
    put32(bad + GPU_LL + L_WORDS, 0xFFu);
    check("accepts GPU max_nodes 0x40000 and word_count 0xFF", dma_snapshot_validate(bad, n));

    /* Accepted without a range check: masked or resolved where used. */
    known32("gpu_linked_list.start_addr", GPU_LL + L_START, 0xFFFFFFFCu);
    known32("gpu_linked_list.current_addr", GPU_LL + L_CUR, 0xFFFFFFFCu);
    known32("gpu_linked_list.next_addr", GPU_LL + L_NEXT, 0xFFFFFFFCu);
    known32("channels[0].madr", CH_BASE, 0xFFFFFFFFu);
    known32("cdrom_async.total_words", ASYNC0 + 2 * ASYNC_SZ + A_TOTAL, 0xFFFFFFFFu);
    printf("%d KNOWN unchecked field(s)\n", known);

    /* MEM-13: MADR reads back 24 bits; block mode takes BS 0 as 10000h. */
    {
        extern void     dma_write(uint32_t addr, uint32_t val);
        extern uint32_t dma_read(uint32_t addr);
        dma_write(0x1F8010A0u, 0x80001000u);
        check("MADR bits 24-31 read 0", dma_read(0x1F8010A0u) == 0x00001000u);
        dma_write(0x1F8010F0u, 0x00000800u);           /* DPCR: enable ch2 */
        dma_write(0x1F8010A4u, 0x00010000u);           /* BS 0, BA 1 */
        dma_write(0x1F8010A8u, 0x01000201u);           /* RAM -> GPU, block mode, start */
        check("block mode BS 0 moves 10000h words",
              dma_read(0x1F8010A0u) == 0x00001000u + 0x10000u * 4u);
    }

    free(good); free(bad); free(after);
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
