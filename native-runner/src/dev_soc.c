/* PP502x system-on-chip registers and the device address decoder.
 *
 * Port of qemu-david hw/arm/ipod_video/ipod_video.c (processor id and
 * control, timers, cache control, remap unit, GPIO, click wheel, clock,
 * device control) and ipod_video_intc.c (interrupt controller).
 *
 * Register blocks with 32-bit registers ignore the access size, as the
 * QEMU blocks do: a narrower read gives the low bytes of the register, and
 * a narrower write sets the register to the value.
 */
#include "dev.h"
#include <stdlib.h>
#include <string.h>

/* ---------- address map ---------- */
#define BCM_BASE        0x30000000u
#define BCM_SIZE        0x00080000u
#define SOC_BASE        0x60000000u     /* 64 KB: blocks below, the other addresses read 0 */
#define DEVREG_BASE     0x70000000u     /* 64 KB, same rule */
#define IDE_BASE        0xc3000000u
#define USB_BASE        0xc5000000u     /* not modelled: reads 0 */
#define CACHE_ARRAY_BASE 0xf0000000u    /* the cache's own storage: plain memory */
#define CACHE_ARRAY_SIZE 0x0000f000u
#define MMAP_BASE       0xf000f000u
#define PAGE            0x1000u

/* blocks in the SoC window, by offset */
#define SOC_PROC_ID     0x0000
#define SOC_MAILBOX     0x1000          /* CPU_QUEUE at +0x10, COP_QUEUE at +0x20 */
#define MBX_MSG_STAT    0x00            /* message bits: the CPU sets them for the COP */
#define MBX_MSG_SET     0x04
#define MBX_MSG_CLR     0x08
#define MBX_CPU_QUEUE   0x10
#define MBX_COP_QUEUE   0x20
#define MBX_DEPTH       4
#define SOC_INTC        0x4000
#define SOC_INTC_SIZE   0x0200
#define SOC_TIMER       0x5000
#define SOC_CLOCK       0x6000
#define SOC_PROC_CTL    0x7000
#define SOC_DMA_A       0x8000
#define SOC_DMA_B       0xa000
#define SOC_DMA_SIZE    0x2000
#define SOC_CACHE_CTL   0xc000
#define SOC_GPIO        0xd000
/* blocks in the device register window */
#define DEVREG_DEV_CTL  0x0000
#define DEVREG_SER0     0x6000
#define DEVREG_PIEZO    0xa000          /* the clicker: bit 31 starts a click (Rockbox piezo.c) */
#define PIEZO_ON        0x80000000u
#define DEVREG_SER0_SIZE 0x0020
#define DEVREG_I2S      0x2800
#define DEVREG_I2S_SIZE 0x0100
#define DEVREG_I2C      0xc000
#define DEVREG_I2C_SIZE 0x0100
#define DEVREG_WHEEL    0xc000          /* the page: the I2C block covers its first 256 bytes */

/* interrupt controller, offsets in a bank */
#define INTC_CPU_INT_STAT   0x00
#define INTC_COP_INT_STAT   0x04
#define INTC_CPU_FIQ_STAT   0x08
#define INTC_COP_FIQ_STAT   0x0c
#define INTC_INT_STAT       0x10
#define INTC_FORCED_STAT    0x14
#define INTC_FORCED_SET     0x18
#define INTC_FORCED_CLR     0x1c
#define INTC_CPU_INT_EN_STAT 0x20
#define INTC_CPU_INT_EN     0x24
#define INTC_CPU_INT_DIS    0x28
#define INTC_CPU_INT_PRIORITY 0x2c      /* a set bit makes the line an FIQ */
#define INTC_COP_INT_EN_STAT 0x30
#define INTC_COP_INT_EN     0x34
#define INTC_COP_INT_DIS    0x38
#define INTC_COP_INT_PRIORITY 0x3c
#define INTC_BANK_SIZE      0x100
#define INTC_HI_IRQ         (1u << 30)  /* low bank: the high bank has an interrupt */

#define TIMER_CFG_ENABLE     0x80000000u
#define TIMER_CFG_IRQ_ENABLE 0x40000000u
#define TIMER_CFG_PERIOD     0x0fffffffu
#define TIMER1_CFG  0x00
#define TIMER1_VAL  0x04
#define TIMER2_CFG  0x08
#define TIMER2_VAL  0x0c
#define USEC_TIMER  0x10

#define PROC_SLEEPING       0x80000000u
#define PROC_CTL_CPU        0x00
#define PROC_CTL_COP        0x04
#define PROC_ID_CPU         0x55
#define PROC_ID_COP         0xaau
#define CACHE_CTL_VECT_REMAP 0x0010u
#define CACHE_CTL_READY     0x4000u
#define MMAP_BASE_MASK      0xff000000u
#define DRAM_BASE           0x10000000u
#define PLL_STATUS          0x3c
#define PLL_LOCKED          0x80000000u
#define DEV_USB_STATUS      0x28
#define DEV_USB_READY       0x80u
#define GPO32_VAL           0x80
#define GPO32_BCM_POWER     0x4000u
#define PP_VER1             0x32324120u /* "22A " */
#define PP_VER2             0x50503530u /* "PP50" */

