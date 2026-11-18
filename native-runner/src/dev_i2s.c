/* The I2S unit of the PP502x (0x70002800) and the audio output of the runner.
 *
 * The QEMU model has no I2S unit: there, the firmware's playback never
 * starts. The register names follow Rockbox (firmware/export/pp5020.h).
 *
 * What the firmware does to play a song (trace of "All That", pc in the
 * firmware):
 *   1. IISCONFIG: FIFO format 0x70 (two 16-bit samples in one 32-bit word),
 *      then TX FIFO enable (bit 29), each as a read-modify-write.
 *   2. DMA block B, channel 1: RAM address, peripheral address IISFIFO_WR,
 *      32-bit port, CMD 0xcd0211fc = START, INTR, RAM_TO_PER, WAIT_REQ for
 *      request 2 (the I2S TX FIFO), 4608 bytes.
 *   3. The DMA interrupt (line 26) is an FIQ (CPU_INT_PRIORITY bit 26). The
 *      handler gives the channel the next buffer.
 *
 * The model: the TX FIFO always has room. A DMA transfer for request 2
 * takes the time that its samples play at 44.1 kHz, 16 bit, stereo
 * (dev_dma.c). The samples go to the audio output when the transfer starts.
 *
 * Audio output: AUDIO_OUT=<path> writes raw PCM (signed 16 bit, little
 * endian, stereo, 44100 Hz) to the file, AUDIO_OUT=- to stdout. stdout is
 * non-blocking: if the reader is slow, samples are lost, but the guest does
 * not stop. Without AUDIO_OUT the samples are counted only.
 */
#include "dev.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IIS_CONFIG      0x00
#define IIS_CLK         0x08
#define IIS_FIFO_CFG    0x0c
#define IIS_FIFO_WR     0x40
#define IIS_FIFO_RD     0x80
#define IIS_REGS        0x100

#define IIS_FIFO_CFG_TXCLR  (1u << 8)       /* self-clearing */
#define IIS_FIFO_CFG_RXCLR  (1u << 12)
#define IIS_TX_FREE_SHIFT   16
#define IIS_TX_FIFO_DEPTH   16u             /* 32-bit entries */

#define DIRECT_BUF          4096u           /* samples that the CPU writes to the FIFO itself */

static struct {
    uint32_t regs[IIS_REGS / 4];
} i2s;

static int g_out_fd = -2;                   /* -2: AUDIO_OUT not read yet, -1: no output */
static uint64_t g_bytes_out, g_bytes_lost;
static uint64_t g_last_out_ns, g_max_gap_ns;    /* host time between two outputs: the largest in a status period */
static uint8_t g_direct[DIRECT_BUF];
static uint32_t g_direct_n;

static void out_open(void) {
    const char *e = getenv("AUDIO_OUT");
    g_out_fd = -1;
    if (!e || !*e) return;
    if (!strcmp(e, "-")) {
        g_out_fd = 1;
        fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
        return;
    }
    g_out_fd = open(e, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_out_fd < 0) fprintf(stderr, "audio: cannot open %s: %s\n", e, strerror(errno));
}

void i2s_out(const void *pcm, uint32_t len) {
    if (g_dev_replay || !len) return;
    if (g_out_fd == -2) out_open();
    g_bytes_out += len;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    if (g_last_out_ns && now - g_last_out_ns > g_max_gap_ns) g_max_gap_ns = now - g_last_out_ns;
    g_last_out_ns = now;
    if (g_out_fd < 0) return;
    const uint8_t *p = pcm;
    while (len) {
        ssize_t n = write(g_out_fd, p, len);
        if (n > 0) { p += n; len -= (uint32_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        g_bytes_lost += len;                    /* EAGAIN: the reader is slow */
        return;
    }
}

static void direct_flush(void) {
    i2s_out(g_direct, g_direct_n);
    g_direct_n = 0;
}

void i2s_reset(void) {
    memset(&i2s, 0, sizeof i2s);
    g_direct_n = 0;
}

void i2s_access(uint32_t off, int is_write, uint32_t *value, int size) {
    if (off >= IIS_REGS) {
        if (!is_write) *value = 0;
        return;
    }
    if (is_write) {
        switch (off) {
        case IIS_FIFO_WR:                       /* the CPU feeds the FIFO itself */
            memcpy(g_direct + g_direct_n, value, (size_t)size);
            g_direct_n += (uint32_t)size;
            if (g_direct_n + 4 > DIRECT_BUF) direct_flush();
            break;
        case IIS_FIFO_CFG:
            i2s.regs[off / 4] = *value & ~(IIS_FIFO_CFG_TXCLR | IIS_FIFO_CFG_RXCLR);
            if (*value & IIS_FIFO_CFG_TXCLR) g_direct_n = 0;
            break;
        default:
            i2s.regs[off / 4] = *value;
            break;
        }
        return;
    }
    switch (off) {
    case IIS_FIFO_CFG:                          /* the TX FIFO is always empty */
        *value = (i2s.regs[off / 4] & ~(0x3fu << IIS_TX_FREE_SHIFT)) | IIS_TX_FIFO_DEPTH << IIS_TX_FREE_SHIFT;
        break;
    case IIS_FIFO_WR: case IIS_FIFO_RD:
        *value = 0;
        break;
    default:
        *value = i2s.regs[off / 4];
        break;
    }
}

void i2s_status(char *buf, int len) {
    if (g_direct_n) direct_flush();
    snprintf(buf, (size_t)len, "audio %llu KB (%llu KB lost, gap %u ms)", (unsigned long long)(g_bytes_out >> 10),
             (unsigned long long)(g_bytes_lost >> 10), (unsigned)(g_max_gap_ns / 1000000u));
    g_max_gap_ns = 0;
}

int i2s_snapshot(FILE *f, int save) {
    return snap_rw(f, &i2s, sizeof i2s, save);
}
