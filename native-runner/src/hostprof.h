/* Sampling profile of the runner's own code (HOSTPROF=1). See hostprof.c. */
#ifndef HOSTPROF_H
#define HOSTPROF_H
#include <stdint.h>

void hostprof_start(void);
void hostprof_print(void);
void hostprof_jit_range(uint32_t lo, uint32_t hi);   /* the JIT code buffer: its own bucket */

#endif