#define PULSE_CTL           0x300       /* the pulse unit of the backlight pin D7 */
#define PULSE_START         0x40000000u
#define PULSE_LEN           0x320
#define WHEEL_DATA          0x140
#define WHEEL_STATUS        0x104
#define WHEEL_STATUS_RX     (1u << 26)
#define WHEEL_PACKET        0x8000001au
#define WHEEL_TOUCHED       0x40000000u
#define WHEEL_CLICKS        96
#define WHEEL_SELECT        0x00000100u
#define WHEEL_RIGHT         0x00000200u
#define WHEEL_LEFT          0x00000400u
#define WHEEL_PLAY          0x00000800u
#define WHEEL_MENU          0x00001000u

#define REGS (PAGE / 4)

static struct {
    /* interrupt controller */
    uint32_t irq_raw[2], irq_forced[2], cpu_enabled[2], cop_enabled[2];
    uint32_t cpu_fiq[2], cop_fiq[2];    /* INT_PRIORITY: the lines that are FIQs */
    /* timers: deadlines in guest time */
    uint32_t timer_cfg[2];
    uint64_t timer_period[2], timer_deadline[2];
    uint64_t usec_bias;                 /* USEC_TIMER = guest time + bias */
    /* processor control */
    uint32_t proc_ctl[2], cpu_sleeping;
    uint32_t cop_started, cop_sleeping;     /* the COP is held until the CPU first wakes it */
    uint32_t mbx[2][MBX_DEPTH], mbx_n[2];   /* the message queues: [0] to the CPU, [1] to the COP */
    uint32_t mbx_msg;                       /* MBX_MSG_STAT */
    uint32_t cache_ctl, dram_remapped;
    uint32_t mmap_regs[REGS], wheel_regs[REGS], clock_regs[REGS], dev_regs[REGS];
    uint32_t wheel_buttons, wheel_pending, wheel_touched, wheel_position, wheel_release;
    uint32_t wheel_held;                    /* buttons pressed until their release byte */
    uint32_t wheel_release_wait;            /* releases that wait: see button_try_release() */
    uint64_t wheel_release_at;              /* guest time: the end of the shortest press */
    char msg[5];                            /* the power message "%NNNs" so far (input_msg()) */
    uint8_t msg_n;
    int32_t wheel_move;                     /* units of the gesture still to go, < 0 anticlockwise */
    int32_t wheel_queue;                    /* keys that wait for their gesture, < 0 anticlockwise */
    uint64_t wheel_next_us;                 /* guest time of the next touch, step or lift */
    uint8_t serial_regs[8];
    uint32_t piezo;
    uint8_t cache_arrays[CACHE_ARRAY_SIZE];
} soc;

int g_dev_replay;
static int g_clock_read;
static uint32_t g_access_pc;
static uint64_t g_access_count;
static int g_trace_key_seen;                            /* 1: a key came, 2: the trace saw it */

/* ---------- interrupt controller ---------- */

static uint32_t intc_pending(int bank) {
    return soc.irq_raw[bank] | soc.irq_forced[bank];
}

void intc_set_irq(int line, int level) {
    uint32_t bit = 1u << (line % 32);
    if (level) soc.irq_raw[line / 32] |= bit;
    else soc.irq_raw[line / 32] &= ~bit;
}

/* The firmware makes the DMA lines (26, 27) FIQs. The QEMU model has no
 * FIQ: there, all lines are IRQs. */
static int intc_active(const uint32_t *enabled, const uint32_t *fiq, int want_fiq) {
    uint32_t sel0 = want_fiq ? fiq[0] : ~fiq[0], sel1 = want_fiq ? fiq[1] : ~fiq[1];
    return ((intc_pending(0) & enabled[0] & sel0) | (intc_pending(1) & enabled[1] & sel1)) != 0;
}

int intc_cpu_irq(void) { return intc_active(soc.cpu_enabled, soc.cpu_fiq, 0); }
int intc_cop_irq(void) { return intc_active(soc.cop_enabled, soc.cop_fiq, 0); }
int intc_cpu_fiq(void) { return intc_active(soc.cpu_enabled, soc.cpu_fiq, 1); }
int intc_cop_fiq(void) { return intc_active(soc.cop_enabled, soc.cop_fiq, 1); }

/* the core that makes the device accesses now: 0 = CPU, 1 = COP */
static int g_core;
void dev_set_core(int core) { g_core = core; }

/* what the low bank shows: the high bank is line 30 */
static uint32_t intc_low_pending(uint32_t hi_enabled) {
    uint32_t p = intc_pending(0);
    if (intc_pending(1) & hi_enabled) p |= INTC_HI_IRQ;
    return p;
}

static uint32_t intc_read(uint32_t off) {
    int bank = off / INTC_BANK_SIZE;
    switch (off % INTC_BANK_SIZE) {
    case INTC_CPU_INT_STAT:
        return (bank ? intc_pending(1) : intc_low_pending(soc.cpu_enabled[1] & ~soc.cpu_fiq[1])) &
               soc.cpu_enabled[bank] & ~soc.cpu_fiq[bank];
    case INTC_COP_INT_STAT:
        return (bank ? intc_pending(1) : intc_low_pending(soc.cop_enabled[1] & ~soc.cop_fiq[1])) &
               soc.cop_enabled[bank] & ~soc.cop_fiq[bank];
    case INTC_CPU_FIQ_STAT:    return intc_pending(bank) & soc.cpu_enabled[bank] & soc.cpu_fiq[bank];
    case INTC_COP_FIQ_STAT:    return intc_pending(bank) & soc.cop_enabled[bank] & soc.cop_fiq[bank];
    case INTC_CPU_INT_PRIORITY: return soc.cpu_fiq[bank];
    case INTC_COP_INT_PRIORITY: return soc.cop_fiq[bank];
    case INTC_INT_STAT:        return bank ? intc_pending(1) : intc_low_pending(~0u);
    case INTC_FORCED_STAT:     return soc.irq_forced[bank];
    case INTC_CPU_INT_EN_STAT: return soc.cpu_enabled[bank];
    case INTC_COP_INT_EN_STAT: return soc.cop_enabled[bank];
    default:                   return 0;
    }
}

