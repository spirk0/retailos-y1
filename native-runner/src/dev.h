/* Device models of the PP502x machine: internal interface.
 *
 * Each model is a port of the QEMU model that boots the firmware
 * (qemu-david hw/arm/ipod_video/). The QEMU sources give the reasons for
 * each register and constant.
 */
#ifndef DEV_H
#define DEV_H
#include <stdint.h>
#include <stdio.h>

/* interrupt lines of the PP502x interrupt controller */
#define IRQ_TIMER1   0
#define IRQ_TIMER2   1
#define IRQ_MAILBOX  4       /* the COP posted a message to CPU_QUEUE */
#define IRQ_IDE      23
#define IRQ_DMA_B    26
#define IRQ_DMA_A    27
#define IRQ_I2C      40      /* high bank, bit 8: on this machine it is the click wheel */

/* ---- services of the machine (machine.c) ---- */

/* Device-state replay runs (QBOOT_MMIO): a device must not change guest
 * memory or the disk, and must not stop the core. */
extern int g_dev_replay;

/* Guest RAM by guest address, any alias. Returns 0, or -1 if the range is
 * not RAM. */
int mach_ram_read(uint32_t addr, void *buf, uint32_t len);
int mach_ram_write(uint32_t addr, const void *buf, uint32_t len);

/* The firmware's remap unit put DRAM at address 0. */
void mach_remap_dram(void);

/* File descriptor of the disk image, or -1 */
int mach_disk_fd(void);
/* The disk (machine.c): read n sectors (zeros on an error), the size in
 * sectors (0: unknown) */
void mach_disk_read(uint64_t lba, uint32_t n, uint8_t *buf);
uint64_t mach_disk_sectors(void);

/* KEY_LOG=1: at a present, log the host time since the last key input that
 * no present answered yet (the input latency of the runner) */
void mach_key_log_present(void);

/* The firmware ticked the clicker (dev_soc.c): an event for the app */
void mach_click(void);

/* The backlight (dev_backlight.c): GPIO B3 (driver on) and L7 (LED on),
 * the pulses of the unit at 0x7000c300 that step the level. The visible
 * level goes to mach_backlight(): 0 = dark, 1-32. */
void backlight_reset(void);
void backlight_pins(int enable, int led);
void backlight_pulse_length(uint32_t len);
void backlight_pulse(void);
int backlight_level(void);
int backlight_snapshot(FILE *f, int save);
void mach_backlight(int level);

/* The codec's headphone volume changed (dev_i2c.c): the attenuation of each
 * channel in centibels (0 = full volume), or VOLUME_MUTE */
#define VOLUME_MUTE 0xffffu
void mach_volume(uint32_t left_cb, uint32_t right_cb);

/* Guest time in microseconds (cpu.c) */
uint64_t cpu_guest_time_us(void);

/* The guest reads the microsecond clock (cpu.c): a wait loop on it lets
 * guest time make up its lag */
void cpu_clock_poll(void);

/* ---- device bus (dev_soc.c) ---- */

void dev_reset(void);

/* 1 if addr is in a device address range (not RAM) */
int dev_decode(uint32_t addr);

/* One device access. size is 1, 2 or 4. A read sets *value. */
void dev_access(uint32_t addr, int is_write, uint32_t *value, int size);

/* count bytes to one device port in units of width (a DMA transfer), as
 * fast as the device allows. Returns 0 if the caller must make the single
 * writes: the device has no fast path, or the trace is on. */
int dev_write_burst(uint32_t addr, const uint8_t *data, uint32_t count, unsigned width);

/* Guest pc of the following accesses, for the trace */
void dev_set_access_pc(uint32_t pc);
uint32_t dev_access_pc(void);

/* 1 after the firmware's remap unit put DRAM at address 0 */
int dev_remapped(void);

/* Save (save != 0) or load the state of all devices. Returns 0 on success. */
int dev_snapshot(FILE *f, int save);

/* Helper for the snapshot functions: write or read n bytes. */
int snap_rw(FILE *f, void *p, size_t n, int save);

