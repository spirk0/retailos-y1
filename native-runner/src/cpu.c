/* Guest CPU glue.
 *
 * The guest (ARMv4T/ARMv5 firmware) runs natively in this user-mode process.
 * What the host CPU cannot do for it arrives here as a signal:
 *
 *   SIGILL   a privileged instruction (MRS/MSR, exception return) or a
 *            semihosting call. The loader replaced it with a trap word
 *            (privtrap.h).
 *   SIGSEGV  a data access to a device register or to guest page 0, or an
 *   SIGBUS   instruction fetch in page 0, which the host cannot map (the
 *            code then runs through an alias of the same memory).
 *   SIGALRM  a periodic check of the guest timers, so that an interrupt
 *            arrives while the guest runs code that does not trap.
 *
 */
#define _GNU_SOURCE
#include "machine.h"
#include "runner.h"
#include "dev.h"
#include "interp.h"
#include "jit.h"
#include "privtrap.h"
#include "pv.h"
#include "hostprof.h"
#include "timeconv.h"
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#define TICK_US             1000u   /* period of the asynchronous timer check */
#define STATUS_PERIOD_US    2000000u
#define HOUSEKEEP_PERIOD_US 5000u   /* key input and frame output: a key waits at most this long */
#define SLEEP_MAX_US        2000u   /* longest wait step of a sleeping core */
#define SLEEP_MIN_US        50u
#define RING_SIZE           64
#define RUNAHEAD_IDLE       1024    /* stop after this many instructions that would run natively */
#define RUNAHEAD_MAX        200000  /* most instructions in one handler */
#define TIMER_CHECK_INSNS   64      /* timer and IRQ check interval in the handler. With 256 the
                                     * cold boot got an IRQ storm on timer line 0 (run cb4). */
#define NS_PER_INSN         4       /* guest time of one interpreted instruction (QEMU -icount shift=2) */
#define CALIBRATE_TRAPS     2000
#define PAGE0_END           0x1000u /* the host cannot map guest page 0 */

static struct vcpu g_vcpu;
static struct arm_cpu g_start;
static uint64_t g_stolen_ns;        /* host time that is not guest time */
static uint64_t g_frozen_at;        /* host time (ns) at handler entry, 0 while the guest runs */
static uint64_t g_kernel_ns;        /* calibrated: kernel time of one signal round trip */
static uint64_t g_n_trap, g_n_interp, g_n_exec, g_n_irq, g_n_sleep, g_n_stop;
static int g_in_handler;
static uint64_t g_time0;

/* TASKLOG=1 (debug): log each guest task the first time it runs. The
 * retailOS kernel keeps the current task at CURRENT_TASK. */
#define CURRENT_TASK 0x1081db00u
#define MAX_TASKS    128
static int g_tasklog;
static uint32_t g_tasks[MAX_TASKS], g_n_tasks, g_last_task;