static void intc_write(uint32_t off, uint32_t v) {
    int bank = off / INTC_BANK_SIZE;
    switch (off % INTC_BANK_SIZE) {
    case INTC_FORCED_SET:  soc.irq_forced[bank] |= v; break;
    case INTC_FORCED_CLR:  soc.irq_forced[bank] &= ~v; break;
    case INTC_CPU_INT_EN:  soc.cpu_enabled[bank] |= v; break;
    case INTC_CPU_INT_DIS: soc.cpu_enabled[bank] &= ~v; break;
    case INTC_COP_INT_EN:  soc.cop_enabled[bank] |= v; break;
    case INTC_COP_INT_DIS: soc.cop_enabled[bank] &= ~v; break;
    case INTC_CPU_INT_PRIORITY: soc.cpu_fiq[bank] = v; break;
    case INTC_COP_INT_PRIORITY: soc.cop_fiq[bank] = v; break;
    default: break;
    }
}

/* ---------- timers ---------- */

uint32_t timers_usec(void) {
    return (uint32_t)(cpu_guest_time_us() + soc.usec_bias);
}

void timers_resume(uint32_t usec_now) {
    uint64_t now = cpu_guest_time_us();
    soc.usec_bias = (uint64_t)usec_now - now;
    for (int n = 0; n < 2; n++) soc.timer_deadline[n] = now + soc.timer_period[n];
}

int timers_clock_was_read(void) {
    int r = g_clock_read;
    g_clock_read = 0;
    return r;
}

/* TIMERn_CFG: bit 31 enables the timer, bit 30 enables its interrupt, the
 * low bits are the period in microseconds minus one. A write restarts it. */
static void timer_write_cfg(int n, uint32_t cfg) {
    soc.timer_cfg[n] = cfg;
    soc.timer_period[n] = (cfg & TIMER_CFG_ENABLE) ? (uint64_t)(cfg & TIMER_CFG_PERIOD) + 1 : 0;
    soc.timer_deadline[n] = cpu_guest_time_us() + soc.timer_period[n];
}

static void wheel_tick(uint64_t now_us);
static uint64_t wheel_next_deadline_us(void);
static void button_try_release(uint64_t now_us);
static uint64_t button_next_deadline_us(void);

void timers_tick(uint64_t now_us) {
    ide_tick(now_us);
    dma_tick(now_us);
    wheel_tick(now_us);
    button_try_release(now_us);
    for (int n = 0; n < 2; n++) {
        if (!soc.timer_period[n] || now_us < soc.timer_deadline[n]) continue;
        if (soc.timer_cfg[n] & TIMER_CFG_IRQ_ENABLE) intc_set_irq(n ? IRQ_TIMER2 : IRQ_TIMER1, 1);
        soc.timer_deadline[n] += soc.timer_period[n];
        /* the line stays active until the guest acknowledges it: periods
         * that the guest missed give no more interrupts */
        if (soc.timer_deadline[n] <= now_us) soc.timer_deadline[n] = now_us + soc.timer_period[n];
    }
}

uint64_t timers_next_deadline_us(void) {
    uint64_t next = ide_next_deadline_us(), dma_next = dma_next_deadline_us(), wheel_next = wheel_next_deadline_us();
    if (dma_next && (!next || dma_next < next)) next = dma_next;
    if (wheel_next && (!next || wheel_next < next)) next = wheel_next;
    uint64_t button_next = button_next_deadline_us();
    if (button_next && (!next || button_next < next)) next = button_next;
    for (int n = 0; n < 2; n++)
        if (soc.timer_period[n] && (!next || soc.timer_deadline[n] < next)) next = soc.timer_deadline[n];
    return next;
}

static uint32_t timer_read(uint32_t off) {
    switch (off) {
    case TIMER1_CFG: return soc.timer_cfg[0];
    case TIMER2_CFG: return soc.timer_cfg[1];
    case TIMER1_VAL: intc_set_irq(IRQ_TIMER1, 0); return 0;     /* a read acknowledges the interrupt */
    case TIMER2_VAL: intc_set_irq(IRQ_TIMER2, 0); return 0;
    case USEC_TIMER: g_clock_read = 1; cpu_clock_poll(); return timers_usec();
    default: return 0;
    }
}

/* ---------- mailbox ----------
 * The COP posts a message to the CPU by writing CPU_QUEUE. That raises the
 * CPU's mailbox interrupt (line 4: the retailOS dispatcher at 0x277128 tests
 * it and calls 0x267b50, which reads CPU_QUEUE). A read takes the oldest
 * message; the line drops when the queue is empty. The QEMU model has no
 * mailbox. COP_QUEUE is stored only: its interrupt line is not known.
 * The other way, the CPU sets bits in MBX_MSG_STAT (MBX_MSG_SET) and wakes
 * the COP through COP_CTL; the COP reads the bits and clears them. */

