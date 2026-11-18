/* Interpreter speed benchmark. Runs bench/guest.c (build/guest.bin) with
 * interp_step() and, on an ARM host, natively. Prints ns per instruction.
 * usage: bench <guest.bin> [rounds] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include "../src/interp.h"
#include "../src/jit.h"

#define MEM_BASE  0x10000000u
#define MEM_SIZE  0x00100000u
#define TEXT      0x10010094u     /* .text of build/guest.elf */
#define ENTRY     0x100100d8u     /* bench() */
#define BUF       0x10080000u
#define STACK     0x100f0000u
#define STOP      0x10000000u     /* return address: the run ends here */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint8_t *ptr(uint32_t addr, int size) {
    if (addr - MEM_BASE >= MEM_SIZE || addr - MEM_BASE + (uint32_t)size > MEM_SIZE) return NULL;
    return (uint8_t *)(uintptr_t)addr;
}
static int bus_read(void *ctx, uint32_t addr, int size, uint32_t *val) {
    uint8_t *p = ptr(addr, size);
    uint32_t v = 0;
    (void)ctx;
    if (!p) return -1;
    memcpy(&v, p, (size_t)size);
    *val = v;
    return 0;
}
static int bus_write(void *ctx, uint32_t addr, int size, uint32_t val) {
    uint8_t *p = ptr(addr, size);
    (void)ctx;
    if (!p) return -1;
    memcpy(p, &val, (size_t)size);
    return 0;
}
static const struct arm_bus bus = { bus_read, bus_write, 0 };
static int fetch(void *ctx, uint32_t pc, uint32_t *w) { return bus_read(ctx, pc, 4, w) == 0; }
static void semihost(void *ctx, struct arm_cpu *c) { (void)ctx; c->r[15] += 4; }
static uint8_t direct[1u << 16];
static struct interp_env env = { &bus, fetch, semihost, 0, 0 };

static void reset_buf(void) {
    for (uint32_t i = 0; i < 64; i++) ((uint32_t *)(uintptr_t)BUF)[i] = i * 0x01010101u + 0x37;
}

int main(int argc, char **argv) {
    int rounds = argc > 2 ? atoi(argv[2]) : 20000;
    if (argc < 2) { fprintf(stderr, "usage: bench <guest.bin> [rounds]\n"); return 2; }
    if (mmap((void *)(uintptr_t)MEM_BASE, MEM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) { perror("mmap"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread((void *)(uintptr_t)TEXT, 1, 0x10000, f);
    fclose(f);
    __builtin___clear_cache((char *)(uintptr_t)TEXT, (char *)(uintptr_t)TEXT + n);

    if (jit_init()) { fprintf(stderr, "jit_init failed\n"); return 1; }
    for (int mode = 0; mode < 3; mode++) {
    struct vcpu v;
    struct arm_cpu c;
    env.direct = mode ? direct : NULL;                  /* 0: bus, 1: direct, 2: JIT */
    for (uint32_t b = MEM_BASE >> 16; b < (MEM_BASE + MEM_SIZE) >> 16; b++) direct[b] = 1;
    memset(&c, 0, sizeof c);
    vcpu_reset(&v, VCPU_SVC);
    reset_buf();
    *(uint32_t *)(uintptr_t)STOP = 0xee110f10u;   /* mrc: the interpreter stops there */
    c.r[0] = BUF; c.r[1] = (uint32_t)rounds; c.r[13] = STACK; c.r[14] = STOP; c.r[15] = ENTRY;
    c.cpsr = VCPU_SVC;
    uint64_t insns = 0, t0 = now_ns();
    while (c.r[15] != STOP) {
        /* batches of 64, as cpu.c runs them */
        struct interp_run run = { 64, 256, 0, 0, 0, 0 };
        int rc = mode == 2 ? jit_run(&v, &c, &env, &run) : interp_run(&v, &c, &env, &run);
        insns += run.n;
        if (rc != INTERP_OK && c.r[15] != STOP) { fprintf(stderr, "stop rc=%d pc=%08x\n", rc, c.r[15]); return 1; }
    }
    uint64_t t = now_ns() - t0;
    uint32_t isum = c.r[0];
    printf("%s: %llu insns, %.1f ns/insn, result %08x\n",
           mode == 2 ? "jit" : mode ? "interp (direct)" : "interp (bus)",
           (unsigned long long)insns, (double)t / (double)insns, isum);
    if (mode == 2) {
#ifdef __arm__
    reset_buf();
    unsigned (*fn)(unsigned *, int) = (unsigned (*)(unsigned *, int))(uintptr_t)ENTRY;
    t0 = now_ns();
    uint32_t nsum = fn((unsigned *)(uintptr_t)BUF, rounds);
    t = now_ns() - t0;
    printf("native: %.2f ns/insn, result %08x %s\n", (double)t / (double)insns, nsum,
           nsum == isum ? "(same)" : "(DIFFERENT)");
#endif
    }
    }
    return 0;
}
