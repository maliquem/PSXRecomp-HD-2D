/* A thread preempted inside the exception handler resumes in place.
 *
 * A game's interrupt callback can point PCB[0] at another thread, so the
 * ReturnFromException that ends the handler resumes THAT thread (Front
 * Mission 2's VBlank callback takes the CPU back from its job thread every
 * frame). On the host-fiber scheduler (PSX_HLE_SCHEDULER=0) the interrupted
 * thread's native stack stays suspended in its fiber and continues when a
 * later switch names it again.
 *
 * This drives the real traps.c psx_fiber_preempt_switch with real host
 * fibers: the main thread is preempted for the job thread, which starts from
 * its TCB, sets its own host nesting and preempts back. The main thread must
 * resume inside the call with its own nesting and PCB[0] naming it. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"

#define PCB_ADDR  0x0000E000u /* [0x108]: the process control block */
#define TCB_BASE  0x0000E1F4u /* [0x110]: first TCB, the main thread */
#define TCB_SIZE  (4u * 0xC0u) /* [0x114]: four TCBs */
#define TCB_JOB   (TCB_BASE + 0xC0u)
#define SAVE(tcb, reg) ((tcb) + 8u + (reg) * 4u)
#define JOB_EPC   0x800922B4u /* the job thread's loop */
#define JOB_S0    0x8012B0B4u

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
extern int g_psx_call_bail;      /* traps.c */
static uint32_t s_resume_latch;
uint32_t psx_irq_compiled_resume_pc(void) { return s_resume_latch; }
void psx_irq_set_compiled_resume_pc(uint32_t pc) { s_resume_latch = pc; }
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

int psx_fiber_preempt_switch(CPUState *cpu, uint32_t from_tcb, uint32_t to_tcb);

static CPUState cpu;
static int s_job_runs;

/* The job thread's fiber dispatches its TCB's EPC. */
void psx_dispatch(CPUState *c, uint32_t pc) {
    assert(pc == JOB_EPC);
    assert(c->gpr[16] == JOB_S0);              /* its own context, from its TCB */
    assert(ram[PCB_ADDR / 4u] == TCB_JOB);
    s_job_runs++;
    g_psx_dispatch_depth = 9;                  /* the job thread's own nesting */
    g_psx_call_bail = 1;
    s_resume_latch = 0x8009AAA0u;
    /* Its VBlank callback hands the CPU back to the main thread. */
    ram[PCB_ADDR / 4u] = TCB_BASE;
    assert(psx_fiber_preempt_switch(c, TCB_JOB, TCB_BASE) == 1);
    assert(!"the main thread never switches back to the job thread here");
}

int main(void) {
    ram[0x108u / 4u] = PCB_ADDR;
    ram[0x110u / 4u] = TCB_BASE;
    ram[0x114u / 4u] = TCB_SIZE;
    ram[TCB_BASE / 4u] = 0x4000u;  /* both threads runnable */
    ram[TCB_JOB / 4u] = 0x4000u;
    ram[SAVE(TCB_JOB, 16u) / 4u] = JOB_S0;
    ram[SAVE(TCB_JOB, 29u) / 4u] = 0x801FFF00u;
    ram[SAVE(TCB_JOB, 32u) / 4u] = JOB_EPC;
    ram[SAVE(TCB_JOB, 35u) / 4u] = 0x00000404u; /* SR */

    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    g_psx_dispatch_depth = 3;          /* the main thread is three calls deep */
    s_resume_latch = 0x80091D30u;

    /* The exception handler pointed PCB[0] at the job thread. */
    ram[PCB_ADDR / 4u] = TCB_JOB;
    assert(psx_fiber_preempt_switch(&cpu, TCB_BASE, TCB_JOB) == 1);

    assert(s_job_runs == 1);
    assert(ram[PCB_ADDR / 4u] == TCB_BASE);    /* the main thread is current */
    assert(g_psx_dispatch_depth == 3);         /* with its own host nesting */
    assert(g_psx_call_bail == 0);
    assert(s_resume_latch == 0x80091D30u);
    return 0;
}