static uint32_t mailbox_read(uint32_t reg) {
    int q = reg == MBX_CPU_QUEUE ? 0 : reg == MBX_COP_QUEUE ? 1 : -1;
    uint32_t v;
    if (reg == MBX_MSG_STAT) return soc.mbx_msg;
    if (q < 0 || !soc.mbx_n[q]) return 0;
    v = soc.mbx[q][0];
    memmove(&soc.mbx[q][0], &soc.mbx[q][1], sizeof soc.mbx[q][0] * (MBX_DEPTH - 1));
    soc.mbx_n[q]--;
    if (q == 0 && !soc.mbx_n[0]) intc_set_irq(IRQ_MAILBOX, 0);
    return v;
}

static void mailbox_write(uint32_t reg, uint32_t v) {
    int q = reg == MBX_CPU_QUEUE ? 0 : reg == MBX_COP_QUEUE ? 1 : -1;
    if (reg == MBX_MSG_SET) soc.mbx_msg |= v;
    if (reg == MBX_MSG_CLR) soc.mbx_msg &= ~v;
    if (q < 0) return;
    if (soc.mbx_n[q] == MBX_DEPTH) {                    /* full: the oldest message is lost */
        memmove(&soc.mbx[q][0], &soc.mbx[q][1], sizeof soc.mbx[q][0] * (MBX_DEPTH - 1));
        soc.mbx_n[q]--;
    }
    soc.mbx[q][soc.mbx_n[q]++] = v;
    if (q == 0) intc_set_irq(IRQ_MAILBOX, 1);
}

/* ---------- processor control ---------- */

int proc_cpu_sleeping(void) { return (int)soc.cpu_sleeping; }
void proc_cpu_wake(void) { soc.cpu_sleeping = 0; }
int proc_cop_started(void) { return (int)soc.cop_started; }
int proc_cop_sleeping(void) { return (int)soc.cop_sleeping; }
void proc_cop_wake(void) { soc.cop_sleeping = 0; }

static uint32_t proc_ctl_read(uint32_t off) {
    if (off == PROC_CTL_CPU) return soc.proc_ctl[0] | (soc.cpu_sleeping ? PROC_SLEEPING : 0);
    if (off == PROC_CTL_COP) return soc.proc_ctl[1] | (soc.cop_sleeping ? PROC_SLEEPING : 0);
    return 0;
}

/* The sleep bit stops a core until one of its interrupt lines is active; a
 * write without it wakes the core. The COP comes out of reset at the first
 * wake (as in the QEMU model: it would derail the boot to run it earlier). */
static void proc_ctl_write(uint32_t off, uint32_t v) {
    if (off != PROC_CTL_CPU && off != PROC_CTL_COP) return;
    soc.proc_ctl[off / 4] = v & ~PROC_SLEEPING;
    if (g_dev_replay) return;
    if (off == PROC_CTL_CPU) soc.cpu_sleeping = (v & PROC_SLEEPING) != 0;
    else {
        soc.cop_sleeping = (v & PROC_SLEEPING) != 0;
        if (!soc.cop_sleeping) soc.cop_started = 1;
    }
}

/* ---------- remap unit ---------- */

static void mmap_write(uint32_t off, uint32_t v) {
    soc.mmap_regs[off / 4] = v;
    if (soc.dram_remapped) return;
    /* a window is two words, logical then physical: the top byte is the base */
    unsigned pair = (off / 4) & ~1u;
    if ((soc.mmap_regs[pair] & MMAP_BASE_MASK) == 0 && (soc.mmap_regs[pair + 1] & MMAP_BASE_MASK) == DRAM_BASE) {
        soc.dram_remapped = 1;
        if (!g_dev_replay) mach_remap_dram();
    }
}

/* ---------- click wheel ---------- */

static uint32_t wheel_read(uint32_t off) {
    if (off == WHEEL_DATA) {
        uint32_t packet = WHEEL_PACKET | soc.wheel_buttons | soc.wheel_held;
        if (soc.wheel_touched) packet |= WHEEL_TOUCHED | ((soc.wheel_position & 0x7f) << 16);
        return packet;
    }
    if (off == WHEEL_STATUS) return soc.wheel_regs[off / 4] | (soc.wheel_pending ? WHEEL_STATUS_RX : 0);
    return soc.wheel_regs[off / 4];
}

static void wheel_event(void);

static void wheel_write(uint32_t off, uint32_t v) {
    if (off == WHEEL_STATUS && (v & WHEEL_STATUS_RX)) {  /* the handler acknowledges the packet */
        soc.wheel_pending = 0;
        intc_set_irq(IRQ_I2C, 0);
        v &= ~WHEEL_STATUS_RX;
        button_try_release(cpu_guest_time_us());         /* the press was read */
    }
    /* the pulse unit of the backlight (dev_backlight.c), in the same page */
    if (off >= PULSE_LEN && off < PULSE_LEN + 0x20) backlight_pulse_length(v);
    if (off == PULSE_CTL && (v & PULSE_START)) backlight_pulse();
    soc.wheel_regs[off / 4] = v;
}

