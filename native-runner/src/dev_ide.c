/* The PP502x IDE host adapter and one ATA disk.
 *
 * Port of qemu-david hw/arm/ipod_video/ipod_video_ide.c (the adapter) and
 * of the parts of QEMU hw/ide/core.c that the firmware uses: IDENTIFY in
 * PIO mode, SET FEATURES, the power commands, and READ/WRITE DMA (EXT).
 *
 * Adapter registers (offsets from 0xc3000000):
 *   0x028        IDE0_CFG: bit 3 is the INTRQ latch, a write of 1 clears it
 *   0x1e0-0x1fc  task file, 8 registers 4 bytes apart (data port first)
 *   0x3f8        device control / alternate status
 *   0x400        DMA control: bit 0 GO starts the transfer
 *   0x408/0x40c  DMA length (bytes - 4) and guest address
 * The other words of the 4 KB window are plain storage.
 *
 * The drive raises its interrupt line (IRQ 23) at the end of a command and
 * lowers it when the host reads STATUS or writes a command.
 *
 * Disk writes go to an overlay in memory, not to the image file: each run
 * starts from the same disk. Set DISK_WRITE=1 to write the image.
 */
#include "dev.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IDE_REGION_SIZE 0x1000
#define IDE0_CFG        0x028
#define IDE_TASKFILE    0x1e0
#define IDE_TASKFILE_SIZE 0x020
#define IDE_CONTROL     0x3f8
#define IDE_CONTROL_SIZE 0x008
#define IDE_DMA_CONTROL 0x400
#define IDE_DMA_LENGTH  0x408
#define IDE_DMA_ADDR    0x40c
#define IDE_CFG_INTRQ   (1u << 3)
#define IDE_DMA_GO      (1u << 0)
/* A DMA command completes this long after it starts: the status update and
 * the interrupt come later, as in QEMU, where the block I/O completes in
 * the main loop. The guest depends on it: right after the command it does
 * a read-modify-write of IDE0_CFG, which would acknowledge an INTRQ latch
 * that was already set, and then it would never see the interrupt. */
#define IDE_DMA_LATENCY_US 200

/* status and error bits */
#define ERR_STAT    0x01
#define DRQ_STAT    0x08
#define SEEK_STAT   0x10
#define READY_STAT  0x40
#define BUSY_STAT   0x80
#define ABRT_ERR    0x04
/* device control bits */
#define CTRL_DISABLE_IRQ 0x02
#define CTRL_RESET       0x04
#define CTRL_HOB         0x80
#define DEV_ALWAYS_ON    0xa0
#define DEV_SELECT       0x10

enum { DMA_NONE, DMA_READ, DMA_WRITE };

static struct {
    uint32_t regs[IDE_REGION_SIZE / 4];
    uint32_t nsector;                   /* up to 65536 after the 48-bit transform */
    uint8_t feature, hob_feature, error;
    uint8_t hob_nsector, sector, hob_sector, lcyl, hob_lcyl, hcyl, hob_hcyl;
    uint8_t select, status, ctrl, unit, lba48, intrq, dma_cmd;
    uint32_t data_pos, data_end;        /* PIO data in io_buffer */
    uint32_t done_pending;              /* a DMA command waits for its completion time */
    uint64_t done_at;                   /* guest time of the completion */
    uint8_t io_buffer[512];
} ide;

static const uint16_t identify_data[256] = {
#include "identify_data.h"
};

/* ---------- disk with a write overlay ---------- */

#define OVERLAY_SLOTS 8192              /* 4 MB of written sectors */
static struct { uint64_t key; uint8_t *data; } overlay[OVERLAY_SLOTS];   /* key = lba + 1 */
static unsigned overlay_used;
static int disk_write_through = -1;

