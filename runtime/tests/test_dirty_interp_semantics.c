/* Per-instruction semantics of the dirty-RAM interpreter, CPU-27 (b).
 *
 * Each case puts MIPS words in test RAM, sets input registers, runs the
 * production decoder (exec_one_fetched, the entry the other interpreter tests
 * use) and checks GPRs, HI/LO, memory and, for exceptions, COP0 Cause
 * (ExcCode, BD), EPC, BadVAddr, SR and the vector PC. Expected values come
 * from the R3000A as documented by psx-spx, not from the interpreter. Cases
 * where the interpreter disagrees are KNOWN divergences tied to their audit
 * item: printed, counted, and they do not fail the run.
 *
 * Same build as test_ws_cull_edge_interp.c: gpu.c and dirty_ram_interp.c are
 * #included, unreferenced code is dropped at link time (LTO plus section GC /
 * dead_strip), so only the stubs below are needed. */
#include "../src/gpu.c"
#include "../src/dirty_ram_interp.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- link stubs for code reachable from exec_one_fetched ------------------ */
uint64_t s_frame_count;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen;
uint64_t g_dispatch_static_hits;
uint32_t g_psx_mod_instruction_hooks;
uint8_t  g_psx_mod_instruction_filter[1024];
void psx_mod_instruction(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)cpu; (void)pc; (void)instruction; abort();
}
int      g_rfe_escape_pending;
int      g_exc_escape_reason;
int      g_psx_call_bail;
uint64_t g_psx_bail_first;
uint64_t g_psx_bail_resolved;
uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
/* Live RAM geometry (memory.c): retail 2 MiB, as the stub RAM below. */
uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;

static uint8_t test_ram[0x00200000u];

uint8_t *memory_get_ram_ptr(void) { return test_ram; }
uint8_t psx_read_byte(uint32_t a) { return test_ram[a & 0x1FFFFFu]; }
uint16_t psx_read_half(uint32_t a) {
    uint16_t v; memcpy(&v, test_ram + (a & 0x1FFFFEu), sizeof v); return v;
}
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, test_ram + (a & 0x1FFFFCu), sizeof v); return v;
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t a, uint32_t rt, uint32_t m) {
    (void)cpu; (void)rt; (void)m; return psx_read_byte(a);
}
uint32_t psx_cyc_lwc2_read_slow(CPUState *cpu, uint32_t a) { (void)cpu; return psx_read_word(a); }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 1; }
int mdec_recently_active(uint32_t f) { (void)f; return 0; }
void gte_execute(CPUState *cpu, uint32_t cmd) { (void)cpu; (void)cmd; }
int gte_nclip_native_wide_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = 0; return 0;
}
int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = 0; return 0;
}
void gte_precision_store_word(uint32_t a, uint8_t r) { (void)a; (void)r; }
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t r) { (void)cpu; (void)r; return 0; }
uint32_t gte_read_data(CPUState *cpu, uint8_t r) { (void)cpu; (void)r; return 0; }
void gte_write_ctrl(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; (void)r; (void)v; }
void gte_write_data(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; (void)r; (void)v; }
static uint32_t s_native_v0;  /* v0 a native callee would see (CPU-7 (d)) */
int overlay_loader_call_native(CPUState *cpu, uint32_t a) { (void)a; s_native_v0 = cpu->gpr[2]; return 0; }
void psx_bail_record(uint32_t ra, uint32_t sp, uint32_t pc, uint32_t gsp) {
    (void)ra; (void)sp; (void)pc; (void)gsp;
}
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) { (void)cpu; (void)code; (void)pc; }
void psx_check_interrupts(struct CPUState *cpu) { (void)cpu; }
void psx_dispatch_call(CPUState *cpu, uint32_t t, uint32_t r) { (void)cpu; (void)t; (void)r; }
void psx_fatal_halt(const char *reason) { printf("fatal: %s\n", reason); exit(2); }
void psx_get_freeze_diag(uint64_t *a, uint32_t *b, int *c, int *d, uint64_t *e,
                         uint64_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
int psx_get_in_exception(void) { return 0; }
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 1; }
void psx_pgxp_alu(struct CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)r; (void)a; (void)b;
}
void psx_pgxp_cop2(struct CPUState *c, uint32_t i, uint32_t v, uint32_t a) {
    (void)c; (void)i; (void)v; (void)a;
}
uint32_t g_pgxp_mem_skip;   /* pgxp_hooks.h PGXP_MEM_ON (pgxp_session.cpp) */
void psx_pgxp_load(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_pgxp_muldiv(struct CPUState *c, uint32_t i, uint32_t h, uint32_t l,
                     uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)h; (void)l; (void)a; (void)b;
}
void psx_pgxp_store(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_rfe_mark_escape(void) {}
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; return 0; }