static void wheel_event(void) {                         /* a change is a new packet: latch and interrupt */
    soc.wheel_pending = 1;
    intc_set_irq(IRQ_I2C, 1);
}

/* A key turns the wheel by a gesture, as a finger does: touch, then the
 * units in packets, then the lift after a pause. The firmware accelerates fast
 * movement and long gestures: steps that came all at once, a finger that
 * never lifted, and three keys in one gesture (five items) made the list
 * jump by a different number of items for each key. So each key is a
 * gesture of its own: a key during a gesture waits in the queue, and its
 * gesture starts one step after the lift. With 8 units, one key moves one
 * item (4 units: none, 6 units: one item for two keys). Each packet is an
 * interrupt, and the firmware does real work for it in the handler, so a
 * packet carries 2 units: 4 packets 4 ms apart. A key then shows its present
 * after about 53 ms (one unit per packet, 2 ms apart: about 68 ms). 3 or 4
 * units per packet looked like fast turning: the firmware accelerated and
 * ten keys moved too far. The lift comes late (80 ms), so that keys in a row
 * move at most ten items per second. With these values ten keys at once
 * moved ten items, in the main menu and in the language list. WHEEL_UNITS,
 * WHEEL_STEP_US, WHEEL_LIFT_US and WHEEL_PACKET change the gesture. */
static uint32_t g_wheel_units = 8, g_wheel_step_us = 4000, g_wheel_lift_us = 80000;
static uint32_t g_wheel_packet = 2;         /* units per packet (WHEEL_PACKET) */

void wheel_config(uint32_t units, uint32_t step_us, uint32_t lift_us, uint32_t per_packet) {
    g_wheel_units = units;
    g_wheel_step_us = step_us;
    g_wheel_lift_us = lift_us;
    g_wheel_packet = per_packet ? per_packet : 1;
}

static void wheel_config_env(void) {
    const char *u = getenv("WHEEL_UNITS"), *st = getenv("WHEEL_STEP_US"), *l = getenv("WHEEL_LIFT_US");
    const char *pk = getenv("WHEEL_PACKET");
    if (u) g_wheel_units = (uint32_t)strtoul(u, NULL, 0);
    if (st) g_wheel_step_us = (uint32_t)strtoul(st, NULL, 0);
    if (l) g_wheel_lift_us = (uint32_t)strtoul(l, NULL, 0);
    if (pk && strtoul(pk, NULL, 0)) g_wheel_packet = (uint32_t)strtoul(pk, NULL, 0);
}

/* touch: a packet at the old position, the steps follow */
static void wheel_touch(int dir, uint64_t now_us) {
    soc.wheel_touched = 1;
    soc.wheel_move = dir * (int32_t)g_wheel_units;
    soc.wheel_next_us = now_us + g_wheel_step_us;
    wheel_event();
}

static void wheel_turn(int dir) {
    if (soc.wheel_touched || soc.wheel_queue) soc.wheel_queue += dir;
    else wheel_touch(dir, cpu_guest_time_us());
}

static void wheel_tick(uint64_t now_us) {
    if (!(soc.wheel_touched || soc.wheel_queue) || now_us < soc.wheel_next_us) return;
    if (!soc.wheel_touched) {                           /* the next key's gesture */
        int dir = soc.wheel_queue > 0 ? 1 : -1;
        soc.wheel_queue -= dir;
        wheel_touch(dir, now_us);
        return;
    }
    if (soc.wheel_move) {
        int dir = soc.wheel_move > 0 ? 1 : -1;
        uint32_t k = (uint32_t)(dir * soc.wheel_move) < g_wheel_packet ? (uint32_t)(dir * soc.wheel_move) : g_wheel_packet;
        soc.wheel_position = (soc.wheel_position + (dir > 0 ? k : WHEEL_CLICKS - k)) % WHEEL_CLICKS;
        soc.wheel_move -= dir * (int32_t)k;
        soc.wheel_next_us = now_us + (soc.wheel_move ? g_wheel_step_us : g_wheel_lift_us);
    } else {
        soc.wheel_touched = 0;                          /* the lift */
        soc.wheel_next_us = now_us + g_wheel_step_us;   /* a queued key touches after one step */
    }
    wheel_event();
}

static uint64_t wheel_next_deadline_us(void) {
    return (soc.wheel_touched || soc.wheel_queue) ? soc.wheel_next_us : 0;
}

/* The shortest press, in guest time: a short tap of a finger. A press and
 * its release in one key poll was a press of a few microseconds, and the
 * firmware did not see it. A release waits for this time after the press,
 * and for the ack of the press packet: the packet is read when the firmware
 * takes it, not latched, so an early release replaced the press. */
#define MIN_PRESS_US 50000u

static void button_try_release(uint64_t now_us) {
    if (!soc.wheel_release_wait || soc.wheel_pending || now_us < soc.wheel_release_at) return;
    soc.wheel_held &= ~soc.wheel_release_wait;
    soc.wheel_release_wait = 0;
    wheel_event();
}

static uint64_t button_next_deadline_us(void) {
    return (soc.wheel_release_wait && !soc.wheel_pending) ? soc.wheel_release_at : 0;
}

/* A held button: M/B/P/F/C press menu/left/play/right/select until the
 * release byte m/b/p/f/c. The firmware's long presses need it: sleep,
 * backlight, scan. Returns 1 if c is one of these bytes. */
