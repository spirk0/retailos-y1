/* Paravirtual interrupt masking.
 *
 * The retailOS masks interrupts through two small functions, thousands of
 * times per second (the drawing code and the kernel's locks):
 *   irq_disable at 0x289ec0: mrs r0, cpsr; orr r1, r0, #0x80; msr cpsr, r1;
 *                            mov pc, lr   (returns the old CPSR)
 *   irq_restore at 0x289eb8: msr cpsr, r0; mov pc, lr
 * In native code each MRS and MSR is a trap of about 24 us. The traps also
 * kept the CPU's drawing code in the handler, at JIT speed.
 *
 * The loader redirects both functions (ldr pc, [pc, #-4] and the address)
 * to stubs in a host page at PV_BASE, which is no guest address. The stubs
 * work on a shadow word V (mode, A, I, F of the core that runs natively):
 * the runner writes V at each handler exit and reads it back at each entry
 * (pv_sync_out, pv_sync_in). Two cases still trap in the stub, and the
 * handler finishes them (pv_trap): a restore to another mode (the banked
 * registers), and a restore that enables a pending interrupt (the word P,
 * also written at handler exit), so that the interrupt comes at once.
 *
 * The JIT and the interpreter cannot fetch from the page (no guest memory):
 * at a stub entry they stop, and the handler emulates the function on the
 * virtual CPU (pv_emulate). The stubs use r0, r1, r12 and the flags as the
 * firmware's functions may (AAPCS scratch registers).
 */
#define _GNU_SOURCE
#include "pv.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PV_SIZE         0x1000u
#define PV_V            0u              /* word index: the shadow mode and mask bits */
#define PV_P            1u              /* word index: the pending lines, as CPSR mask bits */
#define IRQ_RESTORE     0x289eb8u
#define IRQ_DISABLE     0x289ec0u
#define TRAP_MODE       0xe7fdeef0u     /* UDF: the restore needs another mode */
#define TRAP_DELIVER    0xe7fdeef1u     /* UDF: the restore enabled a pending interrupt */
#define MSR_CPSR_R0     0xe12ff000u     /* msr CPSR_fsxc, r0 */
#define LDR_PC_NEXT     0xe51ff004u     /* ldr pc, [pc, #-4] */
#define CPSR_FLAGS      0xf8000000u

static const uint32_t stub_disable[] = {
    0xe3a0c40fu,                        /* mov r12, #PV_BASE */
    0xe59c0000u,                        /* ldr r0, [r12]: V */
    0xe10f1000u,                        /* mrs r1, cpsr: the flags (user mode may read them) */
    0xe20114f8u,                        /* and r1, r1, #0xf8000000 */
    0xe1800001u,                        /* orr r0, r0, r1: the old CPSR */
    0xe3801080u,                        /* orr r1, r0, #0x80 */
    0xe7e81051u,                        /* ubfx r1, r1, #0, #9: mode, A, I, F */
    0xe58c1000u,                        /* str r1, [r12] */
    0xe12fff1eu,                        /* bx lr */
};

static const uint32_t stub_restore[] = {
    0xe3a0c40fu,                        /*  0 mov r12, #PV_BASE */
    0xe59c1000u,                        /*  1 ldr r1, [r12]: V */
    0xe0211000u,                        /*  2 eor r1, r1, r0 */
    0xe311001fu,                        /*  3 tst r1, #0x1f */
    0x1a000006u,                        /*  4 bne 12: another mode */
    0xe7e81050u,                        /*  5 ubfx r1, r0, #0, #9 */
    0xe58c1000u,                        /*  6 str r1, [r12]: V */
    0xe59c1004u,                        /*  7 ldr r1, [r12, #4]: P */
    0xe1d11000u,                        /*  8 bics r1, r1, r0: pending and now enabled? */
    0x1a000002u,                        /*  9 bne 13 */
    0xe128f000u,                        /* 10 msr APSR_nzcvq, r0: the flags (user mode may write them) */
    0xe12fff1eu,                        /* 11 bx lr */
    TRAP_MODE,                          /* 12 */
    TRAP_DELIVER,                       /* 13 */
};

static volatile uint32_t *g_page;

int pv_init(void) {
    void *p = mmap((void *)(uintptr_t)PV_BASE, PV_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    if ((uintptr_t)p != PV_BASE) {                      /* the hint was not free: no paravirtualization */
        munmap(p, PV_SIZE);
        return -1;
    }
    memcpy((uint8_t *)p + (PV_DISABLE - PV_BASE), stub_disable, sizeof stub_disable);
    memcpy((uint8_t *)p + (PV_RESTORE - PV_BASE), stub_restore, sizeof stub_restore);
    __builtin___clear_cache((char *)p, (char *)p + PV_SIZE);
    g_page = p;
    return 0;
}

int pv_active(void) { return g_page != NULL; }

int pv_patch(uint32_t *image, size_t words) {
    static const uint32_t fw[6] = { 0xe12ff000u, 0xe1a0f00eu, 0xe10f0000u, 0xe3801080u, 0xe12ff001u, 0xe1a0f00eu };
    if (words < IRQ_DISABLE / 4 + 4 || memcmp(&image[IRQ_RESTORE / 4], fw, sizeof fw)) return 0;
    image[IRQ_RESTORE / 4] = LDR_PC_NEXT;
    image[IRQ_RESTORE / 4 + 1] = PV_RESTORE;
    image[IRQ_DISABLE / 4] = LDR_PC_NEXT;
    image[IRQ_DISABLE / 4 + 1] = PV_DISABLE;
    return 2;
}

int pv_in_page(uint32_t pc) {
    return pc - PV_BASE < PV_SIZE;
}

/* msr CPSR_fsxc, r0, then return to lr (of the new mode, as the firmware's mov pc, lr) */
static void restore(struct vcpu *v, struct arm_cpu *c, const struct arm_bus *bus) {
    vcpu_emu_priv(v, c, MSR_CPSR_R0, bus);
    c->r[15] = c->r[14];
}

int pv_emulate(struct vcpu *v, struct arm_cpu *c, const struct arm_bus *bus) {
    if (c->r[15] == PV_DISABLE) {
        uint32_t old = vcpu_cpsr(v, c);
        c->r[0] = old;
        c->r[1] = old | VCPU_I;
        v->ctl |= VCPU_I;
        c->r[15] = c->r[14];
        return 1;
    }
    if (c->r[15] == PV_RESTORE) {
        restore(v, c, bus);
        return 1;
    }
    return 0;
}

int pv_trap(struct vcpu *v, struct arm_cpu *c, const struct arm_bus *bus) {
    uint32_t i = (c->r[15] - PV_RESTORE) / 4;
    if (c->r[15] < PV_RESTORE || i >= sizeof stub_restore / 4) return 0;
    if (stub_restore[i] == TRAP_MODE) {                 /* V is unchanged */
        restore(v, c, bus);
        return 1;
    }
    if (stub_restore[i] == TRAP_DELIVER) {              /* V is in v (pv_sync_in): the flags remain */
        c->cpsr = (c->cpsr & ~CPSR_FLAGS) | (c->r[0] & CPSR_FLAGS);
        c->r[15] = c->r[14];
        return 1;
    }
    return 0;
}

void pv_sync_in(struct vcpu *v) {
    if (!g_page) return;
    v->ctl = (v->ctl & ~VCPU_CTL_MASK) | (g_page[PV_V] & VCPU_CTL_MASK);
}

void pv_sync_out(const struct vcpu *v, uint32_t pending) {
    if (!g_page) return;
    g_page[PV_V] = v->mode | v->ctl;
    g_page[PV_P] = pending;
}