/* interrupt controller */
void intc_set_irq(int line, int level);
int  intc_cpu_irq(void);            /* 1 if an enabled IRQ line is active */
int  intc_cop_irq(void);            /* the same for the COP's enable masks */
int  intc_cpu_fiq(void);            /* 1 if an enabled FIQ line is active (CPU_INT_PRIORITY) */
int  intc_cop_fiq(void);
void dev_set_core(int core);        /* 0 = CPU, 1 = COP: the core of the next accesses */

/* timers */
void timers_tick(uint64_t now_us);
uint64_t timers_next_deadline_us(void);
void timers_resume(uint32_t usec_now);   /* after a capture or snapshot load: continue from this clock value */
uint32_t timers_usec(void);
int  timers_clock_was_read(void);

/* core sleep (CPU_CTL) */
int  proc_cpu_sleeping(void);
void proc_cpu_wake(void);
int  proc_cop_started(void);        /* the CPU woke the COP at least once */
int  proc_cop_sleeping(void);
void proc_cop_wake(void);

/* click wheel input: one character, see wheel_key() in dev_soc.c */
void wheel_key(char c);
/* the gesture of one wheel key: units, guest time per packet, pause before
 * the lift, units per packet (defaults, or WHEEL_UNITS, WHEEL_STEP_US,
 * WHEEL_LIFT_US, WHEEL_PACKET) */
void wheel_config(uint32_t units, uint32_t step_us, uint32_t lift_us, uint32_t per_packet);

void dev_status(char *buf, int len);
uint64_t bcm_presents(void);        /* surfaces presented so far */
void ide_status(char *buf, int len);     /* disk sectors read and the host time for them */

/* ---- the other device files ---- */

void dma_reset(void);
void dma_access(int block, uint32_t off, int is_write, uint32_t *value);
void dma_tick(uint64_t now_us);             /* complete the I2S transfers whose time came */
uint64_t dma_next_deadline_us(void);        /* 0 if nothing is pending */
int  dma_snapshot(FILE *f, int save);

/* I2S unit and audio output (dev_i2s.c) */
#define DMA_REQ_I2S        2                /* DMA request line of the I2S TX FIFO */
#define I2S_BYTES_PER_SEC  176400u          /* 44.1 kHz, 16 bit, stereo */
void i2s_reset(void);
void i2s_access(uint32_t off, int is_write, uint32_t *value, int size);
void i2s_out(const void *pcm, uint32_t len);    /* samples to the audio output */
void i2s_status(char *buf, int len);
int  i2s_snapshot(FILE *f, int save);

/* GPIO ports A-L and their interrupt lines 32-34 (dev_gpio.c). reg is the
 * offset in the GPIO page. gpio_set_input(): an input pin reads level
 * (1 = high, the default). Port 0 is A. */
uint32_t gpio_read(uint32_t reg);
void gpio_write(uint32_t reg, uint32_t v);
void gpio_set_input(unsigned port, unsigned bit, int level);
void gpio_toggle_hold(void);

/* The Y1's battery (dev_i2c.c): level in percent, state 'u' (no external
 * power), 'c' (charging) or 'f' (power, not charging) */
void power_set(unsigned percent, char state);
void gpio_reset(void);
int  gpio_snapshot(FILE *f, int save);

void i2c_reset(void);
void i2c_access(uint32_t off, int is_write, uint32_t *value, int size);
int  i2c_snapshot(FILE *f, int save);

void ide_reset(void);
void ide_access(uint32_t off, int is_write, uint32_t *value, int size);
void ide_tick(uint64_t now_us);             /* complete a DMA command whose time came */
uint64_t ide_next_deadline_us(void);        /* 0 if nothing is pending */
int  ide_snapshot(FILE *f, int save);
int  ide_overlay_snapshot(FILE *f, int save);

void bcm_reset(void);
int  bcm_access(uint32_t addr, int is_write, uint32_t *value, int size);
int  bcm_write_burst(uint32_t addr, const uint8_t *data, uint32_t count, unsigned width);
void bcm_frame_poll(uint64_t now_us);    /* write frame.ppm if the display changed */
int  bcm_snapshot_save(FILE *f);
int  bcm_snapshot_load(FILE *f);
extern int g_frame_presented;            /* 1 after the first frame file */

#endif
