/* ChangeTh returns 1 to the thread that called it, once it runs again.
 *
 * The kernel's SYS(03h) handler stores the 1 in R2 of the yielding thread's
 * TCB (psx-spx "B(10h) - ChangeTh"); the kernel wrapper itself only runs
 * `syscall; jr $ra`, so R2 holds whatever the wrapper left there (its TCB
 * offset arithmetic). The HLE scheduler takes SYS(03h) without that handler
 * and saved the live R2 instead. Front Mission 2 switches to its job thread
 * every frame and turns the thread off for good when ChangeTh returns anything
 * but 1, which left its scene loads queued forever.
 *
 * This drives the real traps.c case 3 with the HLE scheduler (the default) and
 * a two-thread TCB table, then checks the yielding thread's saved context. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"

#define PCB_ADDR  0x0000E000u /* [0x108]: the process control block */
#define TCB_BASE  0x0000E1F4u /* [0x110]: first TCB, the main thread */
#define TCB_SIZE  (4u * 0xC0u) /* [0x114]: four TCBs */
#define TCB_JOB   (TCB_BASE + 0xC0u)
#define SAVE(tcb, reg) ((tcb) + 8u + (reg) * 4u)

static uint32_t ram[0x10000u / 4u];

static uint32_t read_word(uint32_t address) {
    assert(address < sizeof(ram) && (address & 3u) == 0u);
    return ram[address / 4u];
}
static void write_word(uint32_t address, uint32_t value) {
    assert(address < sizeof(ram) && (address & 3u) == 0u);
    ram[address / 4u] = value;
}

/* Kernel state seams traps.c reads on this path. */
uint64_t s_frame_count;
extern jmp_buf g_scheduler_jmpbuf;
int psx_get_in_exception(void) { return 0; }
int overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
int parity_trace_is_armed(void) { return 0; }
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current_tcb, uint32_t target_tcb,
                                   uint32_t target_pc) {
    (void)kind; (void)cpu; (void)current_tcb; (void)target_tcb; (void)target_pc;
}

int psx_syscall(CPUState *cpu, uint32_t code);

static CPUState cpu;
static CPUState before;

int main(void) {
    ram[0x108u / 4u] = PCB_ADDR;
    ram[0x110u / 4u] = TCB_BASE;
    ram[0x114u / 4u] = TCB_SIZE;
    ram[PCB_ADDR / 4u] = TCB_BASE; /* the main thread is current */
    ram[TCB_BASE / 4u] = 0x4000u;  /* both threads runnable */
    ram[TCB_JOB / 4u] = 0x4000u;

    memset(&cpu, 0, sizeof(cpu));
    for (unsigned i = 1; i < 32; ++i) cpu.gpr[i] = 0x55550000u + i;
    cpu.gpr[2] = 0x00000546u;   /* what the kernel wrapper leaves in R2 */
    cpu.gpr[4] = 3u;            /* SYS(03h) */
    cpu.gpr[5] = TCB_JOB;       /* ChangeThread target: the job thread */
    cpu.gpr[31] = 0x80092084u;  /* the caller's resume point */
    cpu.cop0[12] = 0x00000401u;
    cpu.cop0[13] = 0x00000400u;
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    before = cpu;

    if (setjmp(g_scheduler_jmpbuf) == 0) {
        psx_syscall(&cpu, 0u);
        assert(!"a switch to another runnable thread must unwind to the scheduler");
    }

    assert(ram[PCB_ADDR / 4u] == TCB_JOB);           /* the job thread is current */
    assert(ram[SAVE(TCB_BASE, 2u) / 4u] == 1u);      /* ChangeTh returns 1 */
    assert(ram[SAVE(TCB_BASE, 32u) / 4u] == before.gpr[31]); /* resume at the caller */
    for (unsigned i = 1; i < 32; ++i) {
        if (i == 2u || i == 26u) continue;            /* R2 = result, k0 not saved */
        assert(ram[SAVE(TCB_BASE, i) / 4u] == before.gpr[i]);
    }
    return 0;
}
