/* Live main-RAM geometry, end to end, through the REAL runtime paths.
 *
 * Links the runtime's own memory.c (guest loads/stores, MMIO routing),
 * dma.c (channel 6 OTC, a DMA that writes RAM from a MADR the guest chose),
 * boot_state.c (savestate save/load) and psx_ram_geometry.c (the state machine
 * memory_init() drives). Everything else those files call is a counted stub in
 * psx_ram_runtime_map_stubs.c. Both geometries are exercised in one process,
 * exactly as a launch with and without the 8 MB mod would set them.
 *
 * Load-bearing claims:
 *  1. Retail (mod off): every KUSEG/KSEG0/KSEG1 alias and every 2nd-4th RAM
 *     mirror of an address reaches the same DRAM byte, for word/half/byte
 *     loads and stores; the stack in the 4th mirror (Kula World) works.
 *  2. Expanded (mod on): the whole 8 MiB window decodes uniquely.
 *  3. A DMA cursor folds exactly like the CPU in each geometry.
 *  4. A savestate records the live size; a state from the other geometry is
 *     REJECTED BEFORE ANY MUTATION (CPU, RAM, devices untouched), and a state
 *     whose header is compatible but whose RAM section is the wrong size is
 *     refused by the two-pass loader with zero mutation too.
 *  5. The geometry helpers agree with hardware on every page of the window. */
#include "psx_memory.h"
#include "mod_plugins.h"
#include "boot_state.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);
extern void     psx_write_half(uint32_t addr, uint16_t val);
extern void     psx_write_byte(uint32_t addr, uint8_t val);
extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t *p);
extern int      dma_snapshot_validate(const uint8_t *p, uint32_t len);
extern int      dma_snapshot_read(const uint8_t *p, uint32_t len);

/* Stub-side mutation counters (psx_ram_runtime_map_stubs.c). */
extern int g_stub_device_restores;

static int failures;

static int check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
        return 0;
    }
    return 1;
}

static void set_geometry(int expanded) {
    psx_ram_reset_size_request();
    if (expanded) psx_mod_set_main_ram_8mb(1);
    psx_ram_apply_size_request();
}

