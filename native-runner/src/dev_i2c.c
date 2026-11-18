/* The PP502x I2C controller and the PCF50605 power management unit on its
 * bus. Port of qemu-david hw/arm/ipod_video/ipod_video_i2c.c and
 * ipod_video_pcf50605.c.
 *
 * The controller takes a slave address, up to four data bytes and a length,
 * and does the transfer when the guest sets SEND in CTRL. STATUS bits 2:0
 * are a result code: 0 is success. Two devices are on the bus: the PMU at
 * 7-bit address 0x08 and the WM8758 codec at 0x1a. An address without a
 * device gets a NAK code (the retailOS also talks to 0x2a).
 *
 * The data bytes are byte registers 4 bytes apart (DATA(n)), and the PMU
 * driver writes them so. The codec driver of the retailOS writes one 32-bit
 * word to DATA(0) instead: its bytes are the data bytes 0-3, low byte first.
 * Before, the model took only the low byte, and the codec's volume level
 * (the second byte) was lost.
 *
 * The firmware cannot boot without the PMU: it starts an A/D conversion and
 * waits for the ready flag with no timeout.
 */
#include "dev.h"
#include <string.h>
#include <time.h>

/* ---------- PCF50605 ---------- */

#define PMU_I2C_ADDR    0x08
#define PMU_NUM_REGS    0x40
#define PMU_RTCSC       0x0a    /* seconds, minutes, hours, weekday, day, month, year: BCD */
#define PMU_RTCMN       0x0b
#define PMU_RTCHR       0x0c
#define PMU_RTCWD       0x0d
#define PMU_RTCDT       0x0e
#define PMU_RTCMT       0x0f
#define PMU_RTCYR       0x10
#define PMU_ADCC2       0x2f
#define PMU_ADCS1       0x30
#define PMU_ADCS2       0x31
#define PMU_ADCS3       0x32
#define PMU_ADCC2_START 0x01
#define PMU_ADCC2_MUX   0x1e
#define PMU_ADCS2_ADCRDY 0x80
#define PMU_MUX_ADCIN1_RES   0x2
#define PMU_MUX_ADCIN1_SUBTR 0x3
#define PMU_ADC_FULL_SCALE   0x3ff
#define PMU_ADC_BATTERY_RES   682   /* 4000 mV on Rockbox's scale */
#define PMU_RTC_EPOCH_YEAR 100      /* the year register counts from 2000 */

static struct {
    uint8_t regs[PMU_NUM_REGS];
    uint8_t ptr;
    uint8_t expect_ptr;
    int64_t rtc_offset;             /* seconds added to the host clock */
    uint16_t battery;               /* the ADC value of the battery channel (power_set) */
} pmu;

static uint16_t pmu_adc_sample(unsigned mux) {
    switch (mux) {
    case PMU_MUX_ADCIN1_RES:   return PMU_ADC_BATTERY_RES;
    case PMU_MUX_ADCIN1_SUBTR: return pmu.battery;
    default:                   return PMU_ADC_FULL_SCALE;   /* open circuit: no accessory */
    }
}

/* ---------- battery and power (from the app) ----------
 * The firmware reads the battery from ADC channel 3 (ADCIN1 subtractive).
 * The table gives the value for a level in percent: measured on the
 * firmware's battery icon (filled columns of 21) without external power.
 * 290 is just above the empty icon (250 and below), 300 shows it red. */
static const struct { uint8_t percent; uint16_t raw; } battery_table[] = {
    { 0, 290 }, { 5, 300 }, { 33, 320 }, { 52, 340 }, { 71, 390 }, { 90, 450 }, { 100, 500 },
};

static uint16_t battery_raw(unsigned percent) {
    unsigned n = sizeof battery_table / sizeof battery_table[0];
    if (percent >= battery_table[n - 1].percent) return battery_table[n - 1].raw;
    unsigned i = 1;
    while (battery_table[i].percent < percent) i++;
    unsigned p0 = battery_table[i - 1].percent, p1 = battery_table[i].percent;
    unsigned r0 = battery_table[i - 1].raw, r1 = battery_table[i].raw;
    return (uint16_t)(r0 + (percent - p0) * (r1 - r0) / (p1 - p0));
}

