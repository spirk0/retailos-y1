/* native-runner: run the iPod 5G (PP502x) firmware natively on an ARMv7 host.
 *
 * The host CPU executes the guest's ARM code directly, in a user-mode
 * process. Guest RAM is mapped at the guest's own addresses. Everything the
 * host CPU cannot do for the guest arrives as a signal: cpu.c handles it,
 * with the machine in machine.c (memory map, loader) and the device models
 * in dev_*.c.
 */
#ifndef RUNNER_H
#define RUNNER_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* Guest physical map (PP5022), from qemu-david include/hw/arm/ipod_video/ipod_video.h */
#define GUEST_DRAM_BASE   0x10000000u
#define GUEST_DRAM_SIZE   0x04000000u   /* 64 MB */
#define GUEST_IRAM_BASE   0x40000000u
#define GUEST_IRAM_SIZE   0x00020000u   /* 128 KB */

struct runner;

struct runner *runner_create(void);
int  runner_load_osos(struct runner *r, const char *path);
int  runner_load_disk(struct runner *r, const char *path);
/* VDISK=<folder>: the disk from a folder (see machine.c). Returns 0 on success. */
int  runner_open_vdisk(struct runner *r);
void runner_run(struct runner *r);          /* never returns until exit */
void runner_destroy(struct runner *r);

#endif
