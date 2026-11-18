/* The PP502x DMA engine. Port of qemu-david hw/arm/ipod_video/ipod_video_dma.c.
 *
 * Two identical blocks: A (master 0x60008000, channels 0x60009000 + n*0x20,
 * IRQ 27) and B (0x6000a000 / 0x6000b000, IRQ 26). The firmware sends the
 * Broadcom chip's firmware image (about 200 KB) through block A.
 *
 * A transfer moves "count" bytes between RAM (the address walks) and a
 * peripheral port (the address stays), in units of the access width of the
 * peripheral descriptor. It starts on the rising edge of START in CMD and
 * is complete when the write returns.
 *
 * Exception (not in the QEMU model): a transfer that waits for the request
 * of the I2S TX FIFO (WAIT_REQ, request 2) is audio. Its samples go to the
 * audio output at the start (dev_i2s.c). The channel is busy for the time
 * that the samples play, and at the end START clears and the interrupt
 * comes. The next transfer plays from the end of the last one when its
 * start write comes up to one buffer late (the FIQ comes late in the
 * runner), as the I2S clock runs on: else the stream lost 0.8% of its time.
 */
#include "dev.h"
#include <string.h>

#define DMA_NUM_CHANNELS    8
#define DMA_CHANNEL_BASE    0x1000
#define DMA_CHANNEL_STRIDE  0x20

/* master registers */
#define DMA_MASTER_CONTROL  0x00
#define DMA_MASTER_STATUS   0x04
#define DMA_REQ_STATUS      0x08
#define DMA_MASTER_STATUS_CH0 24

/* channel registers */
#define DMA_CMD         0x00
#define DMA_STATUS      0x04
#define DMA_RAM_ADDR    0x10
#define DMA_RAM_DESC    0x14
#define DMA_PER_ADDR    0x18
#define DMA_PER_DESC    0x1c

#define DMA_CMD_SIZE        0xfffcu         /* bytes - 4 */
#define DMA_CMD_REQ_SHIFT   16              /* request line of WAIT_REQ */
#define DMA_CMD_REQ_MASK    (0xfu << DMA_CMD_REQ_SHIFT)
#define DMA_CMD_WAIT_REQ    (1u << 24)
#define DMA_CMD_RAM_TO_PER  (1u << 27)
#define DMA_CMD_INTR        (1u << 30)
#define DMA_CMD_START       (1u << 31)
#define DMA_STATUS_INTR     (1u << 30)
#define DMA_STATUS_BUSY     (1u << 31)
#define DMA_DESC_WIDTH_SHIFT 28
#define DMA_DESC_WIDTH_MASK (0x7u << DMA_DESC_WIDTH_SHIFT)

struct dma_chan {
    uint32_t cmd, status, ram_addr, ram_desc, per_addr, per_desc;
};

static struct dma_block {
    uint32_t master_control, req_status;
    struct dma_chan chan[DMA_NUM_CHANNELS];
} dma[2];

static const int dma_irq[2] = { IRQ_DMA_A, IRQ_DMA_B };

/* the I2S transfers: end time per channel (0: none), and the end of the
 * last one. Not in the snapshot: a snapshot is taken before playback. */
static uint64_t g_audio_end[2][DMA_NUM_CHANNELS], g_audio_free_at;
static uint64_t g_audio_frac;               /* the part of a microsecond that the last end left: in 1/I2S_BYTES_PER_SEC us */
static uint64_t g_audio_next;               /* the first end, or 0 */

static void audio_update_next(void) {
    g_audio_next = 0;
    for (int b = 0; b < 2; b++)
        for (int n = 0; n < DMA_NUM_CHANNELS; n++)
            if (g_audio_end[b][n] && (!g_audio_next || g_audio_end[b][n] < g_audio_next))
                g_audio_next = g_audio_end[b][n];
}

/* access width of a descriptor: 0 = 8 bit, 1 = 16, 2 = 32 */
static unsigned dma_width(uint32_t desc) {
    uint32_t code = (desc & DMA_DESC_WIDTH_MASK) >> DMA_DESC_WIDTH_SHIFT;
    return code == 0 ? 1 : code == 1 ? 2 : 4;
}

/* the interrupt line of a block is the OR of the latched channel interrupts */
static void dma_update_irq(int b) {
    int level = 0;
    for (int i = 0; i < DMA_NUM_CHANNELS; i++)
        if (dma[b].chan[i].status & DMA_STATUS_INTR) level = 1;
    intc_set_irq(dma_irq[b], level);
}

static void audio_start(int b, int n, uint32_t count) {
    struct dma_chan *c = &dma[b].chan[n];
    static uint8_t buf[0x10000];
    uint64_t now = cpu_guest_time_us();
    uint64_t play = ((uint64_t)count * 1000000u + g_audio_frac) / I2S_BYTES_PER_SEC;
    /* The I2S clock runs on: a buffer that the firmware starts up to one
     * buffer late (its FIQ came late) plays from the end of the last one. */
    uint64_t start = now;
    if (g_audio_free_at && now < g_audio_free_at + play) start = g_audio_free_at;
    else g_audio_frac = 0;                              /* the stream stopped: a new start */
    g_audio_frac = ((uint64_t)count * 1000000u + g_audio_frac) % I2S_BYTES_PER_SEC;
    if (mach_ram_read(c->ram_addr, buf, count)) memset(buf, 0, count);
    i2s_out(buf, count);
    g_audio_free_at = start + play;
    g_audio_end[b][n] = g_audio_free_at;
    c->status = DMA_STATUS_BUSY;
    audio_update_next();
}

