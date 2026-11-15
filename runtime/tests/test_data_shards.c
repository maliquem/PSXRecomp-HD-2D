/*
 * data_shards.c unit test: capture -> replay round trip, purity poisons, budget
 * and edge inputs, and (Windows only, where the disk load exists) the .dss file
 * loader against valid, truncated and corrupted files (audit OVL-9 / OVL-20).
 *
 * data_shards.c is linked as-is; the memory, cycle and interrupt services it
 * calls are tiny fakes below. Disk files live in ./ds_test_cache and are removed
 * at start and end. Build/run: ctest -R data_shards_test
 */
#include "data_shards.h"
#include "psx_memory.h"
#include "interrupts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif

/* ---- fakes for the symbols data_shards.c needs ---- */
uint32_t g_psx_ram_size = PSX_MAIN_RAM_EXPANDED_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_EXPANDED_BYTES - 1u;
uint32_t g_psx_mem_gate = 0;
uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
int      psx_in_device_service = 1;   /* inline psx_advance_cycles then only adds to the counter */
int      g_event_step_conservative = 0;
int      g_ls_replay_active = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
void psx_advance_cycles_slow(uint32_t c) { psx_cycle_count += c; }
void psx_devices_service_to_now(void) { }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }

static unsigned irq_checks;
static uint32_t irq_last_pc;
void psx_check_interrupts_at(struct CPUState *cpu, uint32_t resume_pc) {
    (void)cpu; irq_checks++; irq_last_pc = resume_pc;
}

static uint8_t *g_ram;
static uint8_t g_sp[1024];
uint8_t *memory_get_ram_ptr(void) { return g_ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return g_sp; }

void psx_write_byte(uint32_t va, uint8_t v) {
    uint32_t phys = va & 0x1FFFFFFFu;
    if (phys >= 0x1F800000u && phys < 0x1F800000u + 1024u) g_sp[phys - 0x1F800000u] = v;
    else g_ram[phys & (PSX_MAIN_RAM_WINDOW_BYTES - 1u)] = v;
}
static unsigned exec_marks;
void dirty_ram_mark_executable_range(uint32_t addr, uint32_t len) { (void)addr; (void)len; exec_marks++; }

/* ---- harness ---- */
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define DS_SPACE_T (PSX_MAIN_RAM_BACKING_BYTES + 1024u)

static uint64_t ds_stat(const char *name) {
    char buf[1024], pat[64];
    ds_stats_json(buf, sizeof(buf));
    snprintf(pat, sizeof(pat), "\"%s\":", name);
    const char *p = strstr(buf, pat);
    if (!p) return (uint64_t)-1;
    return strtoull(p + strlen(pat), NULL, 10);
}

static CPUState cpu;
#define SP_VAL 0x801FFF00u
#define RA_VAL 0x80123450u
static void begin_regs(uint32_t a0) {
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[4] = a0; cpu.gpr[29] = SP_VAL; cpu.gpr[31] = RA_VAL;
}
/* Abort an armed capture the way a DMA write would, then close the window. */
static void finish_poisoned(void) {
    ds_note_dma_write();
    psx_datashard_ret(&cpu);
}

#ifdef _WIN32
#define CACHE_ROOT "ds_test_cache"
#define CACHE_DIR  CACHE_ROOT "/ds_test_game/datashards"
static void clean_cache(void) {
    struct _finddata_t fd; intptr_t h = _findfirst(CACHE_DIR "/*.dss", &fd);
    if (h == -1) return;
    do { char p[512]; snprintf(p, sizeof(p), CACHE_DIR "/%s", fd.name); remove(p); }
    while (_findnext(h, &fd) == 0);
    _findclose(h);
}
static void remove_cache_dirs(void) {
    clean_cache();
    _rmdir(CACHE_ROOT "/ds_test_game/datashards");
    _rmdir(CACHE_ROOT "/ds_test_game");
    _rmdir(CACHE_ROOT);
}