static int wheel_hold_key(char c) {
    static const char press[] = "MBPFC", release[] = "mbpfc";
    static const uint32_t bits[] = { WHEEL_MENU, WHEEL_LEFT, WHEEL_PLAY, WHEEL_RIGHT, WHEEL_SELECT };
    for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        if (c == press[i]) {
            soc.wheel_release_wait &= ~bits[i];         /* pressed again before its release */
            if (soc.wheel_held & bits[i]) return 1;
            soc.wheel_held |= bits[i];
            soc.wheel_release_at = cpu_guest_time_us() + MIN_PRESS_US;
            wheel_event();
            return 1;
        }
        if (c == release[i]) {
            if (!(soc.wheel_held & bits[i])) return 1;
            soc.wheel_release_wait |= bits[i];
            button_try_release(cpu_guest_time_us());
            return 1;
        }
    }
    return 0;
}

/* The power message of the app: '%', the battery level as three digits,
 * and the state (u/c/f, see power_set()). Returns 1 if c belongs to it. */
static int input_msg(char c) {
    if (!soc.msg_n) {
        if (c != '%') return 0;
        soc.msg[soc.msg_n++] = c;
        return 1;
    }
    if (!c) return 1;                                   /* the release poll between two reads */
    soc.msg[soc.msg_n++] = c;
    if (soc.msg_n < sizeof soc.msg) return 1;
    soc.msg_n = 0;
    const char *m = soc.msg;
    if (m[1] < '0' || m[1] > '9' || m[2] < '0' || m[2] > '9' || m[3] < '0' || m[3] > '9') return 1;
    if (m[4] != 'u' && m[4] != 'c' && m[4] != 'f') return 1;
    power_set((unsigned)((m[1] - '0') * 100 + (m[2] - '0') * 10 + (m[3] - '0')), m[4]);
    return 1;
}

/* One input character: w/a/s/d = menu/left/play/right, enter or space =
 * select (pressed until the next call with 0), e/q = turn the wheel, and
 * the press and release bytes of wheel_hold_key(). c == 0: release the
 * clicks (not the held buttons). */
void wheel_key(char c) {
    uint32_t bit = 0;
    if (input_msg(c)) return;
    if (c && !g_trace_key_seen) g_trace_key_seen = 1;
    if (wheel_hold_key(c)) return;
    if (c == 'H') { gpio_toggle_hold(); return; }       /* the hold switch */
    switch (c) {
    case 0:
        if (!soc.wheel_buttons) return;
        soc.wheel_buttons = 0;
        wheel_event();
        return;
    case 'w': bit = WHEEL_MENU; break;
    case 'a': bit = WHEEL_LEFT; break;
    case 's': bit = WHEEL_PLAY; break;
    case 'd': bit = WHEEL_RIGHT; break;
    case '\n': case '\r': case ' ': bit = WHEEL_SELECT; break;
    case 'e': case 'q':
        wheel_turn(c == 'e' ? 1 : -1);
        return;
    default: return;
    }
    soc.wheel_buttons |= bit;
    wheel_event();
}

/* ---------- serial port 0: a 16550, registers 4 bytes apart ---------- */

static void serial_putc(uint8_t c) {
    static char line[160];
    static unsigned n;
    if (c != '\n' && c != '\r' && n < sizeof line - 1) { line[n++] = (char)c; return; }
    if (!n) return;
    line[n] = 0;
    fprintf(stderr, "serial: %s\n", line);
    n = 0;
}

static uint32_t serial_read(uint32_t off) {
    unsigned reg = (off >> 2) & 7;
    int dlab = soc.serial_regs[3] & 0x80;
    switch (reg) {
    case 0: return 0;                                   /* no received data (or divisor latch) */
    case 1: return dlab ? 0 : soc.serial_regs[1];
    case 2: return 0x01;                                /* no interrupt is pending */
    case 5: return 0x60;                                /* transmitter empty */
    case 6: return 0xb0;                                /* carrier, data set ready, clear to send */
    default: return soc.serial_regs[reg];
    }
}

static void serial_write(uint32_t off, uint32_t v) {
    unsigned reg = (off >> 2) & 7;
    int dlab = soc.serial_regs[3] & 0x80;
    if (reg == 0 && !dlab && !g_dev_replay) serial_putc((uint8_t)v);
    else if (!(dlab && reg <= 1)) soc.serial_regs[reg] = (uint8_t)v;
}

/* ---------- decoder ---------- */

static uint32_t soc_read(uint32_t off) {
    uint32_t page = off & ~(PAGE - 1), reg = off & (PAGE - 1);
    if (off >= SOC_INTC && off < SOC_INTC + SOC_INTC_SIZE) return intc_read(off - SOC_INTC);
    if (off >= SOC_DMA_A && off < SOC_DMA_B + SOC_DMA_SIZE) {
        uint32_t v = 0;
        dma_access(off >= SOC_DMA_B, (off - SOC_DMA_A) % SOC_DMA_SIZE, 0, &v);
        return v;
    }
    switch (page) {
    case SOC_PROC_ID:   return reg == 0 ? (g_core ? PROC_ID_COP : PROC_ID_CPU) : 0;
    case SOC_MAILBOX:   return mailbox_read(reg);
    case SOC_TIMER:     return timer_read(reg);
    case SOC_CLOCK:     return soc.clock_regs[reg / 4] | (reg == PLL_STATUS ? PLL_LOCKED : 0);
    case SOC_PROC_CTL:  return proc_ctl_read(reg);
    case SOC_CACHE_CTL: return reg == 0 ? soc.cache_ctl | CACHE_CTL_READY : 0;
    case SOC_GPIO:      return gpio_read(reg);
    default:            return 0;                       /* a block that is not modelled */
    }
}