/* the firmware cleared START of a busy channel: the transfer stops */
static void audio_stop(int b, int n) {
    if (!g_audio_end[b][n]) return;
    g_audio_end[b][n] = 0;
    dma[b].chan[n].status = 0;
    g_audio_free_at = 0;
    audio_update_next();
}

void dma_tick(uint64_t now_us) {
    if (!g_audio_next || now_us < g_audio_next) return;
    for (int b = 0; b < 2; b++)
        for (int n = 0; n < DMA_NUM_CHANNELS; n++) {
            struct dma_chan *c = &dma[b].chan[n];
            if (!g_audio_end[b][n] || now_us < g_audio_end[b][n]) continue;
            g_audio_end[b][n] = 0;
            c->cmd &= ~DMA_CMD_START;
            c->status = DMA_STATUS_INTR;
            if (c->cmd & DMA_CMD_INTR) dma_update_irq(b);
        }
    audio_update_next();
}

uint64_t dma_next_deadline_us(void) {
    return g_audio_next;
}

static void dma_run(int b, int n) {
    struct dma_chan *c = &dma[b].chan[n];
    uint32_t count = (c->cmd & DMA_CMD_SIZE) + 4;
    unsigned width = dma_width(c->per_desc);
    static uint8_t buf[0x10000];

    if (c->cmd & DMA_CMD_WAIT_REQ) {
        /* only the I2S FIFO raises a request line here */
        if ((c->cmd & DMA_CMD_REQ_MASK) >> DMA_CMD_REQ_SHIFT == DMA_REQ_I2S) audio_start(b, n, count);
        return;
    }

    if (c->cmd & DMA_CMD_RAM_TO_PER) {
        memset(buf, 0, count);
        if (!g_dev_replay) mach_ram_read(c->ram_addr, buf, count);
        for (uint32_t off = dev_write_burst(c->per_addr, buf, count, width) ? count : 0; off + width <= count;
             off += width) {
            uint32_t v = 0;
            memcpy(&v, buf + off, width);
            dev_access(c->per_addr, 1, &v, (int)width);
        }
    } else {
        for (uint32_t off = 0; off + width <= count; off += width) {
            uint32_t v = 0;
            dev_access(c->per_addr, 0, &v, (int)width);
            memcpy(buf + off, &v, width);
        }
        if (!g_dev_replay) mach_ram_write(c->ram_addr, buf, count);
    }
    /* START stays set: the firmware clears it itself */
    c->status = DMA_STATUS_INTR;
    if (c->cmd & DMA_CMD_INTR) dma_update_irq(b);
}

void dma_reset(void) {
    memset(dma, 0, sizeof dma);
    memset(g_audio_end, 0, sizeof g_audio_end);
    g_audio_free_at = 0;
    g_audio_frac = 0;
    g_audio_next = 0;
}

void dma_access(int b, uint32_t off, int is_write, uint32_t *value) {
    struct dma_block *s = &dma[b];
    if (off < DMA_CHANNEL_BASE) {
        if (is_write) {
            if (off == DMA_MASTER_CONTROL) s->master_control = *value;
            else if (off == DMA_REQ_STATUS) s->req_status = *value;
            return;
        }
        *value = 0;
        if (off == DMA_MASTER_CONTROL) {
            *value = s->master_control;
        } else if (off == DMA_MASTER_STATUS) {
            for (int i = 0; i < DMA_NUM_CHANNELS; i++)
                if (s->chan[i].status & DMA_STATUS_BUSY) *value |= 1u << (DMA_MASTER_STATUS_CH0 + i);
        } else if (off == DMA_REQ_STATUS) {
            *value = s->req_status;
        }
        return;
    }
    unsigned n = (off - DMA_CHANNEL_BASE) / DMA_CHANNEL_STRIDE;
    if (n >= DMA_NUM_CHANNELS) {                    /* no such channel */
        if (!is_write) *value = 0;
        return;
    }
    struct dma_chan *c = &s->chan[n];
    uint32_t reg = off % DMA_CHANNEL_STRIDE;
    if (is_write) {
        uint32_t prev;
        switch (reg) {
        case DMA_CMD:
            prev = c->cmd;
            c->cmd = *value;
            if ((*value & DMA_CMD_START) && !(prev & DMA_CMD_START)) dma_run(b, (int)n);
            else if (!(*value & DMA_CMD_START)) audio_stop(b, (int)n);
            break;
        case DMA_RAM_ADDR: c->ram_addr = *value; break;
        case DMA_RAM_DESC: c->ram_desc = *value; break;
        case DMA_PER_ADDR: c->per_addr = *value; break;
        case DMA_PER_DESC: c->per_desc = *value; break;
        default: break;                             /* STATUS is read-only */
        }
        return;
    }
    switch (reg) {
    case DMA_CMD: *value = c->cmd; break;
    case DMA_STATUS:                                /* a read acknowledges the interrupt */
        *value = c->status;
        c->status &= ~DMA_STATUS_INTR;
        dma_update_irq(b);
        break;
    case DMA_RAM_ADDR: *value = c->ram_addr; break;
    case DMA_RAM_DESC: *value = c->ram_desc; break;
    case DMA_PER_ADDR: *value = c->per_addr; break;
    case DMA_PER_DESC: *value = c->per_desc; break;
    default: *value = 0; break;
    }
}

int dma_snapshot(FILE *f, int save) {
    return snap_rw(f, dma, sizeof dma, save) || snap_rw(f, g_audio_end, sizeof g_audio_end, save) ||
           snap_rw(f, &g_audio_free_at, sizeof g_audio_free_at, save) ||
           snap_rw(f, &g_audio_frac, sizeof g_audio_frac, save) || snap_rw(f, &g_audio_next, sizeof g_audio_next, save);
}
