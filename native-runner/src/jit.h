/* Template JIT for the guest code that the runner interprets (ARM host only).
 *
 * The interpreter costs about 130 ns per guest instruction on the target.
 * The guest and the host have the same instruction set, so a block of guest
 * code translates to host code that does the same operation on the same
 * flags. The guest registers stay in struct arm_cpu: each template loads its
 * operands, executes the operation and stores the result. A load or store
 * gets an inline check against interp_env.direct. A device access calls the
 * bus, and a device write ends the block. An instruction without a template
 * (privileged, trap word, rare forms) calls the interpreter for one
 * instruction.
 *
 * Each block keeps a copy of its guest words and compares it before it runs
 * in a new epoch (see jit_new_epoch()), so guest code that changes (a copy
 * to IRAM, a disk load) is translated again. Guest code that changes itself
 * inside one epoch, through interpreted stores, is not seen. A block inside
 * a static range (jit_static_code()) is never compared again.
 *
 * Bus errors are not reported: the runner's bus never fails.
 */
#ifndef JIT_H
#define JIT_H
#include "interp.h"

/* Allocate the code buffer. Returns 0 on success. */
int jit_init(void);

/* Native guest code ran: guest code can be different now. Each block
 * compares its guest words again before it runs in the new epoch. */
void jit_new_epoch(void);

/* Guest code in [lo, hi) does not change after the load (the firmware's
 * code, which the loader patched): a block inside it keeps its translation
 * in a new epoch. Without the comparison, a linked jump into the block stays
 * in the generated code. At most JIT_MAX_STATIC ranges. */
#define JIT_MAX_STATIC 16
void jit_static_code(uint32_t lo, uint32_t hi);

/* Drop all translations, also the static ones: for a change of static code */
void jit_flush(void);

/* The same contract as interp_run(). env->direct must not be NULL. A block
 * can run past run->budget by up to its length (JIT_MAX_BLOCK). */
int jit_run(struct vcpu *v, struct arm_cpu *c, const struct interp_env *env, struct interp_run *run);

#define JIT_MAX_BLOCK 48

/* translations since the start, blocks run, and flushes of the code buffer */
extern uint64_t jit_n_translated, jit_n_blocks, jit_n_flushes;
/* lookups that found a block with changed guest words, or another pc in the slot */
extern uint64_t jit_n_stale, jit_n_collide;
/* interpreter calls, device reads and writes, link requests, block
 * validations, privileged instructions */
extern uint64_t jit_n_step, jit_n_read, jit_n_write, jit_n_link, jit_n_validate, jit_n_priv;

#endif