static void soc_write(uint32_t off, uint32_t v) {
    uint32_t page = off & ~(PAGE - 1), reg = off & (PAGE - 1);
    if (off >= SOC_INTC && off < SOC_INTC + SOC_INTC_SIZE) { intc_write(off - SOC_INTC, v); return; }
    if (off >= SOC_DMA_A && off < SOC_DMA_B + SOC_DMA_SIZE) {
        dma_access(off >= SOC_DMA_B, (off - SOC_DMA_A) % SOC_DMA_SIZE, 1, &v);
        return;
    }
    switch (page) {
    case SOC_TIMER:
        if (reg == TIMER1_CFG || reg == TIMER2_CFG) timer_write_cfg(reg == TIMER2_CFG, v);
        break;
    case SOC_CLOCK:     soc.clock_regs[reg / 4] = v; break;
    case SOC_PROC_CTL:  proc_ctl_write(reg, v); break;
    case SOC_MAILBOX:   if (!g_dev_replay) mailbox_write(reg, v); break;
    case SOC_CACHE_CTL:
        if (reg != 0) break;
        if ((v & CACHE_CTL_VECT_REMAP) && !(soc.cache_ctl & CACHE_CTL_VECT_REMAP) && !soc.dram_remapped)
            fprintf(stderr, "soc: vector remap to IRAM is not implemented\n");
        soc.cache_ctl = v;
        break;
    case SOC_GPIO:      gpio_write(reg, v); break;
    default: break;
    }
}

int dev_decode(uint32_t addr) {
    return (addr - BCM_BASE < BCM_SIZE) || (addr >> 16) == (SOC_BASE >> 16) || (addr >> 16) == (DEVREG_BASE >> 16) ||
           (addr - IDE_BASE < PAGE) || (addr - USB_BASE < PAGE) || (addr - CACHE_ARRAY_BASE < CACHE_ARRAY_SIZE + PAGE);
}

static void dev_trace(uint32_t addr, int is_write, uint32_t value, int size);
static int g_trace_open;                                /* the MMIO trace file is open */

int dev_write_burst(uint32_t addr, const uint8_t *data, uint32_t count, unsigned width) {
    if (g_trace_open || addr - BCM_BASE >= BCM_SIZE) return 0;
    if (!bcm_write_burst(addr, data, count, width)) return 0;
    g_access_count += count / width;
    return 1;
}

void dev_set_access_pc(uint32_t pc) { g_access_pc = pc; }
uint32_t dev_access_pc(void) { return g_access_pc; }

void dev_access(uint32_t addr, int is_write, uint32_t *value, int size) {
    uint32_t mask = size >= 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    uint32_t v = is_write ? *value & mask : 0;
    g_access_count++;
    if (addr - BCM_BASE < BCM_SIZE) {
        bcm_access(addr, is_write, &v, size);
    } else if ((addr >> 16) == (SOC_BASE >> 16)) {
        if (is_write) soc_write(addr & 0xffff, v); else v = soc_read(addr & 0xffff);
    } else if ((addr >> 16) == (DEVREG_BASE >> 16)) {
        uint32_t off = addr & 0xffff, page = off & ~(PAGE - 1), reg = off & (PAGE - 1);
        if (off - DEVREG_I2C < DEVREG_I2C_SIZE) {
            i2c_access(off - DEVREG_I2C, is_write, &v, size);
        } else if (off - DEVREG_I2S < DEVREG_I2S_SIZE) {
            i2s_access(off - DEVREG_I2S, is_write, &v, size);
        } else if (page == DEVREG_WHEEL) {
            if (is_write) wheel_write(reg, v); else v = wheel_read(reg);
        } else if (page == DEVREG_DEV_CTL) {
            if (is_write) soc.dev_regs[reg / 4] = v;
            else v = soc.dev_regs[reg / 4] | (reg == DEV_USB_STATUS ? DEV_USB_READY : 0);
        } else if (off == DEVREG_PIEZO) {
            /* The firmware writes 0, then 0x80800055 for each click of the
             * wheel. It never ends a click itself: the hardware ticks once. */
            if (is_write) {
                if (v & PIEZO_ON) mach_click();
                soc.piezo = v;
            } else {
                v = soc.piezo;
            }
        } else if (off - DEVREG_SER0 < DEVREG_SER0_SIZE) {
            if (is_write) serial_write(off, v); else v = serial_read(off);
        }                                               /* else: not modelled, reads 0 */
    } else if (addr - IDE_BASE < PAGE) {
        ide_access(addr - IDE_BASE, is_write, &v, size);
    } else if (addr - CACHE_ARRAY_BASE < CACHE_ARRAY_SIZE) {
        uint32_t off = addr - CACHE_ARRAY_BASE;
        if (off + (uint32_t)size <= CACHE_ARRAY_SIZE) {
            if (is_write) memcpy(soc.cache_arrays + off, &v, (size_t)size);
            else memcpy(&v, soc.cache_arrays + off, (size_t)size);
        }
    } else if (addr - MMAP_BASE < PAGE) {
        uint32_t reg = addr - MMAP_BASE;
        if (is_write) mmap_write(reg, v); else v = soc.mmap_regs[reg / 4];
    }                                                   /* else (USB): reads 0 */
    if (!is_write) *value = v & mask;
    dev_trace(addr, is_write, is_write ? v : *value, size);
}

