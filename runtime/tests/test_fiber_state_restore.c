/* A state restore on the host-fiber scheduler lands on the scheduler loop.
 *
 * The scheduler loop (g_scheduler_jmpbuf) lives on the main fiber. A
 * savestate load can be polled on another thread's fiber, and a longjmp
 * across fibers is undefined, so the restore switches to the main fiber,
 * which unwinds to the loop. The native continuations of every other thread
 * belong to the replaced machine: they are dropped, and a thread switched to
 * afterwards starts a new fiber from its TCB.
 *
 * This drives the real traps.c switch and psx_scheduler_resume_at with real
 * host fibers: the main thread is preempted for the job thread, a state is
 * restored from the job thread's fiber, and the job thread is switched to
 * again after the landing. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_scheduler.h"

#define PCB_ADDR  0x0000E000u /* [0x108]: the process control block */
#define TCB_BASE  0x0000E1F4u /* [0x110]: first TCB, the main thread */
#define TCB_SIZE  (4u * 0xC0u) /* [0x114]: four TCBs */
#define TCB_JOB   (TCB_BASE + 0xC0u)
#define SAVE(tcb, reg) ((tcb) + 8u + (reg) * 4u)
#define JOB_EPC   0x800922B4u /* the job thread's loop */
#define RESUME_PC 0x8008FA48u /* where the restored machine resumes */

static uint32_t ram[0x10000u / 4u];

static uint32_t read_word(uint32_t address) {
    assert(address < sizeof(ram) && (address & 3u) == 0u);
    return ram[address / 4u];
}
static void write_word(uint32_t address, uint32_t value) {
    assert(address < sizeof(ram) && (address & 3u) == 0u);
    ram[address / 4u] = value;
}

/* Host nesting a fiber switch keeps per fiber. */
int g_psx_dispatch_depth;
static uint32_t s_resume_latch;
uint32_t psx_irq_compiled_resume_pc(void) { return s_resume_latch; }
void psx_irq_set_compiled_resume_pc(uint32_t pc) { s_resume_latch = pc; }
void psx_irq_arm_compiled_resume_pc(uint32_t pc) { s_resume_latch = pc; }
void *psx_host_nesting_save_alloc(void) { return NULL; }
void psx_host_nesting_restore_free(void *p) { (void)p; }

/* Kernel state seams traps.c reads on this path. */
uint64_t s_frame_count;
int psx_get_in_exception(void) { return 0; }
int parity_trace_is_armed(void) { return 0; }
void psx_irq_refresh_cause_ip2(void) {}
void ds_note_thread_switch(void) {}
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current_tcb, uint32_t target_tcb,
                                   uint32_t target_pc) {
    (void)kind; (void)cpu; (void)current_tcb; (void)target_tcb; (void)target_pc;
}

void psx_hle_scheduler_set_default(int on);
int psx_fiber_preempt_switch(CPUState *cpu, uint32_t from_tcb, uint32_t to_tcb);
void psx_scheduler_resume_at(uint32_t resume_pc);

static CPUState cpu;
static int s_job_starts;

/* The job thread's fiber dispatches its TCB's EPC. The first time, a state
 * restore is requested from it; the second time it hands the CPU back. */
void psx_dispatch(CPUState *c, uint32_t pc) {
    assert(pc == JOB_EPC);
    s_job_starts++;
    if (s_job_starts == 1) {
        g_psx_dispatch_depth = 9;
        psx_scheduler_resume_at(RESUME_PC);
        assert(!"a state restore never returns to the thread that asked");
    }
    ram[PCB_ADDR / 4u] = TCB_BASE;
    assert(psx_fiber_preempt_switch(c, TCB_JOB, TCB_BASE) == 1);
    assert(!"the main thread never switches back to the job thread here");
}

int main(void) {
    psx_hle_scheduler_set_default(0);   /* the host-fiber scheduler */
    ram[0x108u / 4u] = PCB_ADDR;
    ram[0x110u / 4u] = TCB_BASE;
    ram[0x114u / 4u] = TCB_SIZE;
    ram[TCB_BASE / 4u] = 0x4000u;  /* both threads runnable */
    ram[TCB_JOB / 4u] = 0x4000u;
    ram[SAVE(TCB_JOB, 32u) / 4u] = JOB_EPC;

    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = read_word;
    cpu.write_word = write_word;

    volatile int landings = 0;
    if (setjmp(g_scheduler_jmpbuf) == 0) {
        g_psx_dispatch_depth = 3;
        ram[PCB_ADDR / 4u] = TCB_JOB;          /* the handler picked the job thread */
        (void)psx_fiber_preempt_switch(&cpu, TCB_BASE, TCB_JOB);
        assert(!"the restore must unwind to the scheduler loop");
    }
    landings++;
    assert(landings == 1);                     /* landed once, on this stack */
    assert(s_job_starts == 1);
    assert(g_sched_escape.reason == PSX_RUN_RESUME_CURRENT);
    assert(g_sched_escape.resume_pc == RESUME_PC);

    /* The restored machine switches to the job thread again: its old fiber
     * was dropped, so it starts anew from its TCB, then hands back. */
    ram[PCB_ADDR / 4u] = TCB_JOB;
    assert(psx_fiber_preempt_switch(&cpu, TCB_BASE, TCB_JOB) == 1);
    assert(s_job_starts == 2);
    assert(ram[PCB_ADDR / 4u] == TCB_BASE);
    return 0;
}