static uint8_t *overlay_find(uint64_t lba, int create) {
    unsigned i = (unsigned)((lba * 2654435761u) % OVERLAY_SLOTS);
    for (unsigned n = 0; n < OVERLAY_SLOTS; n++, i = (i + 1) % OVERLAY_SLOTS) {
        if (overlay[i].key == lba + 1) return overlay[i].data;
        if (!overlay[i].key) {
            if (!create || overlay_used >= OVERLAY_SLOTS - 1) return NULL;
            overlay[i].data = malloc(512);
            if (!overlay[i].data) return NULL;
            overlay[i].key = lba + 1;
            overlay_used++;
            return overlay[i].data;
        }
    }
    return NULL;
}

/* host time in disk reads, and the sectors read: for the status line */
static uint64_t g_read_ns, g_read_sectors;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void ide_status(char *buf, int len) {
    snprintf(buf, (size_t)len, "disk=%llus/%llums", (unsigned long long)g_read_sectors,
             (unsigned long long)(g_read_ns / 1000000u));
}

static void disk_read(uint64_t lba, uint32_t n, uint8_t *buf) {
    uint64_t t0 = mono_ns();
    mach_disk_read(lba, n, buf);
    g_read_ns += mono_ns() - t0;
    g_read_sectors += n;
    if (!overlay_used) return;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = overlay_find(lba + i, 0);
        if (o) memcpy(buf + (size_t)i * 512, o, 512);
    }
}

static void disk_write(uint64_t lba, uint32_t n, const uint8_t *buf) {
    if (disk_write_through < 0) disk_write_through = getenv("DISK_WRITE") != NULL;
    if (disk_write_through) {
        int fd = mach_disk_fd();
        if (fd >= 0 && pwrite(fd, buf, (size_t)n * 512, (off_t)(lba * 512)) < 0)
            fprintf(stderr, "ide: write error at sector %llu\n", (unsigned long long)lba);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = overlay_find(lba + i, 1);
        if (o) memcpy(o, buf + (size_t)i * 512, 512);
        else fprintf(stderr, "ide: write overlay is full, sector %llu dropped\n", (unsigned long long)(lba + i));
    }
}

/* ---------- drive ---------- */

static void ide_set_irq(void) {
    if (ide.ctrl & CTRL_DISABLE_IRQ) return;
    ide.intrq = 1;                      /* the adapter latches the rising edge */
    intc_set_irq(IRQ_IDE, 1);
}

static void ide_lower_irq(void) {
    intc_set_irq(IRQ_IDE, 0);
}

static void ide_clear_hob(void) {
    ide.ctrl &= (uint8_t)~CTRL_HOB;
}

static void ide_transfer_stop(void) {
    ide.data_pos = ide.data_end = 0;
    ide.status &= (uint8_t)~DRQ_STAT;
}

static void ide_abort_command(void) {
    ide_transfer_stop();
    ide.status = READY_STAT | ERR_STAT;
    ide.error = ABRT_ERR;
}

static uint64_t ide_get_sector(void) {
    if (ide.lba48)
        return ((uint64_t)ide.hob_hcyl << 40) | ((uint64_t)ide.hob_lcyl << 32) | ((uint64_t)ide.hob_sector << 24) |
               ((uint64_t)ide.hcyl << 16) | ((uint64_t)ide.lcyl << 8) | ide.sector;
    return ((uint64_t)(ide.select & 0x0f) << 24) | ((uint64_t)ide.hcyl << 16) | ((uint64_t)ide.lcyl << 8) | ide.sector;
}

static void ide_set_sector(uint64_t n) {
    if (ide.lba48) {
        ide.sector = (uint8_t)n; ide.lcyl = (uint8_t)(n >> 8); ide.hcyl = (uint8_t)(n >> 16);
        ide.hob_sector = (uint8_t)(n >> 24); ide.hob_lcyl = (uint8_t)(n >> 32); ide.hob_hcyl = (uint8_t)(n >> 40);
    } else {
        ide.select = (uint8_t)((ide.select & 0xf0) | ((n >> 24) & 0x0f));
        ide.hcyl = (uint8_t)(n >> 16); ide.lcyl = (uint8_t)(n >> 8); ide.sector = (uint8_t)n;
    }
}