/* External power: GPIO L3 low, the FireWire power input. GPIO L4 high is
 * USB power: the firmware then took 12 s or more to show a plug-in (it
 * probably waits for a computer on USB first), and once it missed an
 * unplug. With L3 it showed the plug-in after 1 s. So L4 stays low: no USB,
 * and no risk of the firmware's disk mode. Charging: GPIO B0 low. Without
 * power the firmware ignores B0. */
#define POWER_PORT      11u             /* L */
#define FIREWIRE_BIT    3u
#define USB_BIT         4u
#define CHARGING_PORT   1u              /* B */
#define CHARGING_BIT    0u

void power_set(unsigned percent, char state) {
    pmu.battery = battery_raw(percent > 100 ? 100 : percent);
    gpio_set_input(POWER_PORT, USB_BIT, 0);
    gpio_set_input(POWER_PORT, FIREWIRE_BIT, state == 'u');
    gpio_set_input(CHARGING_PORT, CHARGING_BIT, state != 'c');
}

/* the conversion is complete when the write returns */
static void pmu_adc_start(void) {
    unsigned mux = (pmu.regs[PMU_ADCC2] & PMU_ADCC2_MUX) >> 1;
    uint16_t raw = pmu_adc_sample(mux);
    pmu.regs[PMU_ADCS1] = (uint8_t)(raw >> 2);
    pmu.regs[PMU_ADCS2] = (uint8_t)(PMU_ADCS2_ADCRDY | (raw & 3));
    pmu.regs[PMU_ADCS3] = 0;
    pmu.regs[PMU_ADCC2] &= (uint8_t)~PMU_ADCC2_START;
}

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return ((v >> 4) & 0xf) * 10 + (v & 0xf); }