/* CPUState memory callbacks over the same test RAM. */
static void t_write_word(uint32_t a, uint32_t v) { memcpy(test_ram + (a & 0x1FFFFCu), &v, 4); }
static void t_write_half(uint32_t a, uint16_t v) { memcpy(test_ram + (a & 0x1FFFFEu), &v, 2); }
static void t_write_byte(uint32_t a, uint8_t v) { test_ram[a & 0x1FFFFFu] = v; }

/* ---- harness --------------------------------------------------------------- */
static int s_fail, s_known;
#define CHECK(name, c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, name); s_fail = 1; } } while (0)
#define KNOWN(name, id, c) do { if (!(c)) { printf("KNOWN %s (%s)\n", name, id); s_known++; } } while (0)

enum { ZERO = 0, V0 = 2, V1 = 3, A0 = 4, A1 = 5 };
enum { SR = 12, CAUSE = 13, EPC = 14, BADVADDR = 8 };
#define CODE   0x80020000u /* phys 0x20000: outside the BIOS load-delay window */
#define DATA   0x80100000u
#define VECTOR 0x80000080u /* SR.BEV = 0 */
#define EXC_ADEL 4u
#define EXC_ADES 5u
#define EXC_RI   10u
#define EXC_OV   12u

static uint32_t i_type(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t r_type(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
static void put_word(uint32_t addr, uint32_t v) { memcpy(test_ram + (addr & 0x1FFFFFu), &v, 4); }
static uint32_t get_word(uint32_t addr) { return psx_read_word(addr); }

static CPUState s_cpu;

/* Fresh CPU: SR = IEc only (BEV = 0, so exceptions vector to 0x80000080). */
static void reset(void) {
    memset(&s_cpu, 0, sizeof s_cpu);
    s_cpu.read_word = psx_read_word;
    s_cpu.read_half = psx_read_half;
    s_cpu.read_byte = psx_read_byte;
    s_cpu.write_word = t_write_word;
    s_cpu.write_half = t_write_half;
    s_cpu.write_byte = t_write_byte;
    s_cpu.cop0[SR] = 0x00000001u;
    g_unsupported_seen = 0;
}

/* Runs one instruction at `pc` (a delay slot, if any, comes from RAM). */
static int step(uint32_t pc, uint32_t insn) {
    uint32_t next = 0;
    s_cpu.pc = pc;
    return exec_one_fetched(&s_cpu, pc, insn, &next);
}

/* An exception was taken: vector PC, ExcCode, BD, EPC and the SR mode stack
 * pushed by two bits (psx-spx COP0). */
static int exc_taken(int rv, uint32_t code, uint32_t epc, uint32_t bd) {
    const uint32_t cause = s_cpu.cop0[CAUSE];
    return rv == 1 && !g_unsupported_seen && s_cpu.pc == VECTOR &&
           ((cause >> 2) & 0x1Fu) == code && (cause >> 31) == bd &&
           s_cpu.cop0[EPC] == epc && (s_cpu.cop0[SR] & 0x3Fu) == ((0x01u << 2) & 0x3Fu);
}

/* ---- LWL / LWR / SWL / SWR ------------------------------------------------- */
/* psx-spx: little-endian; LWL fills rt from its top byte down with the bytes
 * from the aligned word's start up to addr, LWR fills rt from its bottom byte
 * up with the bytes from addr to the word's end; SWL/SWR store the same
 * byte ranges; the other bytes of rt / memory are kept. */
static void test_unaligned(void) {
    static const uint32_t lwl_want[4] = { 0x00BBCCDDu, 0x1100CCDDu, 0x221100DDu, 0x33221100u };
    static const uint32_t lwr_want[4] = { 0x33221100u, 0xAA332211u, 0xAABB3322u, 0xAABBCC33u };
    static const uint32_t swl_want[4] = { 0x332211AAu, 0x3322AABBu, 0x33AABBCCu, 0xAABBCCDDu };
    static const uint32_t swr_want[4] = { 0xAABBCCDDu, 0xBBCCDD00u, 0xCCDD1100u, 0xDD221100u };
    char name[64];
    for (uint32_t k = 0; k < 4; k++) {
        reset();
        put_word(DATA, 0x33221100u);
        s_cpu.gpr[A0] = DATA;
        s_cpu.gpr[V0] = 0xAABBCCDDu;
        CHECK("lwl rv", step(CODE, i_type(0x22, A0, V0, (uint16_t)k)) == 0);
        snprintf(name, sizeof name, "lwl offset %u", (unsigned)k);
        CHECK(name, s_cpu.gpr[V0] == lwl_want[k]);

        reset();
        put_word(DATA, 0x33221100u);
        s_cpu.gpr[A0] = DATA;
        s_cpu.gpr[V0] = 0xAABBCCDDu;
        CHECK("lwr rv", step(CODE, i_type(0x26, A0, V0, (uint16_t)k)) == 0);
        snprintf(name, sizeof name, "lwr offset %u", (unsigned)k);
        CHECK(name, s_cpu.gpr[V0] == lwr_want[k]);

        reset();
        put_word(DATA, 0x33221100u);
        s_cpu.gpr[A0] = DATA;
        s_cpu.gpr[V0] = 0xAABBCCDDu;
        CHECK("swl rv", step(CODE, i_type(0x2A, A0, V0, (uint16_t)k)) == 0);
        snprintf(name, sizeof name, "swl offset %u", (unsigned)k);
        CHECK(name, get_word(DATA) == swl_want[k]);
        CHECK("swl keeps rt", s_cpu.gpr[V0] == 0xAABBCCDDu);

        reset();
        put_word(DATA, 0x33221100u);
        s_cpu.gpr[A0] = DATA;
        s_cpu.gpr[V0] = 0xAABBCCDDu;
        CHECK("swr rv", step(CODE, i_type(0x2E, A0, V0, (uint16_t)k)) == 0);
        snprintf(name, sizeof name, "swr offset %u", (unsigned)k);
        CHECK(name, get_word(DATA) == swr_want[k]);
    }

    /* The unaligned-load idiom: lwr rt,1(a0) then lwl rt,4(a0) reads the word
     * at a0+1 (bytes 11 22 33 44). */
    reset();
    put_word(DATA, 0x33221100u);
    put_word(DATA + 4u, 0x77665544u);
    s_cpu.gpr[A0] = DATA;
    s_cpu.gpr[V0] = 0xAABBCCDDu;
    (void)step(CODE, i_type(0x26, A0, V0, 1));
    (void)step(CODE + 4u, i_type(0x22, A0, V0, 4));
    CHECK("lwr+lwl unaligned word", s_cpu.gpr[V0] == 0x44332211u);
}

/* ---- DIV / DIVU edges ------------------------------------------------------ */
/* psx-spx: no exception; DIV by 0 gives LO = -1 if rs >= 0 else +1, HI = rs;
 * DIVU by 0 gives LO = 0xFFFFFFFF, HI = rs; 0x80000000 / -1 gives
 * LO = 0x80000000, HI = 0; otherwise quotient truncates toward zero and the
 * remainder takes the dividend's sign. */
static void div_case(const char *name, uint32_t funct, uint32_t a, uint32_t b,
                     uint32_t lo, uint32_t hi) {
    reset();
    s_cpu.gpr[A0] = a;
    s_cpu.gpr[A1] = b;
    s_cpu.hi = s_cpu.lo = 0x5A5A5A5Au;
    const int rv = step(CODE, r_type(A0, A1, ZERO, funct));
    CHECK(name, rv == 0 && s_cpu.lo == lo && s_cpu.hi == hi);
}
static void test_div(void) {
    div_case("div 5/0",            0x1A, 5u,          0u,          0xFFFFFFFFu, 5u);
    div_case("div 0/0",            0x1A, 0u,          0u,          0xFFFFFFFFu, 0u);
    div_case("div -5/0",           0x1A, 0xFFFFFFFBu, 0u,          1u,          0xFFFFFFFBu);
    div_case("div 0x80000000/0",   0x1A, 0x80000000u, 0u,          1u,          0x80000000u);
    div_case("div 0x80000000/-1",  0x1A, 0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0u);
    div_case("div -7/2",           0x1A, 0xFFFFFFF9u, 2u,          0xFFFFFFFDu, 0xFFFFFFFFu);
    div_case("div 7/-2",           0x1A, 7u,          0xFFFFFFFEu, 0xFFFFFFFDu, 1u);
    div_case("divu 5/0",           0x1B, 5u,          0u,          0xFFFFFFFFu, 5u);
    div_case("divu 0x80000000/0",  0x1B, 0x80000000u, 0u,          0xFFFFFFFFu, 0x80000000u);
    div_case("divu 0x80000000/0xFFFFFFFF", 0x1B, 0x80000000u, 0xFFFFFFFFu, 0u, 0x80000000u);
}

/* ---- ADD / ADDI / SUB overflow --------------------------------------------- */
/* psx-spx: ADD/ADDI/SUB raise Integer Overflow (ExcCode 0Ch, EPC = the
 * instruction, rt/rd unchanged) on signed overflow; ADDU/ADDIU/SUBU wrap. */
static void ov_case(const char *name, uint32_t insn, uint32_t a, uint32_t b) {
    reset();
    s_cpu.gpr[A0] = a;
    s_cpu.gpr[A1] = b;
    s_cpu.gpr[V0] = 0x12345678u;
    const int rv = step(CODE, insn);
    CHECK(name, exc_taken(rv, EXC_OV, CODE, 0u) && s_cpu.gpr[V0] == 0x12345678u);
}
static void no_ov_case(const char *name, uint32_t insn, uint32_t a, uint32_t b,
                       uint32_t want) {
    reset();
    s_cpu.gpr[A0] = a;
    s_cpu.gpr[A1] = b;
    const int rv = step(CODE, insn);
    CHECK(name, rv == 0 && s_cpu.gpr[V0] == want && s_cpu.cop0[EPC] == 0u);
}
static void test_overflow(void) {
    const uint32_t add  = r_type(A0, A1, V0, 0x20);
    const uint32_t addu = r_type(A0, A1, V0, 0x21);
    const uint32_t sub  = r_type(A0, A1, V0, 0x22);
    const uint32_t subu = r_type(A0, A1, V0, 0x23);
    const uint32_t sub0 = r_type(ZERO, A1, V0, 0x22);  /* sub v0, zero, a1 */

    ov_case("add 0x7FFFFFFF+1",        add,  0x7FFFFFFFu, 1u);
    ov_case("add 0x80000000+0x80000000", add, 0x80000000u, 0x80000000u);
    ov_case("addi 0x7FFFFFFF+1",       i_type(0x08, A0, V0, 1),      0x7FFFFFFFu, 0u);
    ov_case("addi 0x80000000+(-1)",    i_type(0x08, A0, V0, 0xFFFF), 0x80000000u, 0u);
    ov_case("sub 0x80000000-1",        sub,  0x80000000u, 1u);
    ov_case("sub 0-0x80000000",        sub0, 0u,          0x80000000u);

    no_ov_case("add no overflow",   add,  0x7FFFFFFEu, 1u, 0x7FFFFFFFu);
    no_ov_case("add -1+1",          add,  0xFFFFFFFFu, 1u, 0u);
    no_ov_case("addu wraps",        addu, 0x7FFFFFFFu, 1u, 0x80000000u);
    no_ov_case("addi no overflow",  i_type(0x08, A0, V0, 0xFFFF), 0u, 0u, 0xFFFFFFFFu);
    no_ov_case("addiu wraps",       i_type(0x09, A0, V0, 1), 0x7FFFFFFFu, 0u, 0x80000000u);
    no_ov_case("sub no overflow",   sub,  0x80000001u, 1u, 0x80000000u);
    no_ov_case("subu wraps",        subu, 0x80000000u, 1u, 0x7FFFFFFFu);
}

/* ---- exceptions in and out of a branch delay slot -------------------------- */
/* psx-spx COP0: an exception in a branch delay slot sets Cause.BD = 1 and
 * EPC = the branch; outside a slot BD = 0 and EPC = the instruction. Address
 * errors set BadVAddr (AdEL 4 for loads, AdES 5 for stores). */
static void test_delay_slot(void) {
    const uint32_t lw_mis = i_type(0x23, A0, V0, 1);   /* lw v0,1(a0) */
    const uint32_t sw_mis = i_type(0x2B, A0, V0, 2);   /* sw v0,2(a0) */
    const uint32_t beq    = i_type(0x04, ZERO, ZERO, 3); /* beq zero,zero,+3 */
    int rv;

    /* Control: a misaligned load outside a slot. */
    reset();
    s_cpu.gpr[A0] = DATA;
    rv = step(CODE, lw_mis);
    CHECK("lw misaligned: AdEL, BD=0, EPC=pc",
          exc_taken(rv, EXC_ADEL, CODE, 0u) && s_cpu.cop0[BADVADDR] == DATA + 1u);

    reset();
    s_cpu.gpr[A0] = DATA;
    rv = step(CODE, sw_mis);
    CHECK("sw misaligned: AdES, BD=0, EPC=pc",
          exc_taken(rv, EXC_ADES, CODE, 0u) && s_cpu.cop0[BADVADDR] == DATA + 2u);

    /* Control: a taken branch with a plain slot. */
    reset();
    put_word(CODE + 0x104u, i_type(0x09, ZERO, V1, 7)); /* addiu v1,zero,7 */
    rv = step(CODE + 0x100u, beq);
    CHECK("beq + addiu slot", rv == 1 && s_cpu.pc == CODE + 0x110u && s_cpu.gpr[V1] == 7u);

    /* The misaligned load in the slot of a taken branch. */
    reset();
    put_word(CODE + 0x204u, lw_mis);
    s_cpu.gpr[A0] = DATA;
    rv = step(CODE + 0x200u, beq);
    KNOWN("lw misaligned in delay slot: vector, BD=1, EPC=branch", "CPU-2",
          exc_taken(rv, EXC_ADEL, CODE + 0x200u, 1u) &&
          s_cpu.cop0[BADVADDR] == DATA + 1u);
}

/* ---- reserved instructions ------------------------------------------------- */
/* psx-spx: an unassigned SPECIAL funct or primary opcode raises Reserved
 * Instruction (ExcCode 0Ah, EPC = the instruction). */
static void test_reserved(void) {
    int rv;
    reset();
    s_cpu.gpr[V0] = 0x12345678u;
    rv = step(CODE, r_type(A0, A1, V0, 0x01));          /* SPECIAL funct 01h */
    KNOWN("SPECIAL funct 01h raises RI", "CPU-9", exc_taken(rv, EXC_RI, CODE, 0u));

    reset();
    rv = step(CODE, i_type(0x18, A0, V0, 0));           /* primary opcode 18h */
    CHECK("primary opcode 18h raises RI", exc_taken(rv, EXC_RI, CODE, 0u));

    /* LWC0 raises Coprocessor Unusable (0Bh) with CE = 0; BadVAddr untouched. */
    reset();
    s_cpu.cop0[CAUSE] = 0x30000000u;
    s_cpu.cop0[BADVADDR] = 0xDEADBEEFu;
    rv = step(CODE, i_type(0x30, A0, V0, 0));
    CHECK("LWC0 raises CpU, CE=0", exc_taken(rv, 11u, CODE, 0u) &&
          (s_cpu.cop0[CAUSE] & 0x30000000u) == 0u && s_cpu.cop0[BADVADDR] == 0xDEADBEEFu);

    /* A reserved word in a delay slot still aborts (no slot exception delivery yet). */
    reset();
    put_word(CODE + 0x304u, i_type(0x3F, A0, V0, 0));
    rv = step(CODE + 0x300u, i_type(0x04, ZERO, ZERO, 3));
    KNOWN("reserved opcode in delay slot: RI, BD=1, EPC=branch", "CPU-2",
          exc_taken(rv, EXC_RI, CODE + 0x300u, 1u));
    CHECK("reserved in delay slot leaves SR", (s_cpu.cop0[SR] & 0x3Fu) == 0x01u);
    g_unsupported_seen = 0;
}

/* ---- load delay (BIOS kernel RAM, where the interpreter models it) --------- */
/* psx-spx "Caution - Load Delay": the load's target is not updated until the
 * next opcode has completed. Audit CPU-7 (c) and (d). */
#define KCODE 0x80001000u /* phys 0x1000: inside the BIOS load-delay window */
static void test_load_delay(void) {
    const uint32_t lw0 = i_type(0x23, A0, V0, 0);   /* lw v0,0(a0) */
    const uint32_t lw4 = i_type(0x23, A0, V0, 4);   /* lw v0,4(a0) */

    /* (c) Two loads of v0 in a row; the second loads v0's old value. */
    reset();
    dirty_ram_ld_delay_discard();
    s_cpu.gpr[A0] = DATA;
    s_cpu.gpr[V0] = 0x11u;
    put_word(DATA, 0x22u);
    put_word(DATA + 4u, 0x11u);
    step(KCODE, lw0);
    CHECK("lw: next opcode sees old v0", s_cpu.gpr[V0] == 0x11u);
    step(KCODE + 4u, lw4);
    dirty_ram_ld_delay_flush(&s_cpu);
    CHECK("lw v0; lw v0 (old value): the second load lands", s_cpu.gpr[V0] == 0x11u);

    /* (d) jal with a load in its slot: a native callee sees the loaded v0. */
    reset();
    dirty_ram_ld_delay_discard();
    s_cpu.gpr[A0] = DATA;
    s_cpu.gpr[V0] = 0x11u;
    put_word(DATA, 0x22u);
    put_word(KCODE + 0x104u, lw0);
    s_native_v0 = 0;
    step(KCODE + 0x100u, (0x03u << 26) | ((0x80030000u >> 2) & 0x03FFFFFFu)); /* jal */
    CHECK("jal + lw slot: native callee sees loaded v0", s_native_v0 == 0x22u);
    dirty_ram_ld_delay_discard();
}

int main(void) {
    test_unaligned();
    test_div();
    test_overflow();
    test_delay_slot();
    test_reserved();
    test_load_delay();
    if (s_fail) return 1;
    printf("dirty_interp_semantics_test: ok (%d known)\n", s_known);
    return 0;
}
