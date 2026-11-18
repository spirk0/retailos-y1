/* The machine as the guest CPU glue (cpu.c) sees it: guest memory at any
 * guest address, and the entry points of the CPU glue. machine.c implements
 * the memory side; the devices are behind dev.h. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
#include <stdio.h>
#include "vcpu.h"

/* Guest data access at any guest address: RAM and its aliases, page 0,
 * IRAM, device registers. size is 1, 2 or 4. An address with nothing
 * behind it reads 0 and ignores a write. Always returns 0. */
int mach_mem_read(uint32_t addr, int size, uint32_t *val);
int mach_mem_write(uint32_t addr, int size, uint32_t val);

/* One entry for each 64 KB block of the guest address space: 1 if the host
 * has guest RAM at the same address (see interp_env.direct). */
extern uint8_t mach_direct[1u << 16];

/* Instruction word at a guest pc. Returns 0 if pc is not guest RAM. */
int mach_fetch(uint32_t pc, uint32_t *word);

/* The host cannot execute at guest page 0 (it cannot map it). Returns an
 * address where the host can execute the same code, or 0 if pc is not such
 * an address. */
uint32_t mach_exec_alias(uint32_t pc);

/* PROBE (debug): 1 if pc is a probe address that the guest reached now. The
 * original word is back in memory: execute pc again. */
int mach_probe_hit(uint32_t pc, uint32_t lr);

/* 1 if the guest started from reset (a capture or a snapshot has no COP state) */
int mach_cold_boot(void);

/* The COP's part of a snapshot (cpu.c). Returns 0 on success. */
int cpu_cop_snapshot(FILE *f, int save);

/* Guest time continues at us (after a snapshot load, cpu.c) */
void cpu_set_guest_time_us(uint64_t us);

/* Slow work, a few times per second: key input, frame file, snapshot. */
void mach_housekeeping(const struct arm_cpu *c, const struct vcpu *v);

/* ---- guest CPU glue (cpu.c) ---- */

/* Guest time in microseconds: host time without the cost of the emulation */
uint64_t cpu_guest_time_us(void);

/* Start the guest. Does not return. */
void cpu_run(const struct arm_cpu *start, const struct vcpu *v);

#endif