/* ---- 5. helpers vs hardware on every page ---------------------------- */
static void check_helpers_everywhere(int expanded, const char *phase) {
    static const uint32_t segs[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    static const uint32_t offs[3] = { 0x000u, 0x7F0u, 0xFFCu };
    const uint32_t mask = expanded ? 0x007FFFFFu : 0x001FFFFFu;
    uint32_t page, s, o, bad = 0;
    for (page = 0; page < (PSX_MAIN_RAM_WINDOW_BYTES >> 12); page++) {
        for (o = 0; o < 3; o++) {
            uint32_t phys = (page << 12) | offs[o];
            uint32_t want = phys & mask;
            uint32_t off = 0xFFFFFFFFu;
            if (psx_ram_map_read(phys) != want) bad++;
            if (psx_ram_map_write(phys) != want) bad++;
            for (s = 0; s < 3; s++) {
                uint32_t va = segs[s] | phys;
                if (psx_ram_canonical_offset(va) != want) bad++;
                if (!psx_ram_resolve(va, 4u, &off) || off != want) bad++;
            }
        }
    }
    if (bad) fprintf(stderr, "  %s: %u geometry violations\n", phase, bad);
    check(bad == 0u, phase);
    check(memory_get_ram_bytes() == (expanded ? PSX_MAIN_RAM_EXPANDED_BYTES
                                              : PSX_MAIN_RAM_RETAIL_BYTES), phase);
    check(psx_ram_8mb_active() == expanded, phase);
    /* Addresses outside the window are not RAM, whatever the geometry. */
    check(psx_ram_map_read(0x1FC00000u) == 0x1FC00000u, "BIOS ROM is not folded");
    check(!psx_ram_resolve(0x1F801000u, 4u, 0), "MMIO never resolves to RAM");
    check(!psx_ram_resolve(0x00800000u, 4u, 0), "first byte past the window");
    /* Retail: a span crossing a 2 MiB mirror boundary has no contiguous bytes. */
    check(psx_ram_resolve(0x801FFFFEu, 4u, 0) == expanded,
          "span across the 2 MiB end resolves only in the 8 MB map");
    check(psx_ram_resolve(0x803FFFFEu, 4u, 0) == expanded,
          "span across a mirror boundary resolves only in the 8 MB map");
}

/* ---- 1/2. real guest loads and stores -------------------------------- */
static void check_guest_access(int expanded) {
    uint8_t *ram = memory_get_ram_ptr();
    memset(ram, 0, PSX_MAIN_RAM_BACKING_BYTES);
    /* Word store through KSEG0, visible through every alias of its bytes. */
    psx_write_word(0x80001000u, 0x11223344u);
    check(ram[0x1000] == 0x44 && ram[0x1003] == 0x11, "word store lands in DRAM");
    check(psx_read_word(0x00001000u) == 0x11223344u, "KUSEG alias reads it");
    check(psx_read_word(0xA0001000u) == 0x11223344u, "KSEG1 alias reads it");
    for (uint32_t m = 1; m < 4; ++m) {
        uint32_t mirror = 0x80001000u + m * 0x00200000u;
        char label[96];
        snprintf(label, sizeof label, "%s: mirror %u of a word",
                 expanded ? "8 MB" : "retail", m);
        check((psx_read_word(mirror) == 0x11223344u) == !expanded, label);
    }
    /* A store to a high mirror address. */
    psx_write_word(0x80601000u, 0xCAFEF00Du);
    if (expanded) {
        check(ram[0x601000] == 0x0D && psx_read_word(0x80001000u) == 0x11223344u,
              "8 MB: a high-bank store is unique");
    } else {
        check(psx_read_word(0x80001000u) == 0xCAFEF00Du && ram[0x601000] == 0,
              "retail: a 4th-mirror store folds onto the low 2 MiB");
    }
    /* Kula World parks $sp in the 4th mirror: push/pop through it. */
    psx_write_word(0x807FFFF8u, 0x80012344u);
    check(psx_read_word(0x807FFFF8u) == 0x80012344u, "stack slot round-trips");
    check((psx_read_word(0x801FFFF8u) == 0x80012344u) == !expanded,
          "4th-mirror stack aliases the 2 MiB top only on retail RAM");
    /* Half and byte widths fold the same way. */
    psx_write_half(0x80402002u, 0xBEEFu);
    check((psx_read_half(0x80002002u) == 0xBEEFu) == !expanded, "half mirror");
    psx_write_byte(0x80204005u, 0x5Au);
    check((psx_read_byte(0x80004005u) == 0x5Au) == !expanded, "byte mirror");
    check(psx_read_byte(0x80204005u) == 0x5Au, "byte reads back at its own address");
}

/* ---- psx_peek_word: debug read that must never reach the bus ----------- */
static void check_peek(int expanded) {
    uint32_t v = 0xDEADBEEFu;
    memset(memory_get_ram_ptr(), 0, PSX_MAIN_RAM_BACKING_BYTES);
    psx_write_word(0x80001000u, 0x11223344u);
    check(psx_peek_word(0x80001000u, &v) && v == 0x11223344u, "peek: KSEG0 RAM");
    check(psx_peek_word(0x00001000u, &v) && v == 0x11223344u, "peek: KUSEG RAM");
    check(psx_peek_word(0xA0001000u, &v) && v == 0x11223344u, "peek: KSEG1 RAM");
    v = 0;
    check(psx_peek_word(0x80201000u, &v) && ((v == 0x11223344u) == !expanded),
          "peek: 2nd mirror follows the live geometry");
    check(psx_peek_word(0x807FFFFCu, &v), "peek: top of the RAM window");
    memory_get_scratchpad_ptr()[0x10] = 0x78; memory_get_scratchpad_ptr()[0x13] = 0x12;
    check(psx_peek_word(0x1F800010u, &v) && (v & 0xFF) == 0x78 && (v >> 24) == 0x12,
          "peek: scratchpad");
    check(psx_peek_word(0x1F8003FCu, &v) && !psx_peek_word(0x1F800400u, &v),
          "peek: scratchpad end bound");
    check(psx_peek_word(0xBFC00000u, &v) && psx_peek_word(0x1FC7FFFCu, &v) &&
          !psx_peek_word(0x1FC80000u, &v), "peek: BIOS ROM bounds");
    /* Device / unmapped space is refused WITHOUT touching the bus (the stubbed
     * psx_fatal_halt traps if a real access reached unmapped_fatal). */
    v = 0xDEADBEEFu;
    check(!psx_peek_word(0x1F801044u, &v), "peek: JOY_RX refused");
    check(!psx_peek_word(0x1F801810u, &v), "peek: GPUREAD refused");
    check(!psx_peek_word(0x1F802000u, &v), "peek: expansion-2 MMIO refused");
    check(!psx_peek_word(0x1F000000u, &v), "peek: expansion-1 refused");
    check(!psx_peek_word(0x80800000u, &v), "peek: past the RAM window refused");
    check(!psx_peek_word(0xFFFE0130u, &v) && !psx_peek_word(0xC0000000u, &v),
          "peek: KSEG2 refused");
    check(!psx_peek_word(0x80001002u, &v), "peek: unaligned refused");
    check(v == 0xDEADBEEFu, "peek: refused reads leave *out untouched");
}

/* ---- 3. a real DMA writing RAM from a guest-chosen MADR --------------- */
#define DMA_BASE 0x1F801080u
static void run_otc(uint32_t madr, uint32_t entries) {
    psx_write_word(0x1F8010F0u, 0x0FEDCBA9u | (1u << 27));  /* DPCR: ch6 enabled */
    psx_write_word(DMA_BASE + 6u * 16u + 0u, madr);          /* MADR */
    psx_write_word(DMA_BASE + 6u * 16u + 4u, entries);       /* BCR */
    psx_write_word(DMA_BASE + 6u * 16u + 8u, 0x11000002u);   /* CHCR: start, trigger, step -4 */
}

static void check_dma(int expanded) {
    uint8_t *ram = memory_get_ram_ptr();
    memset(ram, 0, PSX_MAIN_RAM_BACKING_BYTES);
    /* OTC writes a backwards linked list ending in the 0x00FFFFFF terminator,
     * from MADR down. MADR in the 4th mirror. */
    run_otc(0x80601010u, 4u);
    uint32_t w_hi_low, w_hi_high;
    memcpy(&w_hi_low, ram + 0x001010u, 4);
    memcpy(&w_hi_high, ram + 0x601010u, 4);
    if (expanded) {
        check(w_hi_high == 0x0060100Cu && w_hi_low == 0u,
              "8 MB: DMA cursor addresses the unique high bank");
    } else {
        check(w_hi_low == 0x0000100Cu && w_hi_high == 0u,
              "retail: DMA cursor folds exactly like the DMAC (0x1FFFFC)");
    }
    uint32_t term;
    memcpy(&term, ram + (expanded ? 0x601004u : 0x001004u), 4);
    check(term == 0x00FFFFFFu, "OTC terminator written at the folded tail");
}

/* ---- 4. savestates ---------------------------------------------------- */
static void fill_cpu(CPUState *cpu, uint32_t salt) {
    memset(cpu, 0, sizeof *cpu);
    for (int i = 0; i < 32; i++) cpu->gpr[i] = 0x1000u * (uint32_t)i + salt;
    cpu->pc = 0x80010000u + salt;
}

static int cpu_equal(const CPUState *a, const CPUState *b) {
    return memcmp(a->gpr, b->gpr, sizeof a->gpr) == 0 && a->pc == b->pc &&
           a->hi == b->hi && a->lo == b->lo &&
           memcmp(a->cop0, b->cop0, sizeof a->cop0) == 0;
}

static uint8_t *save_state(size_t *len, uint32_t salt) {
    CPUState cpu;
    uint8_t *data = NULL;
    fill_cpu(&cpu, salt);
    memset(memory_get_ram_ptr(), (int)(salt & 0xFF), memory_get_ram_bytes());
    if (!boot_state_save_buffer(&cpu, 0x12345678u, 0x80010000u, &data, len))
        return NULL;
    return data;
}

/* Load `state` and require that it is refused with no observable mutation. */
static void expect_refused_untouched(const uint8_t *state, size_t len,
                                     const char *label, const char *reason_part) {
    CPUState live, before;
    uint8_t *ram = memory_get_ram_ptr();
    uint8_t *ram_before = (uint8_t *)malloc(PSX_MAIN_RAM_BACKING_BYTES);
    char reason[256];
    int restores_before = g_stub_device_restores;
    fill_cpu(&live, 0x777u);
    before = live;
    memset(ram, 0xC3, PSX_MAIN_RAM_BACKING_BYTES);
    memcpy(ram_before, ram, PSX_MAIN_RAM_BACKING_BYTES);
    check(!boot_state_load_buffer(state, len, 0x12345678u, 0x80010000u, &live), label);
    check(cpu_equal(&live, &before), label);
    check(memcmp(ram, ram_before, PSX_MAIN_RAM_BACKING_BYTES) == 0, label);
    check(g_stub_device_restores == restores_before, label);
    if (reason_part) {
        check(!boot_state_check_buffer(state, len, 0x12345678u, 0x80010000u,
                                       reason, sizeof reason) &&
                  strstr(reason, reason_part) != NULL,
              label);
    }
    free(ram_before);
}

static void check_savestates(void) {
    size_t len2 = 0, len8 = 0;
    uint8_t *retail, *expanded;
    CPUState cpu, want;

    set_geometry(0);
    retail = save_state(&len2, 0x21u);
    set_geometry(1);
    expanded = save_state(&len8, 0x81u);
    check(retail && expanded, "both geometries save");
    if (!retail || !expanded) return;

    /* 8 MB runtime refuses the retail state, and vice versa, untouched. */
    expect_refused_untouched(retail, len2, "8 MB runtime refuses a 2 MiB state",
                             "main_ram=2MiB(want 8MiB)");
    set_geometry(0);
    expect_refused_untouched(expanded, len8, "retail runtime refuses an 8 MiB state",
                             "main_ram=8MiB(want 2MiB)");

    /* Two-pass: a header forged to look compatible (the RAM cookie cleared)
     * still carries an 8 MiB RAM section AFTER the CPU section. The single-pass
     * loader restored the CPU before rejecting RAM; now nothing is applied. */
    {
        uint8_t *forged = (uint8_t *)malloc(len8);
        uint32_t cookie;
        memcpy(forged, expanded, len8);
        memcpy(&cookie, forged + 32, 4);            /* header.reserved */
        cookie ^= 0x384D4252u;                      /* drop the 8 MiB term */
        memcpy(forged + 32, &cookie, 4);
        check(boot_state_check_buffer(forged, len8, 0x12345678u, 0x80010000u,
                                      NULL, 0),
              "forged header passes the compatibility check");
        expect_refused_untouched(forged, len8,
                                 "wrong-size RAM section refused before the CPU applies",
                                 NULL);
        /* A truncated tail (last section cut) is refused untouched as well. */
        expect_refused_untouched(retail, len2 - 64u,
                                 "truncated state refused before anything applies", NULL);
        free(forged);
    }

    /* The matching geometry loads, and restores what was saved. */
    fill_cpu(&want, 0x21u);
    fill_cpu(&cpu, 0x999u);
    memset(memory_get_ram_ptr(), 0, PSX_MAIN_RAM_BACKING_BYTES);
    check(boot_state_load_buffer(retail, len2, 0x12345678u, 0x80010000u, &cpu),
          "retail state loads on a retail runtime");
    check(cpu_equal(&cpu, &want), "retail load restores the CPU");
    check(memory_get_ram_ptr()[0x1FFFFF] == 0x21 &&
              memory_get_ram_ptr()[0x200000] == 0,
          "retail load restores exactly 2 MiB of RAM");

    set_geometry(1);
    fill_cpu(&want, 0x81u);
    check(boot_state_load_buffer(expanded, len8, 0x12345678u, 0x80010000u, &cpu),
          "8 MiB state loads on an 8 MB runtime");
    check(cpu_equal(&cpu, &want), "8 MB load restores the CPU");
    check(memory_get_ram_ptr()[0x7FFFFF] == 0x81, "8 MB load restores all 8 MiB");
    free(retail);
    free(expanded);
}

/* ---- 6. DMA section: async cursors are range-checked in pass 1 ---------- */
/* Wire offsets of the CD-ROM async channel (dma.c dma_w_async): 7 channels x
 * 12 + DPCR + DICR = 92, two MDEC records of 18, then total/remaining/start. */
enum { CD_ASYNC = 92 + 2 * 18, CD_TOTAL = CD_ASYNC + 2, CD_REMAIN = CD_ASYNC + 6,
       CD_START = CD_ASYNC + 14 };

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void dma_expect(int expanded, uint32_t start, uint32_t total, uint32_t remain,
                       int want_ok, const char *label) {
    uint32_t n = dma_snapshot_bytes();
    uint8_t *good = (uint8_t *)malloc(n), *bad = (uint8_t *)malloc(n);
    uint8_t *after = (uint8_t *)malloc(n);
    set_geometry(expanded);
    dma_snapshot_write(good);
    memcpy(bad, good, n);
    put32(bad + CD_START, start);
    put32(bad + CD_TOTAL, total);
    put32(bad + CD_REMAIN, remain);
    check(dma_snapshot_validate(bad, n) == want_ok, label);
    if (!want_ok) {
        check(!dma_snapshot_read(bad, n), label);
        dma_snapshot_write(after);
        check(memcmp(after, good, n) == 0, label); /* refused read changed nothing */
    }
    free(good); free(bad); free(after);
}

static void check_dma_snapshot_ranges(void) {
    dma_expect(0, 0x001FFFF0u, 4, 4, 1, "DMA state: retail start in RAM validates");
    dma_expect(0, 0x00200000u, 4, 4, 0, "DMA state: retail start past 2 MiB refused");
    dma_expect(0, 0xFFFFFFFCu, 4, 4, 0, "DMA state: wild start refused");
    dma_expect(0, 0x00000102u, 4, 4, 0, "DMA state: unaligned start refused");
    dma_expect(0, 0x00001000u, 4, 5, 0, "DMA state: remaining above total refused");
    dma_expect(1, 0x00200000u, 4, 4, 1, "DMA state: 8 MiB start above 2 MiB validates");
    dma_expect(1, 0x00800000u, 4, 4, 0, "DMA state: 8 MiB start past 8 MiB refused");
    set_geometry(0);
}

/* EST-3: a .pst save replaces the old file in one step instead of writing
 * over it. The target is a hard link to a second name: a writer that opens the
 * target "wb" truncates and rewrites the shared bytes, so the second name
 * changes; a temporary + rename only repoints the target name. */
static void check_state_file_replaced_whole(void) {
    static const char old_bytes[] = "OLD-PLAYER-STATE";
    const char *path = "ram_map_atomic.pst";
    const char *other = "ram_map_atomic_other.pst";
    char got[sizeof old_bytes] = {0};
    uint8_t *saved;
    long n;
    CPUState cpu;
    FILE *f;

    remove(path);
    remove(other);
    f = fopen(other, "wb");
    if (!check(f != NULL, "state file test: create the old save")) return;
    fwrite(old_bytes, 1, sizeof old_bytes, f);
    fclose(f);
#ifdef _WIN32
    if (!check(CreateHardLinkA(path, other, NULL) != 0, "state file test: hard link")) return;
#else
    if (!check(link(other, path) == 0, "state file test: hard link")) return;
#endif
    set_geometry(0);
    fill_cpu(&cpu, 0x5Au);
    check(boot_state_save(&cpu, 0x12345678u, 0x80010000u, path) == 1,
          "state file test: save succeeds");

    f = fopen(other, "rb");
    if (check(f != NULL, "state file test: other name still readable")) {
        (void)fread(got, 1, sizeof got, f);
        fclose(f);
    }
    check(memcmp(got, old_bytes, sizeof old_bytes) == 0,
          "EST-3: save replaces the file whole (old bytes not overwritten in place)");

    f = fopen(path, "rb");
    if (check(f != NULL, "state file test: new save readable")) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        saved = (uint8_t *)malloc((size_t)n);
        check(saved && fread(saved, 1, (size_t)n, f) == (size_t)n, "state file test: read new save");
        fclose(f);
        check(saved && boot_state_check_buffer(saved, (size_t)n, 0x12345678u, 0x80010000u, NULL, 0),
              "state file test: new save is a valid state");
        free(saved);
    }
    remove(path);
    remove(other);
}

int main(void) {
    /* Process start and memory_init() without the mod: retail. */
    check_helpers_everywhere(0, "default geometry is retail 2 MiB mirroring");
    set_geometry(0);
    check_helpers_everywhere(0, "memory_init without the mod stays retail");
    /* The request is inert until memory_init() applies it. */
    psx_ram_reset_size_request();
    check(psx_mod_set_main_ram_8mb(1) == 1, "8 MB request accepted");
    check_helpers_everywhere(0, "8 MB request is inert before memory_init");
    psx_ram_apply_size_request();
    check_helpers_everywhere(1, "8 MB request applies unique 8 MiB decode");
    /* A later boot whose plan drops the mod returns to retail mirroring. */
    set_geometry(0);
    check_helpers_everywhere(0, "reboot without the mod restores retail");

    for (int expanded = 0; expanded < 2; ++expanded) {
        set_geometry(expanded);
        check_guest_access(expanded);
        check_peek(expanded);
        check_dma(expanded);
    }
    check_savestates();
    check_dma_snapshot_ranges();
    check_state_file_replaced_whole();

    if (failures == 0) printf("PASS  psx_ram_runtime_map (2 and 8 MiB, real memory/DMA/savestate paths)\n");
    return failures ? 1 : 0;
}
