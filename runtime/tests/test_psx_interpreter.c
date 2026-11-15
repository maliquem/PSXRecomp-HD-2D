/* The oracle interpreter (psx_interpreter.c) must keep the R3000A pipeline
 * rules the compiled code and dirty_ram_interp.c keep (audit CPU-18):
 *   - a load's delay slot sees the old register; the load wins a same-register
 *     write in the slot; LWL/LWR merge with an in-flight load of rt;
 *   - a branch's delay slot runs before the transfer, and an exception or
 *     interrupt there reports EPC = branch with Cause.BD set;
 *   - RFE pops SR bits 5:2 into 3:0 and leaves bits 5:4 alone;
 *   - an interrupt is taken at the next instruction boundary.
 * Includes the module; the device, GTE and PGXP seams are inert stubs. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/psx_interpreter.c"

uint32_t i_stat, i_mask;
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
static CPUState s_cpu;
void psx_irq_refresh_cause_ip2(void) {
    if (i_stat & i_mask) s_cpu.cop0[13] |= 0x400u; else s_cpu.cop0[13] &= ~0x400u;
}
void sio_tick(int c) { (void)c; }
void timers_tick(int c) { (void)c; }
void gpu_vblank_tick(void) {}
void cdrom_tick(void) {}
uint32_t gte_read_data(CPUState *c, uint8_t r) { (void)c; (void)r; return 0; }
uint32_t gte_read_ctrl(CPUState *c, uint8_t r) { (void)c; (void)r; return 0; }
void gte_write_data(CPUState *c, uint8_t r, uint32_t v) { (void)c; (void)r; (void)v; }
void gte_write_ctrl(CPUState *c, uint8_t r, uint32_t v) { (void)c; (void)r; (void)v; }
void gte_execute(CPUState *c, uint32_t cmd) { (void)c; (void)cmd; }
void gte_precision_store_word(uint32_t a, uint8_t r) { (void)a; (void)r; }
void psx_pgxp_load(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) { (void)c; (void)i; (void)a; (void)v; }
void psx_pgxp_store(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) { (void)c; (void)i; (void)a; (void)v; }
void psx_pgxp_alu(struct CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) { (void)c; (void)i; (void)r; (void)a; (void)b; }
void psx_pgxp_muldiv(struct CPUState *c, uint32_t i, uint32_t h, uint32_t l, uint32_t a, uint32_t b) { (void)c; (void)i; (void)h; (void)l; (void)a; (void)b; }
void psx_pgxp_cop2(struct CPUState *c, uint32_t i, uint32_t v, uint32_t a) { (void)c; (void)i; (void)v; (void)a; }

static uint32_t mem[1024];   /* 4 KiB, mirrored over the address space */
static uint32_t rd_w(uint32_t a) { return mem[(a & 0xFFFu) >> 2]; }
static void wr_w(uint32_t a, uint32_t v) { mem[(a & 0xFFFu) >> 2] = v; }
static uint16_t rd_h(uint32_t a) { return (uint16_t)(rd_w(a) >> ((a & 2u) * 8u)); }
static void wr_h(uint32_t a, uint16_t v) { (void)a; (void)v; }
static uint8_t rd_b(uint32_t a) { return (uint8_t)(rd_w(a) >> ((a & 3u) * 8u)); }
static void wr_b(uint32_t a, uint8_t v) { (void)a; (void)v; }

#define I(op, rs, rt, imm) (((uint32_t)(op) << 26) | ((rs) << 21) | ((rt) << 16) | ((uint32_t)(imm) & 0xFFFFu))
#define R(rs, rt, rd, fn)  (((rs) << 21) | ((rt) << 16) | ((rd) << 11) | (fn))
#define OR_(rd, rs, rt)    R(rs, rt, rd, 0x25u)
#define SYSCALL_           0x0000000Cu
#define RFE_               0x42000010u
#define PROG               0x100u

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static void reset(void) {
    interp_ld_delay_discard();
    memset(mem, 0, sizeof mem);
    memset(s_cpu.gpr, 0, sizeof s_cpu.gpr);
    memset(s_cpu.cop0, 0, sizeof s_cpu.cop0);
    s_cpu.pc = PROG;
    i_stat = i_mask = 0;
}

