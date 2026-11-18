/* HOSTPROF=1: a sampling profile of the runner itself (the target kernel
 * has no perf events). SIGPROF arrives every 500 us of process CPU time and
 * records the host pc. hostprof_print() writes the most frequent pcs as
 * offsets from the start of the executable (__executable_start).
 * addr2line -f -e build/runner (NDK) names the functions. */
#define _GNU_SOURCE
#include "hostprof.h"
#include "runner.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define BINS        (1u << 17)          /* 4-byte bins: 512 KB of runner code */
#define PERIOD_US   500
#define TOP         400

extern char __executable_start[];   /* from the linker */

static uint32_t g_bins[BINS];
static uint32_t g_guest, g_other, g_total, g_jit;
static uint32_t g_jit_lo, g_jit_hi;
#define OTHER_BINS 4096                     /* other pcs, in 256-byte bins, hashed */
static uint32_t g_other_pc[OTHER_BINS], g_other_n[OTHER_BINS];
static int g_on;

static int is_guest_pc(uint32_t pc) {
    return pc < 0x08000000u || (pc >= GUEST_DRAM_BASE && pc < 0x18000000u) ||
           (pc >= GUEST_IRAM_BASE && pc < GUEST_IRAM_BASE + GUEST_IRAM_SIZE) || (pc >> 28) == 0x9;
}

static void on_prof(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    uint32_t pc = (uint32_t)uc->uc_mcontext.arm_pc, base = (uint32_t)(uintptr_t)__executable_start;
    (void)sig; (void)si;
    g_total++;
    if (is_guest_pc(pc)) g_guest++;
    else if (pc - g_jit_lo < g_jit_hi - g_jit_lo) g_jit++;
    else if (pc - base < BINS * 4) g_bins[(pc - base) / 4]++;
    else {                              /* libc, the vdso: keep the address */
        uint32_t k = (pc >> 8) % OTHER_BINS;
        g_other++;
        g_other_pc[k] = pc & ~0xffu;
        g_other_n[k]++;
    }
}

void hostprof_jit_range(uint32_t lo, uint32_t hi) {
    g_jit_lo = lo;
    g_jit_hi = hi;
}

void hostprof_start(void) {
    struct sigaction sa;
    char line[256];
    FILE *maps = fopen("/proc/self/maps", "r");         /* to name the "other" addresses */
    while (maps && fgets(line, sizeof line, maps))
        if (strstr(line, " r-xp ")) fprintf(stderr, "hostprof map: %s", line);
    if (maps) fclose(maps);
    struct itimerval it = { { 0, PERIOD_US }, { 0, PERIOD_US } };
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    /* the timer signal must not come in this handler: cpu.c would take its
     * host registers for the guest's (and swap them with the other core's) */
    sigaddset(&sa.sa_mask, SIGALRM);
    sigaction(SIGPROF, &sa, NULL);
    setitimer(ITIMER_PROF, &it, NULL);
    g_on = 1;
}

void hostprof_print(void) {
    if (!g_on || !g_total) return;
    fprintf(stderr, "hostprof (%u samples): guest %u%% jit %u%% other %u%%", g_total, g_guest * 100 / g_total,
            g_jit * 100 / g_total, g_other * 100 / g_total);
    for (int k = 0; k < TOP; k++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < BINS; i++) if (g_bins[i] > g_bins[best]) best = i;
        if (!g_bins[best]) break;
        fprintf(stderr, " +%x:%u", best * 4, g_bins[best] * 1000 / g_total);
        g_bins[best] = 0;
    }
    fprintf(stderr, " (per mille)\n hostprof other:");
    for (int k = 0; k < 8; k++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < OTHER_BINS; i++) if (g_other_n[i] > g_other_n[best]) best = i;
        if (!g_other_n[best]) break;
        fprintf(stderr, " %08x:%u", g_other_pc[best], g_other_n[best] * 1000 / g_total);
        g_other_n[best] = 0;
    }
    fprintf(stderr, "\n");
    memset(g_bins, 0, sizeof g_bins);
    memset(g_other_n, 0, sizeof g_other_n);
    g_guest = g_other = g_total = g_jit = 0;
}
