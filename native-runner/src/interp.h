/* ARM interpreter for the guest (ARMv5, ARM state).
 *
 * A trap into the runner costs about 40 us on the target (the vendor
 * kernel logs every signal). After a trap, cpu.c interprets the following
 * guest instructions in the same handler while more trap-worthy
 * instructions follow, so one trap covers a whole stretch: an IRQ entry
 * with its handler, or a copy loop to a device port.
 *
 * interp_step() executes one instruction. It uses armemu.c for loads and
 * stores, vcpu.c for privileged instructions, and privtrap.h trap words.
 * Pure C.
 */
#ifndef INTERP_H
#define INTERP_H
#include "vcpu.h"

enum {
    INTERP_OK = 0,      /* executed: c->r[15] is the next instruction */
    INTERP_STOP,        /* not executed: the host must run this instruction */
    INTERP_ERROR        /* not executed: the instruction is invalid here */
};

/* Callbacks for what the interpreter cannot do itself. */
struct interp_env {
    /* data access that is not direct (see below). While it runs, c->r[15]
     * is the address of the instruction that makes the access. */
    const struct arm_bus *bus;
    /* instruction word at pc, if pc is not direct: 1 on success */
    int (*fetch)(void *ctx, uint32_t pc, uint32_t *word);
    /* semihosting call (svc 0x123456, as a trap word) */
    void (*semihost)(void *ctx, struct arm_cpu *c);
    void *ctx;
    /* 65536 entries, one for each 64 KB block of the guest address space.
     * 1: the host has guest RAM at the same address, so the interpreter
     * accesses it directly. Page 0 is never direct. NULL: no block is. */
    const uint8_t *direct;
    /* after a device write: 1 if the batch must stop (the write stopped the
     * core, for example). NULL: every device write stops the batch. */
    int (*need_stop)(void *ctx);
    /* the JIT reads guest code at page 0 from pc + page0_alias, if that
     * address is direct (0: page 0 code is interpreted) */
    uint32_t page0_alias;
    /* after a privileged instruction: 1 if an IRQ can be taken now, so the
     * batch must stop. NULL: every privileged instruction stops the batch. */
    int (*irq_ready)(void *ctx);
};

/* One batch of instructions for interp_run() */
struct interp_run {
    uint32_t budget;      /* in: most instructions to execute */
    uint32_t idle_limit;  /* in: stop after this many fast instructions in a row (0: no limit) */
    uint32_t idle;        /* in/out: fast instructions in a row so far */
    uint32_t n;           /* out: instructions executed */
    int stop;             /* out: 1 if the batch stopped early: idle limit, a device
                           * write (see need_stop) or a privileged instruction (see
                           * irq_ready). The caller must then check the core sleep
                           * and the IRQs. */
    int last_slow;        /* out: internal flags of the last instruction */
};

/* Execute instructions from c->r[15]. A "slow" instruction is one that
 * would trap when the host executes it: privileged, a trap word, or an
 * access that is not direct. The others are "fast" (see idle_limit).
 * Returns INTERP_OK, or INTERP_STOP/INTERP_ERROR for the instruction at
 * c->r[15], which did not execute. */
int interp_run(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, struct interp_run *run);

/* Execute the instruction at c->r[15]. *slow is set to 1 if the
 * instruction is privileged or a trap word. */
int interp_step(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, int *slow);

/* Debug: the last interpreted pcs (bit 0: IRQ mode), if interp_pc_ring_on */
#define INTERP_PC_RING 512
extern uint32_t interp_pc_ring[INTERP_PC_RING];
extern unsigned interp_pc_ring_i;
extern int interp_pc_ring_on;

#endif