typedef struct { uint32_t idx, len; } Rng;
/* Build a .dss: header (magic/key as given), args, regs, rd+wr ranges and bytes. Cuts `cut` trailing bytes. */
static void write_dss(uint32_t key, uint32_t magic, uint32_t hdr_key, uint32_t nrd, uint32_t nwr,
                      uint32_t rdtot, uint32_t wrtot, const Rng *rd, const uint8_t *rdb,
                      const Rng *wr, const uint8_t *wrb, uint32_t rdb_n, uint32_t wrb_n,
                      uint32_t cyc, uint32_t v0, uint32_t cut) {
    static uint8_t buf[4096];
    uint32_t n = 0;
    uint32_t hdr[8] = { magic, hdr_key, cyc, 0, nrd, nwr, rdtot, wrtot };
    uint32_t args[4] = { 0, 0, 0, 0 };
    uint32_t regs[19] = { 0 };
    regs[1] = v0;
    memcpy(buf + n, hdr, 32); n += 32;
    memcpy(buf + n, args, 16); n += 16;
    memcpy(buf + n, regs, 76); n += 76;
    if (nrd <= 8) { memcpy(buf + n, rd, nrd * 8); n += nrd * 8; }
    memcpy(buf + n, rdb, rdb_n); n += rdb_n;
    if (nwr <= 8) { memcpy(buf + n, wr, nwr * 8); n += nwr * 8; }
    memcpy(buf + n, wrb, wrb_n); n += wrb_n;
    char path[512];
    static unsigned serial;
    snprintf(path, sizeof(path), CACHE_DIR "/%08X_%08X.dss", key, 0xA0000000u + serial++);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(buf, 1, n - cut, f); fclose(f); }
}

/* Enter a key whose only candidate is a hand-made file; returns the enter() result. */
static int enter_key(uint32_t key) {
    begin_regs(0);
    return psx_datashard_enter(&cpu, key);
}
#endif

