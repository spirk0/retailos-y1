/* The virtual disk: the iPod's disk built from a folder (vdisk.c). */
#ifndef VDISK_H
#define VDISK_H
#include <stdint.h>

struct vdisk_config {
    const char *root;           /* the folder of the FAT32 volume (read only, writes go to the overlay) */
    const char *firmware;       /* the firmware partition: the Firmware-* file of the IPSW */
    const char *manifest;       /* NULL, or lines "path in the volume<TAB>host file": more files */
    uint64_t size_bytes;        /* the disk size */
    uint64_t free_bytes;        /* the free space the volume shows */
};

/* Build the disk. Returns 0 on success. */
int vdisk_open(const struct vdisk_config *cfg);
void vdisk_close(void);

/* 1 after vdisk_open() succeeded */
int vdisk_active(void);

/* The size in 512-byte sectors */
uint64_t vdisk_sectors(void);

/* Read n sectors from lba. Never fails: a sector without content is zero. */
void vdisk_read(uint64_t lba, uint32_t n, uint8_t *buf);

/* A hash of the layout (names, sizes, dates, size): written sectors (the
 * overlay) fit only the same layout */
uint32_t vdisk_layout_id(void);

#endif