int main(void) {
    s_cpu.read_word = rd_w; s_cpu.write_word = wr_w;
    s_cpu.read_half = rd_h; s_cpu.write_half = wr_h;
    s_cpu.read_byte = rd_b; s_cpu.write_byte = wr_b;
    interp_init(&s_cpu);

    /* Load delay: the slot sees the old value, the next one the loaded one. */
    reset();
    mem[0x80 / 4] = 0x1234u; s_cpu.gpr[8] = 0xAAAAu;
    mem[PROG / 4 + 0] = I(0x23, 0, 8, 0x80);      /* lw  t0,0x80($0) */
    mem[PROG / 4 + 1] = OR_(9, 8, 0);             /* or  t1,t0,$0 */
    mem[PROG / 4 + 2] = OR_(10, 8, 0);            /* or  t2,t0,$0 */
    interp_step(&s_cpu, 3);
    CHECK(s_cpu.gpr[9] == 0xAAAAu, "load slot saw 0x%X", s_cpu.gpr[9]);
    CHECK(s_cpu.gpr[10] == 0x1234u, "after slot saw 0x%X", s_cpu.gpr[10]);

    /* Same-register write in the load slot: the load wins. */
    reset();
    mem[0x80 / 4] = 0x1234u;
    mem[PROG / 4 + 0] = I(0x23, 0, 8, 0x80);      /* lw    t0,0x80($0) */
    mem[PROG / 4 + 1] = I(0x09, 0, 8, 5);         /* addiu t0,$0,5 */
    interp_step(&s_cpu, 2);
    CHECK(s_cpu.gpr[8] == 0x1234u, "load lost to slot write: 0x%X", s_cpu.gpr[8]);

    /* LWL then LWR on the same rt merge through the in-flight load. */
    reset();
    mem[0x80 / 4] = 0x44332211u; mem[0x84 / 4] = 0x88776655u;
    s_cpu.gpr[1] = 0x81u; s_cpu.gpr[8] = 0xAAAAAAAAu;
    mem[PROG / 4 + 0] = I(0x22, 1, 8, 3);         /* lwl t0,3(at) */
    mem[PROG / 4 + 1] = I(0x26, 1, 8, 0);         /* lwr t0,0(at) */
    interp_step(&s_cpu, 3);                       /* + nop for the delay */
    CHECK(s_cpu.gpr[8] == 0x55443322u, "unaligned load gave 0x%X", s_cpu.gpr[8]);

    /* Branch: the slot runs, then the target. */
    reset();
    mem[PROG / 4 + 0] = I(0x04, 0, 0, 3);         /* beq $0,$0,+3 -> 0x110 */
    mem[PROG / 4 + 1] = I(0x09, 0, 11, 7);        /* addiu t3,$0,7 */
    interp_step(&s_cpu, 2);
    CHECK(s_cpu.gpr[11] == 7u, "delay slot did not run");
    CHECK(s_cpu.pc == PROG + 0x10u, "branch target pc 0x%X", s_cpu.pc);

    /* SYSCALL in a delay slot: EPC = branch, BD set. */
    reset();
    mem[PROG / 4 + 0] = I(0x04, 0, 0, 3);         /* beq $0,$0,+3 */
    mem[PROG / 4 + 1] = SYSCALL_;
    interp_step(&s_cpu, 2);
    CHECK(s_cpu.pc == 0x80000080u, "vector 0x%X", s_cpu.pc);
    CHECK(s_cpu.cop0[14] == PROG, "EPC 0x%X", s_cpu.cop0[14]);
    CHECK((s_cpu.cop0[13] & 0x8000007Cu) == (0x80000000u | (8u << 2)), "Cause 0x%X", s_cpu.cop0[13]);

    /* RFE keeps the old KUo/IEo pair (bits 5:4). */
    reset();
    s_cpu.cop0[12] = 0x3Cu;
    mem[PROG / 4 + 0] = RFE_;
    interp_step(&s_cpu, 1);
    CHECK(s_cpu.cop0[12] == 0x3Fu, "SR after rfe 0x%X", s_cpu.cop0[12]);

    /* Interrupt at the next boundary, plain and between a branch and its slot. */
    reset();
    s_cpu.cop0[12] = 0x401u; i_stat = 1u; i_mask = 1u;
    interp_step(&s_cpu, 1);                       /* nop at PROG */
    CHECK(s_cpu.pc == 0x80000080u, "irq not taken, pc 0x%X", s_cpu.pc);
    CHECK(s_cpu.cop0[14] == PROG + 4u, "irq EPC 0x%X", s_cpu.cop0[14]);
    CHECK(!(s_cpu.cop0[13] & 0x80000000u), "irq BD set outside a slot");

    reset();
    s_cpu.cop0[12] = 0x401u; i_stat = 1u; i_mask = 1u;
    mem[PROG / 4 + 0] = I(0x04, 0, 0, 3);         /* beq $0,$0,+3 */
    interp_step(&s_cpu, 1);
    CHECK(s_cpu.cop0[14] == PROG, "slot irq EPC 0x%X", s_cpu.cop0[14]);
    CHECK(s_cpu.cop0[13] & 0x80000000u, "slot irq BD clear");

    /* REGIMM links only for rt 0x10/0x11; undefined rt values act as bltz/bgez without a link. */
    reset();
    mem[PROG / 4 + 0] = I(0x01, 0, 0x10, 1);      /* bltzal $0,+1 (not taken, still links) */
    interp_step(&s_cpu, 1);
    CHECK(s_cpu.gpr[31] == PROG + 8u, "bltzal link 0x%X", s_cpu.gpr[31]);
    for (uint32_t rt = 0x12; rt <= 0x13; ++rt) {
        reset();
        mem[PROG / 4 + 0] = I(0x01, 0, rt, 1);
        interp_step(&s_cpu, 1);
        CHECK(s_cpu.gpr[31] == 0u, "regimm rt=0x%X linked: 0x%X", rt, s_cpu.gpr[31]);
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("psx_interpreter_test: OK\n");
    return 0;
}