int main(void) {
    g_ram = (uint8_t *)calloc(1, PSX_MAIN_RAM_BACKING_BYTES);
    if (!g_ram) { printf("no memory\n"); return 1; }
#ifdef _WIN32
    remove_cache_dirs();
    ds_init(CACHE_ROOT, "ds_test_game");
#endif

    /* disabled by default: no hook activity */
    begin_regs(1);
    CHECK(psx_datashard_enter(&cpu, 0x80010000u) == 0);
    CHECK(ds_stat("enters") == 0 && g_ds_recording == 0 && ds_stat("shards") == 0);
    CHECK(ds_stat("enabled") == 0);
    ds_set_enabled(1);
    CHECK(ds_stat("enabled") == 1);

    /* ---- capture, then replay ---- */
    const uint32_t K = 0x80010000u;
    memset(g_ram, 0, 0x20000);
    g_ram[0x1000] = 0xA1; g_ram[0x1001] = 0xA2;          /* read-set input */
    g_ram[0x5000] = 0x11;                                /* written-before-read byte */
    begin_regs(7);
    CHECK(psx_datashard_enter(&cpu, K) == 0);            /* arms */
    CHECK(g_ds_recording == 1 && ds_stat("captures_armed") == 1);
    ds_note_read(0x80001000u, 2);
    ds_note_read(0xBFC00000u, 4);                        /* BIOS ROM: constant, ignored */
    CHECK(ds_stat("captures_poisoned") == 0);
    ds_note_write(0x80002FFEu, 4);                       /* spans a 4 KB page boundary */
    g_ram[0x2FFE] = 1; g_ram[0x2FFF] = 2; g_ram[0x3000] = 3; g_ram[0x3001] = 4;
    ds_note_write(0xA0010000u, 4);                       /* KSEG1 alias of RAM */
    g_ram[0x10000] = 9; g_ram[0x10003] = 8;
    ds_note_write(0x1F800010u, 4);                       /* scratchpad */
    g_sp[0x10] = 0x5A; g_sp[0x13] = 0x5B;
    ds_note_write(0x80005000u, 1);
    g_ram[0x5000] = 0x22;
    ds_note_read(0x80005000u, 1);                        /* read after own write: not an input */
    cpu.gpr[2] = 0x55;                                   /* v0 result */
    cpu.hi = 0x1234; cpu.lo = 0x5678;
    cpu.gpr[8] = 0xFEED;                                 /* t0 */
    psx_cycle_count += 5000;
    /* a callee's own jr $ra (different sp) must not close the window */
    cpu.gpr[29] = SP_VAL - 0x20;
    psx_datashard_ret(&cpu);
    CHECK(g_ds_recording == 1 && ds_stat("captures_ok") == 0);
    cpu.gpr[29] = SP_VAL;
    psx_datashard_ret(&cpu);
    CHECK(g_ds_recording == 0 && ds_stat("captures_ok") == 1 && ds_stat("shards") == 1);

#ifdef _WIN32
    {   /* the persisted file carries the documented header */
        char path[512]; struct _finddata_t fd;
        snprintf(path, sizeof(path), CACHE_DIR "/%08X_*.dss", K);
        intptr_t h = _findfirst(path, &fd);
        CHECK(h != -1);
        if (h != -1) {
            snprintf(path, sizeof(path), CACHE_DIR "/%s", fd.name);
            FILE *f = fopen(path, "rb"); uint32_t hdr[8] = {0};
            CHECK(f && fread(hdr, 4, 8, f) == 8);
            if (f) fclose(f);
            CHECK(hdr[0] == 0x31534450u && hdr[1] == K && hdr[2] == 5000u && hdr[4] == 1 && hdr[5] == 4);
            _findclose(h);
        }
    }
#endif

    /* replay: clobber outputs and the (written-first) byte, keep inputs */
    g_ram[0x2FFE] = g_ram[0x2FFF] = g_ram[0x3000] = g_ram[0x3001] = 0;
    g_ram[0x10000] = g_ram[0x10003] = 0; g_sp[0x10] = g_sp[0x13] = 0;
    g_ram[0x5000] = 0xEE;
    begin_regs(7);
    cpu.gpr[2] = 0; cpu.gpr[8] = 0;
    uint64_t c0 = psx_cycle_count; irq_checks = 0; exec_marks = 0;
    CHECK(psx_datashard_enter(&cpu, K) == 1);
    CHECK(g_ram[0x2FFE] == 1 && g_ram[0x2FFF] == 2 && g_ram[0x3000] == 3 && g_ram[0x3001] == 4);
    CHECK(g_ram[0x10000] == 9 && g_ram[0x10003] == 8 && g_sp[0x10] == 0x5A && g_sp[0x13] == 0x5B);
    CHECK(g_ram[0x5000] == 0x22);
    CHECK(cpu.gpr[2] == 0x55 && cpu.gpr[8] == 0xFEED && cpu.hi == 0x1234 && cpu.lo == 0x5678);
    CHECK(cpu.pc == RA_VAL);
    CHECK(psx_cycle_count - c0 == 5000);                  /* 2048 + 2048 + 904 */
    CHECK(irq_checks == 3 && irq_last_pc == RA_VAL);
    CHECK(exec_marks == 3);                               /* RAM ranges only, not scratchpad */
    CHECK(ds_stat("replays") == 1 && ds_stat("cyc_credited") == 5000 && g_ds_recording == 0);

    /* different a0: prefilter rejects, capture arms (no verify_fail: never verified) */
    begin_regs(8);
    CHECK(psx_datashard_enter(&cpu, K) == 0 && g_ds_recording == 1);
    CHECK(ds_stat("verify_fail") == 0);
    finish_poisoned();
    CHECK(g_ds_recording == 0 && ds_stat("captures_poisoned") == 1 && ds_stat("poison_dma") == 1);

    /* changed input byte: verify fails, capture arms */
    g_ram[0x1001] = 0x00;
    begin_regs(7);
    CHECK(psx_datashard_enter(&cpu, K) == 0 && ds_stat("verify_fail") == 1 && g_ds_recording == 1);
    finish_poisoned();
    g_ram[0x1001] = 0xA2;

    /* ---- poison kinds ---- */
    begin_regs(0);
    CHECK(psx_datashard_enter(&cpu, 0x80020000u) == 0);
    ds_note_mmio(0x1F801810u, 1);
    CHECK(ds_stat("poison_mmio") == 1);
    psx_datashard_ret(&cpu);
    CHECK(g_ds_recording == 0 && ds_stat("shards") == 1);    /* poisoned: nothing stored */

    CHECK(psx_datashard_enter(&cpu, 0x80020004u) == 0);
    ds_note_read(0x1F801070u, 4);                         /* MMIO address through the read feed */
    CHECK(ds_stat("poison_mmio") == 2);
    psx_datashard_ret(&cpu);
    CHECK(psx_datashard_enter(&cpu, 0x80020008u) == 0);
    ds_note_write(0x1F000000u, 4);                        /* unmapped device address: impure */
    CHECK(ds_stat("poison_mmio") == 3);
    psx_datashard_ret(&cpu);

    CHECK(psx_datashard_enter(&cpu, 0x8002000Cu) == 0);
    ds_note_thread_switch();
    CHECK(ds_stat("poison_thread") == 1);
    psx_datashard_ret(&cpu);
    ds_note_thread_switch();                              /* idle: no effect */
    CHECK(ds_stat("poison_thread") == 1);

    CHECK(psx_datashard_enter(&cpu, 0x80020010u) == 0);   /* nested re-entry of the same key */
    CHECK(psx_datashard_enter(&cpu, 0x80020010u) == 0);
    CHECK(ds_stat("poison_stale") == 1 && g_ds_recording == 1);
    psx_datashard_ret(&cpu);
    CHECK(g_ds_recording == 0 && ds_stat("shards") == 1);

    /* byte budget: DS_MAX_BYTES (4 MiB) of distinct bytes is fine, one more poisons */
    CHECK(psx_datashard_enter(&cpu, 0x80020014u) == 0);
    ds_note_read(0x80000000u, 4u * 1024u * 1024u);
    CHECK(ds_stat("poison_budget") == 0);
    ds_note_read(0x80400000u, 1);
    CHECK(ds_stat("poison_budget") == 1);
    psx_datashard_ret(&cpu);

    /* empty capture is a valid shard and replays with no effect and no cycles */
    CHECK(psx_datashard_enter(&cpu, 0x80020018u) == 0);
    psx_datashard_ret(&cpu);
    CHECK(ds_stat("shards") == 2);
    c0 = psx_cycle_count;
    CHECK(psx_datashard_enter(&cpu, 0x80020018u) == 1 && psx_cycle_count == c0);

    /* ret with no armed window is ignored */
    psx_datashard_ret(&cpu);
    CHECK(g_ds_recording == 0 && ds_stat("shards") == 2);

#ifdef _WIN32
    /* ---- .dss loader: valid and corrupted files (OVL-9) ---- */
    {
        const uint8_t rdb[4] = { 1, 2, 3, 4 }, wrb[4] = { 9, 8, 7, 6 };
        const Rng okr = { 0x3000, 4 }, okw = { 0x4000, 4 };
        uint32_t base = 0x80030000u;

        memset(g_ram + 0x3000, 0, 8); g_ram[0x3000] = 1; g_ram[0x3001] = 2; g_ram[0x3002] = 3; g_ram[0x3003] = 4;
        memset(g_ram + 0x4000, 0, 4);
        write_dss(base, 0x31534450u, base, 1, 1, 4, 4, &okr, rdb, &okw, wrb, 4, 4, 10, 0xAB, 0);
        uint64_t sh = ds_stat("shards");
        c0 = psx_cycle_count;
        CHECK(enter_key(base) == 1);                      /* valid file loads and replays */
        CHECK(ds_stat("shards") == sh + 1 && g_ram[0x4000] == 9 && g_ram[0x4003] == 6);
        CHECK(cpu.gpr[2] == 0xAB && psx_cycle_count - c0 == 10);

        /* each corrupt file must be refused: enter() arms a capture instead of replaying */
        uint32_t k = base + 4;
        /* bad magic */
        write_dss(k, 0x12345678u, k, 1, 1, 4, 4, &okr, rdb, &okw, wrb, 4, 4, 10, 0, 0);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        /* key in header differs from the file name's key */
        write_dss(k, 0x31534450u, k + 1, 1, 1, 4, 4, &okr, rdb, &okw, wrb, 4, 4, 10, 0, 0);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        /* read range past the unified index space */
        { Rng bad = { DS_SPACE_T - 2u, 4 };
          write_dss(k, 0x31534450u, k, 1, 1, 4, 4, &bad, rdb, &okw, wrb, 4, 4, 10, 0, 0);
          CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4; }
        /* idx + len wraps 32 bits */
        { Rng bad = { 0xFFFFFFFFu, 4 };
          write_dss(k, 0x31534450u, k, 1, 1, 4, 4, &bad, rdb, &okw, wrb, 4, 4, 10, 0, 0);
          CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4; }
        /* write range out of space */
        { Rng bad = { DS_SPACE_T, 1 };
          write_dss(k, 0x31534450u, k, 1, 1, 4, 1, &okr, rdb, &bad, wrb, 4, 1, 10, 0, 0);
          CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4; }
        /* range lengths do not add up to the stored total */
        write_dss(k, 0x31534450u, k, 1, 1, 8, 4, &okr, rdb, &okw, wrb, 4, 4, 10, 0, 0);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        /* truncated body */
        write_dss(k, 0x31534450u, k, 1, 1, 4, 4, &okr, rdb, &okw, wrb, 4, 4, 10, 0, 2);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        /* absurd counts/totals in the header must not allocate or be accepted */
        write_dss(k, 0x31534450u, k, 0xFFFFFFFFu, 0, 0, 0, &okr, rdb, &okw, wrb, 0, 0, 10, 0, 0);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        write_dss(k, 0x31534450u, k, 0, 0, 0xFFFFFFFFu, 0, &okr, rdb, &okw, wrb, 0, 0, 10, 0, 0);
        CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4;
        /* file shorter than the header */
        { char p[512]; snprintf(p, sizeof(p), CACHE_DIR "/%08X_%08X.dss", k, 1u);
          FILE *f = fopen(p, "wb"); if (f) { fwrite("PDS1", 1, 4, f); fclose(f); }
          CHECK(enter_key(k) == 0 && g_ds_recording == 1); finish_poisoned(); k += 4; }
        CHECK(ds_stat("shards") == sh + 1);                  /* none of the bad files was kept */

        /* valid file with empty sets loads and replays */
        write_dss(k, 0x31534450u, k, 0, 0, 0, 0, NULL, rdb, NULL, wrb, 0, 0, 0, 0x77, 0);
        CHECK(enter_key(k) == 1 && cpu.gpr[2] == 0x77);
    }
    remove_cache_dirs();
#endif

    free(g_ram);
    printf("data_shards_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