/* seconds since 1970 of a UTC date (the C library of the target has no timegm) */
static int64_t utc_seconds(const struct tm *tm) {
    int64_t y = tm->tm_year + 1900, m = tm->tm_mon + 1;
    if (m <= 2) { y--; m += 12; }
    int64_t days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + tm->tm_mday - 719469;
    return ((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec;
}

static void pmu_now(struct tm *tm) {
    time_t t = time(NULL) + (time_t)pmu.rtc_offset;
    gmtime_r(&t, tm);
}

static uint8_t pmu_rtc_read(uint8_t reg) {
    struct tm tm;
    pmu_now(&tm);
    switch (reg) {
    case PMU_RTCSC: return to_bcd(tm.tm_sec);
    case PMU_RTCMN: return to_bcd(tm.tm_min);
    case PMU_RTCHR: return to_bcd(tm.tm_hour);
    case PMU_RTCWD: return to_bcd(tm.tm_wday);
    case PMU_RTCDT: return to_bcd(tm.tm_mday);
    case PMU_RTCMT: return to_bcd(tm.tm_mon + 1);
    default:        return to_bcd(tm.tm_year - PMU_RTC_EPOCH_YEAR);
    }
}

/* The guest writes all seven clock registers in one transfer: the year,
 * the last one, sets the new time. */
static void pmu_rtc_write(uint8_t reg, uint8_t value) {
    struct tm tm;
    pmu.regs[reg] = value;
    if (reg != PMU_RTCYR) return;
    pmu_now(&tm);
    tm.tm_sec  = from_bcd(pmu.regs[PMU_RTCSC]);
    tm.tm_min  = from_bcd(pmu.regs[PMU_RTCMN]);
    tm.tm_hour = from_bcd(pmu.regs[PMU_RTCHR]);
    tm.tm_mday = from_bcd(pmu.regs[PMU_RTCDT]);
    tm.tm_mon  = from_bcd(pmu.regs[PMU_RTCMT]) - 1;
    tm.tm_year = from_bcd(value) + PMU_RTC_EPOCH_YEAR;
    pmu.rtc_offset = utc_seconds(&tm) - (int64_t)time(NULL);
}

static uint8_t pmu_read_reg(uint8_t reg) {
    if (reg >= PMU_NUM_REGS) return 0;          /* no such register */
    if (reg >= PMU_RTCSC && reg <= PMU_RTCYR) return pmu_rtc_read(reg);
    return pmu.regs[reg];           /* the ID register (0x00) reads 0, not 0x24 */
}

static void pmu_write_reg(uint8_t reg, uint8_t value) {
    if (reg >= PMU_NUM_REGS) return;
    if (reg >= PMU_RTCSC && reg <= PMU_RTCYR) { pmu_rtc_write(reg, value); return; }
    pmu.regs[reg] = value;
    if (reg == PMU_ADCC2 && (value & PMU_ADCC2_START)) pmu_adc_start();
}

/* ---------- WM8758 codec ----------
 * A write is two bytes: (register << 1) | bit 8 of the value, then bits 7-0.
 * The codec cannot be read. The model keeps the 9-bit registers and gives
 * the headphone volume (OUT1) to mach_volume(): level in bits 5-0 (0x39 =
 * 0 dB, 1 dB per step), bit 6 mutes, and bit 8 (VU) of a write to either
 * channel makes both values take effect. The samples do not change: the
 * app applies the volume to its audio track. */
#define CODEC_I2C_ADDR  0x1a
#define CODEC_REGS      64
#define CODEC_LOUT1VOL  0x34
#define CODEC_ROUT1VOL  0x35
#define CODEC_VU        0x100u
#define CODEC_MUTE      0x40u
#define CODEC_LEVEL     0x3fu
#define CODEC_0DB       0x39u

static struct {
    uint16_t regs[CODEC_REGS];
    uint8_t first, n;               /* the first byte of a write, bytes so far */
    uint32_t left_cb, right_cb;     /* the volume of the last event */
    int sent;                       /* an event went out */
} codec;

static uint32_t codec_cb(uint16_t v) {
    if (v & CODEC_MUTE) return VOLUME_MUTE;
    unsigned level = v & CODEC_LEVEL;
    return level >= CODEC_0DB ? 0 : (CODEC_0DB - level) * 10u;
}

static void codec_write_reg(unsigned reg, uint16_t value) {
    if (reg >= CODEC_REGS) return;
    codec.regs[reg] = value;
    if ((reg != CODEC_LOUT1VOL && reg != CODEC_ROUT1VOL) || !(value & CODEC_VU)) return;
    uint32_t l = codec_cb(codec.regs[CODEC_LOUT1VOL]), r = codec_cb(codec.regs[CODEC_ROUT1VOL]);
    if (codec.sent && l == codec.left_cb && r == codec.right_cb) return;
    codec.left_cb = l;
    codec.right_cb = r;
    codec.sent = 1;
    mach_volume(l, r);
}

static void codec_send(uint8_t data) {
    if (codec.n++ == 0) { codec.first = data; return; }
    if (codec.n == 2) codec_write_reg(codec.first >> 1, (uint16_t)((codec.first & 1) << 8 | data));
}

/* The bus: the device of the transfer. These three return 0, or 1 for no
 * device. */
static uint8_t bus_dev;

static int bus_start_send(uint8_t addr7) {
    bus_dev = addr7;
    if (addr7 == CODEC_I2C_ADDR) { codec.n = 0; return 0; }
    if (addr7 != PMU_I2C_ADDR) return 1;
    pmu.expect_ptr = 1;             /* the first byte of a write is the register number */
    return 0;
}
static int bus_start_recv(uint8_t addr7) {
    bus_dev = addr7;
    return addr7 != PMU_I2C_ADDR;   /* a read continues at the register pointer */
}
static void bus_send(uint8_t data) {
    if (bus_dev == CODEC_I2C_ADDR) { codec_send(data); return; }
    if (pmu.expect_ptr) { pmu.ptr = data; pmu.expect_ptr = 0; }
    else pmu_write_reg(pmu.ptr++, data);
}
static uint8_t bus_recv(void) {
    return pmu_read_reg(pmu.ptr++);
}

/* ---------- controller ---------- */

#define I2C_NUM_REGS    0x40
#define I2C_CTRL        0x00
#define I2C_ADDR        0x04
#define I2C_ADDR2       0x08
#define I2C_DATA(n)     (0x0c + 4 * (n))
#define I2C_STATUS      0x1c
#define I2C_CTRL_SEND       0x80
#define I2C_CTRL_READ       0x20
#define I2C_CTRL_LEN_MASK   0x06
#define I2C_CTRL_COMBINED   0x48    /* write one byte, repeated start, read one byte */
#define I2C_STATUS_CODE     0x07
#define I2C_CODE_NAK        0x01

static uint8_t i2c_regs[I2C_NUM_REGS];

static uint8_t i2c_run(void) {
    uint8_t ctrl = i2c_regs[I2C_CTRL], addr = i2c_regs[I2C_ADDR];
    unsigned len = ((ctrl & I2C_CTRL_LEN_MASK) >> 1) + 1;

    if ((ctrl & ~I2C_CTRL_SEND) == I2C_CTRL_COMBINED) {
        if (bus_start_send(addr >> 1)) return I2C_CODE_NAK;
        bus_send(i2c_regs[I2C_DATA(0)]);
        if (bus_start_recv(i2c_regs[I2C_ADDR2] >> 1)) return I2C_CODE_NAK;
        i2c_regs[I2C_DATA(1)] = bus_recv();     /* the received byte follows the sent byte */
        return 0;
    }
    if (ctrl & I2C_CTRL_READ) {
        if (bus_start_recv(addr >> 1)) return I2C_CODE_NAK;
        for (unsigned i = 0; i < len; i++) i2c_regs[I2C_DATA(i)] = bus_recv();
        return 0;
    }
    if (bus_start_send(addr >> 1)) return I2C_CODE_NAK;
    for (unsigned i = 0; i < len; i++) bus_send(i2c_regs[I2C_DATA(i)]);
    return 0;
}

static void i2c_write_byte(uint32_t off, uint8_t value) {
    if (off >= I2C_NUM_REGS || off == I2C_STATUS) return;   /* the controller owns STATUS */
    i2c_regs[off] = value;
    if (off == I2C_CTRL && (value & I2C_CTRL_SEND)) {
        i2c_regs[I2C_STATUS] = i2c_run() & I2C_STATUS_CODE;
        i2c_regs[I2C_CTRL] &= (uint8_t)~I2C_CTRL_SEND;      /* SEND clears itself; BUSY is never set */
    }
}

void i2c_reset(void) {
    memset(i2c_regs, 0, sizeof i2c_regs);
    memset(&pmu, 0, sizeof pmu);
    power_set(71, 'f');                             /* until the app sends its state: power, 71 % */
    memset(&codec, 0, sizeof codec);
}

/* byte registers: a wider access is one access for each byte, low byte first */
void i2c_access(uint32_t off, int is_write, uint32_t *value, int size) {
    uint32_t v = 0;
    if (is_write && off == I2C_DATA(0) && size == 4) {  /* the codec driver: data bytes 0-3 in one word */
        for (int i = 0; i < 4; i++) i2c_regs[I2C_DATA(i)] = (uint8_t)(*value >> (8 * i));
        return;
    }
    for (int i = 0; i < size; i++) {
        if (is_write) i2c_write_byte(off + (uint32_t)i, (uint8_t)(*value >> (8 * i)));
        else if (off + (uint32_t)i < I2C_NUM_REGS) v |= (uint32_t)i2c_regs[off + (uint32_t)i] << (8 * i);
    }
    if (!is_write) *value = v;
}

int i2c_snapshot(FILE *f, int save) {
    if (snap_rw(f, i2c_regs, sizeof i2c_regs, save) || snap_rw(f, &pmu, sizeof pmu, save) ||
        snap_rw(f, &codec, sizeof codec, save))
        return -1;
    if (!save && codec.sent) mach_volume(codec.left_cb, codec.right_cb);   /* the app gets it again */
    return 0;
}