/* the sector count of a command: 0 means 256, or 65536 with 48-bit addresses */
static void ide_lba48_transform(int lba48) {
    ide.lba48 = (uint8_t)lba48;
    if (!lba48) {
        if (!ide.nsector) ide.nsector = 256;
    } else if (!ide.nsector && !ide.hob_nsector) {
        ide.nsector = 65536;
    } else {
        ide.nsector = ((uint32_t)ide.hob_nsector << 8) | (ide.nsector & 0xff);
    }
}

/* The transfer of a DMA command. It runs when the command and the GO bit
 * of the adapter are both there. */
static void ide_dma_run(void) {
    uint32_t bytes = ide.nsector * 512, addr = ide.regs[IDE_DMA_ADDR / 4];
    uint64_t left = (uint64_t)ide.regs[IDE_DMA_LENGTH / 4] + 4, lba = ide_get_sector();
    int cmd = ide.dma_cmd;

    ide.dma_cmd = DMA_NONE;
    if (left < bytes) {
        /* the programmed length is too short: the drive stops, with no interrupt */
        fprintf(stderr, "ide: DMA length %llu is less than the command's %u bytes\n",
                (unsigned long long)left, bytes);
        ide.status = READY_STAT | SEEK_STAT;
        return;
    }
    if (!g_dev_replay) {
        uint8_t *buf = malloc(bytes);
        if (!buf) { perror("ide: alloc"); exit(1); }
        if (cmd == DMA_READ) {
            disk_read(lba, ide.nsector, buf);
            if (mach_ram_write(addr, buf, bytes))
                fprintf(stderr, "ide: DMA read to %08x: not guest RAM\n", addr);
        } else {
            if (mach_ram_read(addr, buf, bytes))
                fprintf(stderr, "ide: DMA write from %08x: not guest RAM\n", addr);
            else
                disk_write(lba, ide.nsector, buf);
        }
        free(buf);
    }
    ide_set_sector(lba + ide.nsector);
    ide.nsector = 0;
    if (g_dev_replay) {                 /* the replay has no time: complete now */
        ide.status = READY_STAT | SEEK_STAT;
        ide_set_irq();
        return;
    }
    ide.done_pending = 1;
    ide.done_at = cpu_guest_time_us() + IDE_DMA_LATENCY_US;
}

void ide_tick(uint64_t now_us) {
    if (!ide.done_pending || now_us < ide.done_at) return;
    ide.done_pending = 0;
    ide.status = READY_STAT | SEEK_STAT;
    ide_set_irq();
}

uint64_t ide_next_deadline_us(void) {
    return ide.done_pending ? ide.done_at : 0;
}

static void ide_start_dma(int cmd, int lba48) {
    ide_lba48_transform(lba48);
    ide.status = READY_STAT | SEEK_STAT | DRQ_STAT;
    ide.dma_cmd = (uint8_t)cmd;
    /* if the firmware set GO first, there is nothing more to wait for */
    if (ide.regs[IDE_DMA_CONTROL / 4] & IDE_DMA_GO) ide_dma_run();
}

