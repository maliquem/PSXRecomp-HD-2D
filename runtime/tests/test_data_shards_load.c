/* A .dss file whose ranges fall outside the index space or disagree with the stored totals is
 * refused at load; a well-formed one is still accepted. Loader is Windows-only (_findfirst). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define PSX_OVERLAY_DLL_BUILD 1
#include "../src/data_shards.c"
#include <assert.h>

static uint8_t g_ram[PSX_MAIN_RAM_BACKING_BYTES], g_sp[1024];
uint32_t g_psx_ram_mask = 0x1FFFFFu, g_psx_mem_gate;
uint8_t *memory_get_ram_ptr(void) { return g_ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return g_sp; }
void psx_write_word(uint32_t a, uint32_t v) { (void)a; (void)v; }
void psx_write_byte(uint32_t a, uint8_t v) { (void)a; (void)v; }
void dirty_ram_mark_executable_range(uint32_t a, uint32_t l) { (void)a; (void)l; }
uint64_t psx_get_cycle_count(void) { return 0; }
void psx_advance_cycles(uint32_t c) { (void)c; }
void psx_check_interrupts_at(struct CPUState* cpu, uint32_t pc) { (void)cpu; (void)pc; }

static void write_dss(const char* dir, uint32_t key, uint32_t tag, uint32_t idx, uint32_t len, uint32_t total) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%08X_%08X.dss", dir, key, tag);
    FILE* f = fopen(path, "wb"); assert(f);
    uint32_t hdr[8] = { 0x31534450u, key, 0, 0, 1, 0, total, 0 };
    uint32_t args[4] = {0}, regs[DS_REG_COUNT] = {0};
    DsRange r = { idx, len };
    uint8_t bytes[16] = {0};
    fwrite(hdr, 4, 8, f); fwrite(args, 4, 4, f); fwrite(regs, 4, DS_REG_COUNT, f);
    fwrite(&r, sizeof r, 1, f); fwrite(bytes, 1, total < 16 ? total : 16, f);
    fclose(f);
}

int main(void) {
#ifdef _WIN32
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s", ".");
    s_cache_dir = dir;
    write_dss(dir, 0xA1, 1, 0, 8, 8);                       /* valid */
    ds_load_key(0xA1);
    assert(s_st.shards == 1);
    write_dss(dir, 0xA2, 1, DS_SPACE - 4, 8, 8);            /* runs past DS_SPACE */
    ds_load_key(0xA2);
    assert(s_st.shards == 1);
    write_dss(dir, 0xA3, 1, 0, 8, 4);                       /* sum != total */
    ds_load_key(0xA3);
    assert(s_st.shards == 1);
    remove("./000000A1_00000001.dss"); remove("./000000A2_00000001.dss"); remove("./000000A3_00000001.dss");
#endif
    return 0;
}