static void task_check(void) {
    uint32_t t, w[8];
    if (mach_mem_read(CURRENT_TASK, 4, &t) || t == g_last_task) return;
    g_last_task = t;
    for (uint32_t i = 0; i < g_n_tasks; i++) if (g_tasks[i] == t) return;
    if (g_n_tasks < MAX_TASKS) g_tasks[g_n_tasks++] = t;
    for (int i = 0; i < 8; i++) mach_mem_read(t + 4u * (uint32_t)i, 4, &w[i]);
    fprintf(stderr, "task: #%u %08x first at guest %llu us: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            g_n_tasks, t, (unsigned long long)(cpu_guest_time_us() - g_time0),
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
}
static int g_jit_on;
static uint32_t g_idle_limit = RUNAHEAD_IDLE;

static struct { uint32_t pc, addr; int sig; } g_ring[RING_SIZE];
static unsigned g_ring_i;

/* PROFILE=1: guest pcs at each trap and timer signal, in 64-byte blocks */
#define PROF_BINS 65536
static uint32_t g_prof[PROF_BINS];
static int g_prof_on;

static uint64_t host_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* After a snapshot load: guest time continues at the time of the snapshot.
 * The devices keep their deadlines as absolute guest times (DMA, IDE,
 * I2S): with a new start value they never came, and the music stopped. */
void cpu_set_guest_time_us(uint64_t us) {
    g_stolen_ns = host_now_ns() - us * 1000u;
}

uint64_t cpu_guest_time_us(void) {
    return ns_to_us((g_frozen_at ? g_frozen_at : host_now_ns()) - g_stolen_ns);
}

/* Handler entry: guest time stops. The kernel's part of the time since the
 * last handler exit is not guest time either. */
static void freeze(void) {
    g_frozen_at = host_now_ns();
    g_stolen_ns += g_kernel_ns;
}

/* Handler exit. The handler time is not guest time; the interpreted
 * instructions in it were credited with credit(). */
static uint64_t g_ns_handler, g_ns_sleep, g_n_handler;    /* for the status line */
static uint64_t g_ns_catchup;                               /* guest time that catch_up() added */
static uint64_t g_ns_cop;                                   /* host time in cop_slice() */

static void thaw(void) {
    uint64_t d = host_now_ns() - g_frozen_at;
    g_stolen_ns += d;
    g_ns_handler += d;
    g_n_handler++;
    g_frozen_at = 0;
}

/* inside a handler: interpreted instructions are guest time */
static void credit(uint64_t insns) {
    g_stolen_ns -= insns * NS_PER_INSN;
}

static void prof_sample(uint32_t pc) {
    g_prof[((pc & 0x03ffffffu) >> 6) % PROF_BINS]++;
}

static void prof_print(void) {
    uint32_t total = 0;
    for (int i = 0; i < PROF_BINS; i++) total += g_prof[i];
    if (!total) return;
    fprintf(stderr, "prof (%u samples):", total);
    for (int k = 0; k < 16; k++) {
        int best = 0;
        for (int i = 1; i < PROF_BINS; i++) if (g_prof[i] > g_prof[best]) best = i;
        if (!g_prof[best]) break;
        fprintf(stderr, " %06x:%u%%", best << 6, g_prof[best] * 100 / total);
        g_prof[best] = 0;
    }
    fprintf(stderr, "\n");
    memset(g_prof, 0, sizeof g_prof);
}

static void uc_to_cpu(const ucontext_t *uc, struct arm_cpu *c) {
    const unsigned long *r = &uc->uc_mcontext.arm_r0;   /* r0..r10, fp, ip, sp, lr, pc */
    for (int i = 0; i < 16; i++) c->r[i] = (uint32_t)r[i];
    c->cpsr = (uint32_t)uc->uc_mcontext.arm_cpsr;
}

static void cpu_to_uc(const struct arm_cpu *c, ucontext_t *uc) {
    unsigned long *r = &uc->uc_mcontext.arm_r0;
    for (int i = 0; i < 16; i++) r[i] = c->r[i];
    /* only the flags, GE and T belong to the guest: the host stays in user mode */
    uc->uc_mcontext.arm_cpsr = (uc->uc_mcontext.arm_cpsr & ~(unsigned long)VCPU_HOST_MASK)
                             | (c->cpsr & VCPU_HOST_MASK);
}

/* ---------- the COP ----------
 * The PP502x has a second core, the COP. The retailOS starts it during the
 * boot (COP_CTL) and runs its audio setup and the audio decoder on it. The
 * runner runs the COP on this host thread. While the CPU is awake, the COP
 * runs through the JIT in slices: at the timer checks of run_ahead() and on
 * the timer signal. While the CPU sleeps, the COP runs natively, as the CPU
 * does (schedule(): the handler parks the CPU's registers and returns into
 * the COP's code), until a CPU line is active or the COP sleeps. In the JIT
 * the decoder took about 450 ms of each 2 s of playback. NATIVE_COP=0 runs
 * the COP through the JIT only, also while the CPU sleeps. After a cold
 * boot, or after a snapshot that has the COP state (cpu_cop_snapshot()); a
 * QEMU capture has none. */

#define COP_SLICE_TICK   4096u      /* on the timer signal and in the sleep loop */
#define COP_SLICE_CHECK  64u        /* at each timer check of run_ahead() */
/* Instructions of the COP between its timer and interrupt checks. The guest
 * clock stands in a slice except for the 4 ns credit per instruction, so a
 * check after 64 instructions found nothing new: 1024 make 4 us. */
#define COP_RUN_INSNS    1024u
#define CATCHUP_MAX_NS   1000000000ull  /* lag of guest time that a CPU sleep makes up; older lag stays */

static struct arm_cpu g_cop;
static struct vcpu g_cop_v;
static int g_cop_on, g_cop_live, g_cop_dead;
static uint64_t g_n_cop;
static int g_native_cop_on;         /* the COP may run natively while the CPU sleeps */
static int g_native_cop;            /* 1: the COP runs natively now, the CPU is parked */
static struct arm_cpu g_cpu_parked; /* the CPU's registers while the COP runs natively */
static const struct arm_cpu *g_cop_now;    /* the COP's registers at mach_housekeeping() */
static int g_cop_from_snap;         /* the COP state came from a snapshot */
static uint64_t g_n_native_cop;     /* switches to the native COP */

static int cop_need_stop(void *ctx) { (void)ctx; return proc_cop_sleeping(); }
static int cop_irq_ready(void *ctx) {
    (void)ctx;
    return (intc_cop_fiq() && vcpu_fiq_enabled(&g_cop_v)) || (intc_cop_irq() && vcpu_irq_enabled(&g_cop_v));
}

static void cop_deliver_irq_on(struct arm_cpu *c) {
    if (intc_cop_fiq() && vcpu_fiq_enabled(&g_cop_v)) vcpu_take_fiq(&g_cop_v, c);
    else if (intc_cop_irq() && vcpu_irq_enabled(&g_cop_v)) vcpu_take_irq(&g_cop_v, c);
}

static void cop_deliver_irq(void) { cop_deliver_irq_on(&g_cop); }

/* The COP's part of a snapshot (machine.c): its registers, its virtual CPU
 * and its state. Saved from mach_housekeeping(), where the COP can run
 * natively (then its registers are the native ones). Returns 0 on success. */
int cpu_cop_snapshot(FILE *f, int save) {
    struct arm_cpu c = save && g_cop_now ? *g_cop_now : g_cop;
    int32_t flags[3] = { g_cop_on, g_cop_live, g_cop_dead };
    if (snap_rw(f, &c, sizeof c, save) || snap_rw(f, &g_cop_v, sizeof g_cop_v, save) ||
        snap_rw(f, flags, sizeof flags, save)) return -1;
    if (!save) {
        g_cop = c;
        g_cop_live = flags[1];
        g_cop_dead = flags[2];
        g_cop_from_snap = flags[0];
    }
    return 0;
}

/* out of reset: the same vector as the CPU */
static void cop_init(void) {
    if (g_cop_live) return;
    memset(&g_cop, 0, sizeof g_cop);
    g_cop.r[15] = GUEST_DRAM_BASE;
    vcpu_reset(&g_cop_v, VCPU_SVC | VCPU_I | VCPU_F);
    g_cop_live = 1;
    fprintf(stderr, "cop: out of reset at %08x\n", g_cop.r[15]);
}

/* 1 if the COP has work: started, alive, and awake or woken by its IRQ */
static int cop_runnable(void) {
    if (!g_cop_on || g_cop_dead || !proc_cop_started()) return 0;
    if (proc_cop_sleeping()) {
        if (!intc_cop_irq() && !intc_cop_fiq()) return 0;
        proc_cop_wake();                                /* an active line wakes a sleeping core */
    }
    return 1;
}

static void cop_slice(uint32_t budget);

/* the guest registers that run_ahead() interprets: r15 is the pc of an access */
static const struct arm_cpu *g_cur;

static int bus_read(void *ctx, uint32_t addr, int size, uint32_t *val) {
    (void)ctx;
    dev_set_access_pc(g_cur ? g_cur->r[15] : 0);
    return mach_mem_read(addr, size, val);
}
static int bus_write(void *ctx, uint32_t addr, int size, uint32_t val) {
    (void)ctx;
    dev_set_access_pc(g_cur ? g_cur->r[15] : 0);
    return mach_mem_write(addr, size, val);
}
static const struct arm_bus g_bus = { bus_read, bus_write, 0 };

static int env_fetch(void *ctx, uint32_t pc, uint32_t *word) {
    (void)ctx;
    return mach_fetch(pc, word);
}

/* ARM semihosting (svc 0x123456). The firmware's C library uses it for its
 * console, and that is where its panic messages go. Console output goes to
 * the log; the file operations give the answers of a console device. */
static void env_semihost(void *ctx, struct arm_cpu *c) {
    static char line[200];
    static unsigned n;
    uint32_t op = c->r[0], arg = c->r[1], w[3] = { 0, 0, 0 }, ch = 0, result = 0, len = 0, ptr = 0;
    (void)ctx;
    switch (op) {
    case 0x03: ptr = arg; len = 1; break;                           /* SYS_WRITEC */
    case 0x04:                                                      /* SYS_WRITE0 */
        ptr = arg;
        while (len < 1024 && !mach_mem_read(arg + len, 1, &ch) && ch) len++;
        break;
    case 0x05:                                                      /* SYS_WRITE: handle, pointer, length */
        for (int i = 0; i < 3; i++) mach_mem_read(arg + 4 * (uint32_t)i, 4, &w[i]);
        ptr = w[1];
        len = w[2] > 1024 ? 1024 : w[2];
        break;
    case 0x01: case 0x09: result = 1; break;                        /* SYS_OPEN, SYS_ISTTY */
    case 0x18: fprintf(stderr, "guest: exit through semihosting (reason %08x)\n", arg); break;
    default: break;
    }
    for (uint32_t i = 0; i < len; i++) {
        mach_mem_read(ptr + i, 1, &ch);
        if (ch != '\n' && n < sizeof line - 1) { line[n++] = (char)ch; continue; }
        line[n] = 0;
        fprintf(stderr, "guest: %s\n", line);
        n = 0;
    }
    c->r[0] = result;
    c->r[15] += 4;
}

/* after a device write: the batch stops only if the write stopped the core */
static int env_need_stop(void *ctx) {
    (void)ctx;
    return proc_cpu_sleeping();
}

/* page 0 is the same DRAM as its alias at 0x04000000 (machine.c LOW_ALIAS):
 * the direct table marks that block only after the remap */
/* after a privileged instruction: the batch stops only if an IRQ can come in */
static int cpu_int_ready(void) {
    return (intc_cpu_fiq() && vcpu_fiq_enabled(&g_vcpu)) || (intc_cpu_irq() && vcpu_irq_enabled(&g_vcpu));
}

static int env_irq_ready(void *ctx) {
    (void)ctx;
    return cpu_int_ready();
}

static const struct interp_env g_env = { &g_bus, env_fetch, env_semihost, 0, mach_direct, env_need_stop,
                                         0x04000000u, env_irq_ready };

static const struct interp_env g_cop_env = { &g_bus, env_fetch, env_semihost, 0, mach_direct, cop_need_stop,
                                             0x04000000u, cop_irq_ready };

static void print_state(const struct arm_cpu *c) {
    static const char *const name[VCPU_BANKS] = { "usr", "fiq", "irq", "svc", "abt", "und" };
    char dev[384];
    for (int i = 0; i < 16; i++) fprintf(stderr, " r%d=%08x", i, c->r[i]);
    fprintf(stderr, "\n host cpsr=%08x guest cpsr=%08x", c->cpsr, vcpu_cpsr(&g_vcpu, c));
    for (int b = 0; b < VCPU_BANKS; b++)
        fprintf(stderr, " %s:sp=%08x,lr=%08x,spsr=%08x", name[b], g_vcpu.sp[b], g_vcpu.lr[b], g_vcpu.spsr[b]);
    dev_status(dev, sizeof dev);
    fprintf(stderr, "\n traps=%llu interp=%llu exec=%llu irq=%llu | %s\n", (unsigned long long)g_n_trap,
            (unsigned long long)g_n_interp, (unsigned long long)g_n_exec, (unsigned long long)g_n_irq, dev);
}

static void fatal(const char *why, const struct arm_cpu *c, int sig, uint32_t addr) {
    uint32_t word = 0;
    mach_fetch(c->r[15], &word);
    fprintf(stderr, "FATAL: %s (sig=%d addr=%08x pc=%08x insn=%08x)%s\n", why, sig, addr, c->r[15], word,
            g_native_cop ? " on the COP" : "");
    print_state(c);
    if (interp_pc_ring_on) {
        fprintf(stderr, " last interpreted pcs (oldest first, +1: IRQ mode):");
        for (unsigned k = 0; k < INTERP_PC_RING; k++) {
            uint32_t p = interp_pc_ring[(interp_pc_ring_i + k) % INTERP_PC_RING];
            if (p) fprintf(stderr, " %x", p);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, " last traps (oldest first, sig:pc:addr):");
    for (unsigned k = 0; k < RING_SIZE; k++) {
        unsigned j = (g_ring_i + k) % RING_SIZE;
        if (g_ring[j].sig) fprintf(stderr, " %d:%08x:%08x", g_ring[j].sig, g_ring[j].pc, g_ring[j].addr);
    }
    fprintf(stderr, "\n");
    _exit(2);
}

/* Take the FIQ or the IRQ exception if a line is active and the guest lets
 * it in. An FIQ comes first, as on the ARM core. */
static void deliver_irq(struct arm_cpu *c) {
    if (intc_cpu_fiq() && vcpu_fiq_enabled(&g_vcpu)) vcpu_take_fiq(&g_vcpu, c);
    else if (intc_cpu_irq() && vcpu_irq_enabled(&g_vcpu)) vcpu_take_irq(&g_vcpu, c);
    else return;
    g_n_irq++;
}

/* now_ns: the host time if the caller has it, else 0 */
static void periodic(const struct arm_cpu *native, uint64_t now_ns) {
    static uint64_t last_status, last_housekeep;
    const struct arm_cpu *c = g_native_cop ? &g_cpu_parked : native;      /* the CPU */
    const struct arm_cpu *cop = g_native_cop ? native : &g_cop;
    uint64_t now = ns_to_us(now_ns ? now_ns : host_now_ns());
    if (now - last_housekeep >= HOUSEKEEP_PERIOD_US) {
        last_housekeep = now;
        g_cop_now = cop;
        mach_housekeeping(c, &g_vcpu);
        g_cop_now = NULL;
    }
    if (now - last_status >= STATUS_PERIOD_US) {
        char dev[384];
        last_status = now;
        dev_status(dev, sizeof dev);
        fprintf(stderr, "st pc=%08x cpsr=%08x traps=%llu interp=%llu exec=%llu stop=%llu irq=%llu sleep=%llu"
                " gt=%llums jit=%llu/%llu/%llu/%llu/%llu | %s\n",
                c->r[15], vcpu_cpsr(&g_vcpu, c), (unsigned long long)g_n_trap, (unsigned long long)g_n_interp,
                (unsigned long long)g_n_exec, (unsigned long long)g_n_stop, (unsigned long long)g_n_irq,
                (unsigned long long)g_n_sleep, (unsigned long long)((cpu_guest_time_us() - g_time0) / 1000),
                (unsigned long long)jit_n_blocks, (unsigned long long)jit_n_translated,
                (unsigned long long)jit_n_flushes, (unsigned long long)jit_n_stale,
                (unsigned long long)jit_n_collide, dev);
        fprintf(stderr, "time: handlers %llu (%llu ms, of it asleep %llu ms, COP %llu ms, caught up %llu ms) jit: step %llu"
                " read %llu write %llu link %llu validate %llu priv %llu cop %llu at %08x%s native %llu presents %llu\n",
                (unsigned long long)g_n_handler, (unsigned long long)(g_ns_handler / 1000000u),
                (unsigned long long)(g_ns_sleep / 1000000u), (unsigned long long)(g_ns_cop / 1000000u),
                (unsigned long long)(g_ns_catchup / 1000000u),
                (unsigned long long)jit_n_step, (unsigned long long)jit_n_read, (unsigned long long)jit_n_write,
                (unsigned long long)jit_n_link, (unsigned long long)jit_n_validate,
                (unsigned long long)jit_n_priv, (unsigned long long)g_n_cop, cop->r[15],
                proc_cop_sleeping() ? " (asleep)" : "", (unsigned long long)g_n_native_cop,
                (unsigned long long)bcm_presents());
        g_ns_handler = g_ns_sleep = g_n_handler = g_ns_catchup = g_ns_cop = 0;
        if (g_cop_live) {                               /* debug: where the COP is */
            uint32_t w[12];
            for (int i = 0; i < 12; i++) mach_mem_read(cop->r[13] + 4u * (uint32_t)i, 4, &w[i]);
            fprintf(stderr, "cop: pc %08x lr %08x sp %08x cpsr %08x stack %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    cop->r[15], cop->r[14], cop->r[13], vcpu_cpsr(&g_cop_v, cop),
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
        }
        if (g_prof_on) prof_print();
        hostprof_print();
    }
}


/* Guest time lags real time by the emulation cost that it does not count
 * (the handlers, the COP). In a CPU sleep the lag is made up: guest time
 * jumps forward instead of a host sleep, so guest time keeps pace with real
 * time while the guest has idle time. Audio needs that: during playback the
 * COP decodes, and without the catch-up guest time ran at 64% of real time.
 * Only the lag of the last CATCHUP_MAX_NS is made up: after the boot, guest
 * time does not race through seconds of timer interrupts.
 * Returns the jump, at most want_ns. */
static uint64_t g_lag_ref;

static uint64_t catch_up(uint64_t want_ns) {
    uint64_t lag = g_stolen_ns + (host_now_ns() - g_frozen_at);     /* real time - guest time */
    if (lag > g_lag_ref + CATCHUP_MAX_NS) g_lag_ref = lag - CATCHUP_MAX_NS;
    if (lag <= g_lag_ref) return 0;
    uint64_t jump = lag - g_lag_ref < want_ns ? lag - g_lag_ref : want_ns;
    g_stolen_ns -= jump;
    g_ns_catchup += jump;
    return jump;
}

/* A read of the microsecond clock (dev_soc.c). Two reads in a handler
 * within CLOCK_POLL_NS of host time are a wait loop: the firmware waits
 * until the clock passes a value (the function at 0x2840b8). Each read is a
 * device access, so the loop runs in the handler, where guest time
 * advanced only 4 ns per instruction: the wait of 80 ms at 0x266090 during
 * wheel input took about 2 s of host time, and a game slowed down while
 * the wheel turned. On the device the clock runs while the core waits, so
 * here guest time makes up its lag, as in a sleep: the wait takes its real
 * time. As in a sleep, a jump ends at the next deadline: the next timer
 * check of the handler serves it. CLOCK_POLL=0 turns this off. */
#define CLOCK_POLL_NS 100000u

void cpu_clock_poll(void) {
    static uint64_t last;
    static int on = -1;
    if (on < 0) on = !(getenv("CLOCK_POLL") && !atoi(getenv("CLOCK_POLL")));
    if (!on || !g_frozen_at) return;                    /* off, or not in a handler */
    uint64_t now = host_now_ns();
    if (last && now - last < CLOCK_POLL_NS) {
        /* at most to the next deadline: a jump past the end of an audio
         * buffer restarted the stream late and lost 6% of the play time */
        uint64_t next = timers_next_deadline_us(), g = cpu_guest_time_us();
        if (!next) catch_up(~0ull);
        else if (next > g) catch_up((next - g) * 1000u);
    }
    last = now;
}

/* The guest stopped its core until the next interrupt: wait for a line.
 * Guest time runs during the wait. Returns 1 when a line is active, 0 when
 * the COP has work and may run natively: the caller parks the CPU. */
static int sleep_until_irq(const struct arm_cpu *c) {
    g_n_sleep++;
    for (;;) {
        uint64_t now = cpu_guest_time_us();
        timers_tick(now);
        if (intc_cpu_irq() || intc_cpu_fiq()) break;
        if (cop_runnable()) {                           /* the COP has work: no sleep */
            if (g_native_cop_on) return 0;
            cop_slice(COP_SLICE_TICK);
            periodic(c, 0);
            continue;
        }
        uint64_t next = timers_next_deadline_us();
        uint64_t wait = (next > now) ? next - now : SLEEP_MIN_US;
        if (!next || wait > SLEEP_MAX_US) wait = SLEEP_MAX_US;
        if (wait < SLEEP_MIN_US) wait = SLEEP_MIN_US;
        /* a jump to the deadline: check the timers again. A smaller jump made
         * up all the lag: sleep the rest. (A loop on small jumps made up only
         * its own cost each time, and never slept.) */
        uint64_t jump = catch_up((uint64_t)wait * 1000u);
        if (jump >= (uint64_t)wait * 1000u) continue;
        wait -= jump / 1000u;
        if (wait < SLEEP_MIN_US) wait = SLEEP_MIN_US;     /* guest time jumped: check the timers again */
        struct timespec ts = { 0, (long)wait * 1000 };
        uint64_t t0 = host_now_ns();
        nanosleep(&ts, NULL);
        uint64_t t1 = host_now_ns(), slept = t1 - t0;
        g_stolen_ns -= slept;                   /* the wait is guest time */
        g_ns_sleep += slept;
        periodic(c, t1);
    }
    proc_cpu_wake();
    return 1;
}

/* Run the COP for up to budget instructions. */
static void cop_slice(uint32_t budget) {
    const struct arm_cpu *saved = g_cur;
    uint32_t n = 0;
    if (!cop_runnable()) return;
    uint64_t t0 = host_now_ns();
    cop_init();
    g_cur = &g_cop;
    dev_set_core(1);
    while (n < budget && !proc_cop_sleeping()) {
        struct interp_run run;
        memset(&run, 0, sizeof run);
        run.budget = budget - n < COP_RUN_INSNS ? budget - n : COP_RUN_INSNS;
        cop_deliver_irq();
        int rc = g_jit_on ? jit_run(&g_cop_v, &g_cop, &g_cop_env, &run)
                          : interp_run(&g_cop_v, &g_cop, &g_cop_env, &run);
        n += run.n;
        if (rc == INTERP_STOP && pv_emulate(&g_cop_v, &g_cop, &g_bus)) {   /* a stub entry (pv.c) */
            n++;
            continue;
        }
        if (rc != INTERP_OK) {
            uint32_t w = 0;
            mach_fetch(g_cop.r[15], &w);
            fprintf(stderr, "cop: cannot run %08x at pc %08x (%s): the COP stops\n", w, g_cop.r[15],
                    (g_cop.cpsr & ARM_CPSR_T) ? "Thumb" : rc == INTERP_STOP ? "no emulation" : "undefined");
            g_cop_dead = 1;
            break;
        }
        timers_tick(cpu_guest_time_us());
    }
    dev_set_core(0);
    g_cur = saved;
    g_n_cop += n;
    credit(n);
    g_ns_cop += host_now_ns() - t0;
}

/* 1 if the parked CPU must run: a line is active, or the COP woke it */
static int cpu_wants_back(void) {
    return intc_cpu_irq() || intc_cpu_fiq() || !proc_cpu_sleeping();
}

/* the virtual CPU of the core that runs natively */
static struct vcpu *native_vcpu(void) { return g_native_cop ? &g_cop_v : &g_vcpu; }

/* Before the return to native code: the shadow state of the stubs (pv.c),
 * and the lines of the native core that a stub must not enable silently */
static void pv_out(void) {
    uint32_t pending = g_native_cop ? (intc_cop_irq() ? VCPU_I : 0) | (intc_cop_fiq() ? VCPU_F : 0)
                                    : (intc_cpu_irq() ? VCPU_I : 0) | (intc_cpu_fiq() ? VCPU_F : 0);
    pv_sync_out(native_vcpu(), pending);
}

/* 1 if pc is inside a stub of pv.c: only native code can continue there */
static int mid_stub(uint32_t pc) {
    return pv_in_page(pc) && pc != PV_DISABLE && pc != PV_RESTORE;
}

/* 1 if the CPU sleeps and the COP should run natively */
static int cop_wants_native(void) {
    return g_native_cop_on && cop_runnable() && !intc_cpu_irq() && !intc_cpu_fiq();
}

/* Interpret the code of a core (0: CPU, 1: COP) from c->r[15] while
 * trap-worthy instructions keep coming. The first instruction must execute
 * (it trapped, or it is an exception vector). Returns the number of
 * interpreted instructions. It also returns when the core sleeps and the
 * other core should run natively: schedule() switches. */
static uint64_t run_ahead(struct arm_cpu *c, int core, int sig, uint32_t addr) {
    struct vcpu *v = core ? &g_cop_v : &g_vcpu;
    const struct interp_env *env = core ? &g_cop_env : &g_env;
    uint64_t n = 0, credited = 0;
    uint32_t since_check = 0;
    struct interp_run run;
    memset(&run, 0, sizeof run);
    run.idle_limit = g_idle_limit;
    g_cur = c;
    dev_set_core(core);
    if (g_jit_on) jit_new_epoch();                      /* native code ran since the last handler */
    while (n < RUNAHEAD_MAX || c->r[15] < PAGE0_END) {     /* never return to native code in page 0 */
        run.budget = TIMER_CHECK_INSNS - since_check;
        int rc = g_jit_on ? jit_run(v, c, env, &run) : interp_run(v, c, env, &run);
        n += run.n;
        since_check += run.n;
        if (rc == INTERP_STOP && pv_in_page(c->r[15])) {
            if (!pv_emulate(v, c, &g_bus)) break;       /* inside a stub: native code continues */
            n++;
            since_check++;
            if (core) cop_deliver_irq_on(c);            /* a restore can let an interrupt in */
            else deliver_irq(c);
            continue;
        }
        if (g_tasklog) task_check();
        if (rc == INTERP_ERROR && mach_probe_hit(c->r[15], c->r[14])) {
            if (g_jit_on) jit_flush();                  /* the probe changed static code */
            continue;
        }
        if (rc == INTERP_ERROR) fatal("instruction cannot execute", c, sig, addr);
        if (rc == INTERP_STOP) {
            if (n == 0 && sig != SIGALRM) fatal("trapped instruction cannot be interpreted", c, sig, addr);
            g_n_stop++;
            break;
        }
        if (core) {                                     /* the native COP's code */
            if (proc_cop_sleeping()) break;             /* schedule() goes back to the CPU */
            if (since_check >= TIMER_CHECK_INSNS) {
                credit(n - credited);
                credited = n;
                timers_tick(cpu_guest_time_us());
                if (cpu_wants_back()) break;            /* schedule() goes back to the CPU */
                cop_deliver_irq_on(c);
                since_check = 0;
            } else if (run.stop) {
                cop_deliver_irq_on(c);
            }
        } else if (proc_cpu_sleeping()) {
            credit(n - credited);
            credited = n;
            if (cop_wants_native() || !sleep_until_irq(c)) break;  /* schedule() parks the CPU */
            deliver_irq(c);
            run.idle = 0;
            since_check = 0;
        } else if (since_check >= TIMER_CHECK_INSNS) {
            credit(n - credited);
            credited = n;
            timers_tick(cpu_guest_time_us());
            cop_slice(COP_SLICE_CHECK);
            deliver_irq(c);
            since_check = 0;
        } else if (run.stop) {
            /* An MSR or a device write can let an IRQ in. The guest's idle
             * loop unmasks IRQs for a few instructions only: a check at a
             * fixed interval can miss that window every time. */
            deliver_irq(c);
        }
        if (run.idle >= g_idle_limit && c->r[15] >= PAGE0_END) break;
    }
    g_cur = NULL;
    dev_set_core(0);
    credit(n - credited);
    return n;
}

/* At the end of a handler: c is the state of the core that runs natively
 * next. While the CPU sleeps and the COP has work, the COP runs natively
 * and the CPU's registers wait in g_cpu_parked. The CPU comes back when one
 * of its lines is active, or when the COP sleeps. */
static void schedule(struct arm_cpu *c) {
    for (;;) {
        if (!g_native_cop) {
            if (!proc_cpu_sleeping()) {                 /* the CPU runs */
                deliver_irq(c);
                return;
            }
            if (cop_wants_native()) {                   /* park the CPU */
                cop_init();
                g_cpu_parked = *c;
                *c = g_cop;
                g_native_cop = 1;
                g_n_native_cop++;
                continue;
            }
            if (!sleep_until_irq(c)) continue;          /* the COP has work */
            deliver_irq(c);
            g_n_interp += run_ahead(c, 0, SIGALRM, 0);  /* the vector is in page 0 */
            continue;
        }
        if ((cpu_wants_back() || !cop_runnable()) && !mid_stub(c->r[15])) {
            g_cop = *c;                                 /* back to the CPU: it wakes, or it sleeps on */
            *c = g_cpu_parked;
            g_native_cop = 0;
            continue;
        }
        if (cop_irq_ready(NULL)) {
            cop_deliver_irq_on(c);
            g_n_cop += run_ahead(c, 1, SIGALRM, 0);
            continue;
        }
        return;                                         /* the COP runs */
    }
}

/* the host cannot execute in page 0: continue at the alias of the same code */
static void fix_native_pc(struct arm_cpu *c) {
    uint32_t alias = mach_exec_alias(c->r[15]);
    if (alias) c->r[15] = alias;
}

static void on_fault(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    struct arm_cpu c;
    uint32_t addr = (uint32_t)(uintptr_t)si->si_addr;
    uc_to_cpu(uc, &c);
    if (g_in_handler) {
        fprintf(stderr, "FATAL: fault inside the runner (sig=%d addr=%08x host pc=%08x)\n", sig, addr, c.r[15]);
        _exit(3);
    }
    g_in_handler = 1;
    freeze();
    pv_sync_in(native_vcpu());
    g_n_trap++;
    g_ring[g_ring_i].pc = c.r[15];
    g_ring[g_ring_i].addr = addr;
    g_ring[g_ring_i].sig = sig;
    g_ring_i = (g_ring_i + 1) % RING_SIZE;
    if (g_prof_on) prof_sample(c.r[15]);

    if (sig == SIGILL && pv_trap(native_vcpu(), &c, &g_bus)) {
        /* a stub of pv.c: another mode, or an interrupt that it enabled */
        timers_tick(cpu_guest_time_us());
        schedule(&c);
        fix_native_pc(&c);
        periodic(&c, 0);
    } else if (sig != SIGILL && addr == c.r[15] && !(c.r[15] < PAGE0_END && dev_remapped())) {
        /* an instruction fetch fault outside page 0: a pc outside guest
         * memory, or low code before the remap (it runs at the DRAM base) */
        if (!mach_exec_alias(c.r[15])) fatal("bad guest pc", &c, sig, addr);
        g_n_exec++;
        c.r[15] = mach_exec_alias(c.r[15]);
    } else {
        /* A trap, or a fetch in page 0: the handler runs page 0 code. At
         * the alias address (0x04000000 + pc) the guest would see a wrong
         * pc, and pc-relative addresses (an IRAM function, for example)
         * would get bit 26. */
        if (sig != SIGILL && addr == c.r[15]) g_n_exec++;
        if (g_native_cop) g_n_cop += run_ahead(&c, 1, sig, addr);
        else g_n_interp += run_ahead(&c, 0, sig, addr);
        timers_tick(cpu_guest_time_us());
        schedule(&c);
        fix_native_pc(&c);
        periodic(&c, 0);
    }
    pv_out();
    cpu_to_uc(&c, uc);
    thaw();
    g_in_handler = 0;
}

static void on_tick(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    struct arm_cpu c;
    (void)si;
    if (g_in_handler) return;
    g_in_handler = 1;
    freeze();
    pv_sync_in(native_vcpu());
    uc_to_cpu(uc, &c);
    if (g_prof_on) prof_sample(c.r[15]);
    timers_tick(cpu_guest_time_us());
    if (g_native_cop) {                                 /* a CPU line, the COP's sleep or its interrupt */
        schedule(&c);
        fix_native_pc(&c);
        cpu_to_uc(&c, uc);
    } else {
        cop_slice(COP_SLICE_TICK);
        if (cpu_int_ready()) {
            /* the guest leaves its native code: interpret the IRQ entry */
            deliver_irq(&c);
            g_n_interp += run_ahead(&c, 0, sig, 0);
            timers_tick(cpu_guest_time_us());
            schedule(&c);
            fix_native_pc(&c);
            cpu_to_uc(&c, uc);
        }
    }
    periodic(&c, 0);
    pv_out();
    thaw();
    g_in_handler = 0;
}

/* ---- calibration: the kernel's cost of one trap ---- */

static void on_calibrate(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    (void)sig; (void)si;
    uc->uc_mcontext.arm_pc += 4;
}

static void calibrate(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_calibrate;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGILL, &sa, &old);
    uint64_t t0 = host_now_ns();
    for (int i = 0; i < CALIBRATE_TRAPS; i++) __asm__ volatile(".word 0xe7f456f8" ::: "memory");
    g_kernel_ns = (host_now_ns() - t0) / CALIBRATE_TRAPS;
    sigaction(SIGILL, &old, NULL);
    fprintf(stderr, "cpu: one trap costs %llu ns\n", (unsigned long long)g_kernel_ns);
}

/* The first entry into the guest is a signal return: the kernel loads all
 * registers and the signal mask in one step. */
static void on_enter(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    struct arm_cpu c = g_start;
    (void)sig; (void)si;
    fix_native_pc(&c);
    pv_sync_out(&g_vcpu, 0);
    cpu_to_uc(&c, uc);
    sigemptyset(&uc->uc_sigmask);
    struct itimerval it = { { 0, TICK_US }, { 0, TICK_US } };
    setitimer(ITIMER_REAL, &it, NULL);
}

void cpu_run(const struct arm_cpu *start, const struct vcpu *v) {
    static uint8_t altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    struct sigaction sa;

    g_start = *start;
    g_vcpu = *v;
    g_prof_on = getenv("PROFILE") != NULL;
    if (getenv("HOSTPROF")) hostprof_start();
    g_jit_on = !getenv("NOJIT") && jit_init() == 0;
    interp_pc_ring_on = getenv("PCRING") != NULL;
    g_tasklog = getenv("TASKLOG") != NULL;
    g_cop_on = (mach_cold_boot() || g_cop_from_snap) && !getenv("NOCOP");
    g_native_cop_on = g_cop_on && !(getenv("NATIVE_COP") && !atoi(getenv("NATIVE_COP")));
    fprintf(stderr, "cpu: COP %s%s\n", g_cop_on ? "on" : "off", g_native_cop_on ? ", native while the CPU sleeps" : "");
    fprintf(stderr, "cpu: %s\n", g_jit_on ? "JIT on" : "JIT off");
    if (getenv("RUNAHEAD_IDLE")) g_idle_limit = (uint32_t)atoi(getenv("RUNAHEAD_IDLE"));
    sigaltstack(&ss, NULL);
    calibrate();
    g_time0 = cpu_guest_time_us();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;   /* NODEFER: report a fault in the runner */
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGALRM);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_tick;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_enter;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    fprintf(stderr, "cpu: enter guest pc=%08x cpsr=%08x sp=%08x\n",
            start->r[15], vcpu_cpsr(v, start), start->r[13]);
    kill(getpid(), SIGUSR1);
    for (;;) pause();
}