/* SET FEATURES: returns 0 if the drive does not know the feature */
static int ide_set_features(void) {
    switch (ide.feature) {
    case 0x02: case 0x82:               /* write cache on / off */
    case 0xcc: case 0x66:               /* power-on defaults */
    case 0xaa: case 0x55:               /* read look-ahead */
    case 0x05: case 0x85:               /* advanced power management */
    case 0x69: case 0x67: case 0x96: case 0x9a:
    case 0x42: case 0xc2:               /* acoustic mode */
        return 1;
    case 0x03:                          /* transfer mode */
        switch ((ide.nsector & 0xff) >> 3) {
        case 0x00: case 0x01: case 0x02: case 0x04: case 0x08: return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* The size words of IDENTIFY (60-61: 28-bit, 100-103: 48-bit sectors) for
 * the disk: the data is from a real drive of 2,200,000 sectors. */
static void identify_size(uint64_t sectors) {
    if (!sectors) return;
    uint32_t s28 = sectors > 0x0fffffffu ? 0x0fffffffu : (uint32_t)sectors;
    for (int i = 0; i < 2; i++) { ide.io_buffer[120 + 2 * i] = (uint8_t)(s28 >> (16 * i)); ide.io_buffer[121 + 2 * i] = (uint8_t)(s28 >> (16 * i + 8)); }
    for (int i = 0; i < 4; i++) { ide.io_buffer[200 + 2 * i] = (uint8_t)(sectors >> (16 * i)); ide.io_buffer[201 + 2 * i] = (uint8_t)(sectors >> (16 * i + 8)); }
}

static void ide_exec_cmd(uint8_t cmd) {
    int set_dsc = 0;
    if (ide.unit) return;                               /* no second drive */
    if (ide.status & (BUSY_STAT | DRQ_STAT)) return;    /* a command is in progress */
    ide.status = READY_STAT | BUSY_STAT;
    ide.error = 0;
    switch (cmd) {
    case 0xec:                          /* IDENTIFY DEVICE: 512 bytes, PIO */
        memcpy(ide.io_buffer, identify_data, sizeof ide.io_buffer);
        identify_size(mach_disk_sectors());
        ide.status = READY_STAT | SEEK_STAT | DRQ_STAT;
        ide.data_pos = 0;
        ide.data_end = sizeof ide.io_buffer;
        ide_set_irq();
        return;
    case 0x25: ide_start_dma(DMA_READ, 1); return;      /* READ DMA EXT */
    case 0xc8: ide_start_dma(DMA_READ, 0); return;      /* READ DMA */
    case 0x35: ide_start_dma(DMA_WRITE, 1); return;     /* WRITE DMA EXT */
    case 0xca: ide_start_dma(DMA_WRITE, 0); return;     /* WRITE DMA */
    case 0xef:                          /* SET FEATURES */
        if (!ide_set_features()) ide_abort_command();
        set_dsc = 1;
        break;
    case 0xe0: case 0xe1: case 0xe2: case 0xe6:         /* standby, idle, sleep */
        break;
    case 0xe3: case 0xe7: case 0xea:    /* IDLE, FLUSH CACHE (EXT) */
        set_dsc = 1;
        break;
    case 0xe5:                          /* CHECK POWER MODE: active */
        ide.nsector = 0xff;
        set_dsc = 1;
        break;
    default:
        fprintf(stderr, "ide: command %02x is not implemented\n", cmd);
        ide_abort_command();
        break;
    }
    ide.status &= (uint8_t)~BUSY_STAT;
    if (set_dsc && !ide.error) ide.status |= SEEK_STAT;
    ide_set_irq();
}

/* the state of the drive after power-on or a software reset */
static void ide_drive_reset(void) {
    ide.status = READY_STAT | SEEK_STAT;
    ide.error = 0x01;
    ide.nsector = 1;
    ide.sector = 1;
    ide.lcyl = ide.hcyl = 0;
    ide.select = DEV_ALWAYS_ON;
    ide.unit = 0;
    ide.dma_cmd = DMA_NONE;
    ide.data_pos = ide.data_end = 0;
}

static uint32_t ide_taskfile_read(unsigned reg) {
    int hob = ide.ctrl & CTRL_HOB;
    if (reg == 0) {                                     /* data port */
        if (!(ide.status & DRQ_STAT) || ide.data_pos + 2 > ide.data_end) return 0;
        uint32_t v = ide.io_buffer[ide.data_pos] | ((uint32_t)ide.io_buffer[ide.data_pos + 1] << 8);
        ide.data_pos += 2;
        if (ide.data_pos >= ide.data_end) ide_transfer_stop();
        return v;
    }
    if (ide.unit) return 0;                             /* no second drive */
    switch (reg) {
    case 1: return hob ? ide.hob_feature : ide.error;
    case 2: return hob ? ide.hob_nsector : ide.nsector & 0xff;
    case 3: return hob ? ide.hob_sector : ide.sector;
    case 4: return hob ? ide.hob_lcyl : ide.lcyl;
    case 5: return hob ? ide.hob_hcyl : ide.hcyl;
    case 6: return ide.select;
    default:
        ide_lower_irq();
        return ide.status;
    }
}

static void ide_taskfile_write(unsigned reg, uint8_t val) {
    switch (reg) {
    case 0: break;                                      /* data port: PIO writes are not used */
    case 1: ide_clear_hob(); ide.hob_feature = ide.feature; ide.feature = val; break;
    case 2: ide_clear_hob(); ide.hob_nsector = (uint8_t)ide.nsector; ide.nsector = val; break;
    case 3: ide_clear_hob(); ide.hob_sector = ide.sector; ide.sector = val; break;
    case 4: ide_clear_hob(); ide.hob_lcyl = ide.lcyl; ide.lcyl = val; break;
    case 5: ide_clear_hob(); ide.hob_hcyl = ide.hcyl; ide.hcyl = val; break;
    case 6:
        ide_clear_hob();
        ide.select = val | DEV_ALWAYS_ON;
        ide.unit = (val & DEV_SELECT) ? 1 : 0;
        break;
    default:
        ide_clear_hob();
        ide_lower_irq();
        ide_exec_cmd(val);
        break;
    }
}

/* ---------- adapter ---------- */

void ide_reset(void) {
    memset(&ide, 0, sizeof ide);
    ide_drive_reset();
}

void ide_access(uint32_t off, int is_write, uint32_t *value, int size) {
    (void)size;
    if (off >= IDE_REGION_SIZE) { if (!is_write) *value = 0; return; }
    if (off >= IDE_TASKFILE && off < IDE_TASKFILE + IDE_TASKFILE_SIZE) {
        unsigned reg = (off - IDE_TASKFILE) >> 2;
        if (is_write) ide_taskfile_write(reg, (uint8_t)*value);
        else *value = ide_taskfile_read(reg);
        return;
    }
    if (off >= IDE_CONTROL && off < IDE_CONTROL + IDE_CONTROL_SIZE) {
        if (!is_write) {
            *value = ide.unit ? 0 : ide.status;         /* alternate status: the line stays */
        } else {
            if (!(ide.ctrl & CTRL_RESET) && (*value & CTRL_RESET)) ide_drive_reset();
            ide.ctrl = (uint8_t)*value;
        }
        return;
    }
    if (!is_write) {
        *value = ide.regs[off / 4];
        if (off == IDE0_CFG && ide.intrq) *value |= IDE_CFG_INTRQ;
        return;
    }
    if (off == IDE0_CFG) {
        if (*value & IDE_CFG_INTRQ) ide.intrq = 0;      /* a 1 acknowledges INTRQ */
        ide.regs[off / 4] = *value & ~IDE_CFG_INTRQ;
    } else if (off == IDE_DMA_CONTROL) {
        uint32_t prev = ide.regs[off / 4];
        ide.regs[off / 4] = *value;
        if ((*value & IDE_DMA_GO) && !(prev & IDE_DMA_GO) && ide.dma_cmd != DMA_NONE) ide_dma_run();
    } else {
        ide.regs[off / 4] = *value;
    }
}

int ide_snapshot(FILE *f, int save) {
    if (snap_rw(f, &ide, sizeof ide, save)) return -1;
    return ide_overlay_snapshot(f, save);
}

/* the written sectors alone: in the snapshot, and in the overlay file of
 * the virtual disk (machine.c) */
int ide_overlay_snapshot(FILE *f, int save) {
    uint32_t n = overlay_used;
    if (snap_rw(f, &n, sizeof n, save)) return -1;
    if (save) {
        for (unsigned i = 0; i < OVERLAY_SLOTS; i++)
            if (overlay[i].key && (snap_rw(f, &overlay[i].key, sizeof overlay[i].key, 1) ||
                                   snap_rw(f, overlay[i].data, 512, 1))) return -1;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t key;
        uint8_t sector[512];
        if (snap_rw(f, &key, sizeof key, 0) || snap_rw(f, sector, sizeof sector, 0) || !key) return -1;
        uint8_t *o = overlay_find(key - 1, 1);
        if (o) memcpy(o, sector, 512);
    }
    return 0;
}