/* MMIO_TRACE=<max lines>: write each device access to mmio-trace.log, one
 * line each: address, R or W, value, size, guest pc (and c1 for the COP). MMIO_TRACE_FROM=<n>: start after n
 * accesses, MMIO_TRACE_FROM=key: start at the first key input.
 * MMIO_TRACE_RANGE=lo-hi: only accesses to [lo, hi]. MMIO_TRACE_QUIET=1:
 * no timer and no BCM accesses (most of the trace). */
static void dev_trace(uint32_t addr, int is_write, uint32_t value, int size) {
    static FILE *f;
    static int on = -1;
    static unsigned long n, max;
    static unsigned long long from;                     /* MMIO_TRACE_FROM: skip the first accesses */
    static uint32_t lo, hi = 0xffffffffu;               /* MMIO_TRACE_RANGE=lo-hi: only these addresses */
    static int quiet;                                   /* MMIO_TRACE_QUIET=1: no timers, no BCM */
    if (g_trace_key_seen == 1) { from = 0; g_trace_key_seen = 2; }
    if (on < 0) {
        const char *e = getenv("MMIO_TRACE"), *dir = getenv("RUNNER_TMP"), *fr = getenv("MMIO_TRACE_FROM");
        from = fr ? (strcmp(fr, "key") ? strtoull(fr, NULL, 0) : ~0ull) : 0;
        quiet = getenv("MMIO_TRACE_QUIET") != NULL;
        const char *rg = getenv("MMIO_TRACE_RANGE");
        if (rg) {
            char *end;
            lo = (uint32_t)strtoul(rg, &end, 0);
            if (*end == '-') hi = (uint32_t)strtoul(end + 1, NULL, 0);
        }
        char path[512];
        on = e != NULL;
        max = e ? strtoul(e, NULL, 0) : 0;
        if (max < 1000) max = 3000000;
        snprintf(path, sizeof path, "%smmio-trace.log", (dir && *dir) ? dir : "/data/local/tmp/");
        if (on) f = fopen(path, "w");
        g_trace_open = f != NULL;
    }
    if (!f || g_dev_replay || n >= max || g_access_count < from || addr < lo || addr > hi) return;
    if (quiet && ((addr >> 8) == 0x600050u || addr - BCM_BASE < BCM_SIZE)) return;
    fprintf(f, "%08x %c %08x %d @%08x%s\n", addr, is_write ? 'W' : 'R', value, size, g_access_pc,
            g_core ? " c1" : "");                       /* the COP, as in the QEMU log */
    if (++n == max) fflush(f);
}

void dev_reset(void) {
    memset(&soc, 0, sizeof soc);
    /* what the boot ROM leaves: the Broadcom chip has power, and the chip
     * version registers say PP5022 */
    soc.dev_regs[GPO32_VAL / 4] = GPO32_BCM_POWER;
    soc.cop_sleeping = 1;                   /* held in reset until the first wake */
    wheel_config_env();
    soc.dev_regs[0] = PP_VER1;
    soc.dev_regs[1] = PP_VER2;
    dma_reset();
    gpio_reset();
    backlight_reset();
    i2s_reset();
    i2c_reset();
    ide_reset();
    bcm_reset();
}

void dev_status(char *buf, int len) {
    char ide[64], audio[64];
    ide_status(ide, sizeof ide);
    i2s_status(audio, sizeof audio);
    snprintf(buf, (size_t)len, "raw=%08x/%08x forced=%08x/%08x en=%08x/%08x fiq=%08x/%08x t1=%08x t2=%08x "
             "sleep=%u dev=%llu %s %s",
             soc.irq_raw[0], soc.irq_raw[1], soc.irq_forced[0], soc.irq_forced[1],
             soc.cpu_enabled[0], soc.cpu_enabled[1], soc.cpu_fiq[0], soc.cpu_fiq[1], soc.timer_cfg[0],
             soc.timer_cfg[1], soc.cpu_sleeping, (unsigned long long)g_access_count, ide, audio);
}

int dev_remapped(void) { return (int)soc.dram_remapped; }

int snap_rw(FILE *f, void *p, size_t n, int save) {
    size_t done = save ? fwrite(p, 1, n, f) : fread(p, 1, n, f);
    return done == n ? 0 : -1;
}

int dev_snapshot(FILE *f, int save) {
    uint32_t usec = timers_usec();
    if (snap_rw(f, &soc, sizeof soc, save) || snap_rw(f, &usec, sizeof usec, save)) return -1;
    if (dma_snapshot(f, save) || gpio_snapshot(f, save) || backlight_snapshot(f, save) || i2c_snapshot(f, save) ||
        i2s_snapshot(f, save) || ide_snapshot(f, save))
        return -1;
    if (save ? bcm_snapshot_save(f) : bcm_snapshot_load(f)) return -1;
    if (!save) {
        soc.cpu_sleeping = 0;           /* a wake without cause is harmless: the idle loop sleeps again */
        timers_resume(usec);
    }
    return 0;
}
