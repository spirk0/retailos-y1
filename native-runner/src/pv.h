/* Paravirtual interrupt masking: the firmware's interrupt mask functions run
 * natively without a trap. See pv.c. */
#ifndef PV_H
#define PV_H
#include <stddef.h>
#include <stdint.h>
#include "armemu.h"
#include "vcpu.h"

#define PV_BASE         0x0f000000u     /* host page: shadow state and stubs (no guest address) */
#define PV_DISABLE      (PV_BASE + 0x100u)
#define PV_RESTORE      (PV_BASE + 0x140u)

/* Map the page and write the stubs. Returns 0 on success. */
int pv_init(void);

/* 1 after pv_init() succeeded */
int pv_active(void);

/* Redirect the firmware's interrupt mask functions in image (the firmware
 * at guest address 0) to the stubs, if their words are the expected ones.
 * Call it before priv_patch(). Returns the number of functions redirected. */
int pv_patch(uint32_t *image, size_t words);

/* 1 if pc is in the page */
int pv_in_page(uint32_t pc);

/* Handler side (JIT, interpreter): c->r[15] is in the page. If it is a stub
 * entry, emulate the original function on the virtual CPU and return to lr:
 * returns 1. Inside a stub, only native code can continue: returns 0. */
int pv_emulate(struct vcpu *v, struct arm_cpu *c, const struct arm_bus *bus);

/* A trap of a stub in native code (c->r[15] is the trap word): finish the
 * function on the virtual CPU and return to lr. Returns 1 if pc was one.
 * After it, an enabled pending interrupt is due. */
int pv_trap(struct vcpu *v, struct arm_cpu *c, const struct arm_bus *bus);

/* Handler entry: the I, F and A bits that the stubs changed in native code
 * go into the virtual CPU of the core that ran natively. */
void pv_sync_in(struct vcpu *v);

/* Handler exit: v is the virtual CPU of the core that runs natively next.
 * pending: the CPSR mask bits of its active lines (0x80 IRQ, 0x40 FIQ): a
 * stub that enables one of them traps, so that the handler delivers it. */
void pv_sync_out(const struct vcpu *v, uint32_t pending);

#endif
