#include "sima_ide.h"

#define IDE_DATA       0x1F0
#define IDE_ERROR      0x1F1
#define IDE_SECCOUNT   0x1F2
#define IDE_LBA0       0x1F3
#define IDE_LBA1       0x1F4
#define IDE_LBA2       0x1F5
#define IDE_HDDEVSEL   0x1F6
#define IDE_COMMAND    0x1F7
#define IDE_STATUS     0x1F7

#define IDE_STATUS_BSY 0x80
#define IDE_STATUS_DRQ 0x08
#define IDE_STATUS_ERR 0x01

extern UINT8 io_inb(UINT16 port);
extern void  io_outb(UINT16 port, UINT8 value);
extern void  io_insw(UINT16 port, void *buffer, UINT16 count);
extern void  io_outsw(UINT16 port, const void *buffer, UINT16 count);

static BOOL ide_wait(UINT8 mask, UINT8 value, UINT16 timeout) {
    UINT16 i;
    for (i = 0; i < timeout; ++i) {
        UINT8 status = io_inb(IDE_STATUS);
        if ((status & mask) == value) return TRUE;
    }
    return FALSE;
}

static BOOL ide_wait_ready(void) {
    return ide_wait(IDE_STATUS_BSY | IDE_STATUS_DRQ, 0, 0xFFFF);
}

static BOOL ide_wait_drq(void) {
    return ide_wait(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 0xFFFF);
}

static void ide_select_drive(UINT32 lba) {
    UINT8 drive = (UINT8)(0xE0 | ((lba >> 24) & 0x0F));
    io_outb(IDE_HDDEVSEL, drive);
}

BOOL ide_read_sector(UINT32 lba, UINT8 *buffer) {
    if (!buffer) return FALSE;
    if (!ide_wait_ready()) return FALSE;

    ide_select_drive(lba);
    io_outb(IDE_SECCOUNT, 1);
    io_outb(IDE_LBA0, (UINT8)(lba & 0xFF));
    io_outb(IDE_LBA1, (UINT8)((lba >> 8) & 0xFF));
    io_outb(IDE_LBA2, (UINT8)((lba >> 16) & 0xFF));
    io_outb(IDE_COMMAND, 0x20);

    if (!ide_wait_drq()) return FALSE;
    io_insw(IDE_DATA, buffer, 256);

    if (io_inb(IDE_STATUS) & IDE_STATUS_ERR) return FALSE;
    return TRUE;
}

BOOL ide_write_sector(UINT32 lba, const UINT8 *buffer) {
    if (!buffer) return FALSE;
    if (!ide_wait_ready()) return FALSE;

    ide_select_drive(lba);
    io_outb(IDE_SECCOUNT, 1);
    io_outb(IDE_LBA0, (UINT8)(lba & 0xFF));
    io_outb(IDE_LBA1, (UINT8)((lba >> 8) & 0xFF));
    io_outb(IDE_LBA2, (UINT8)((lba >> 16) & 0xFF));
    io_outb(IDE_COMMAND, 0x30);

    if (!ide_wait_drq()) return FALSE;
    io_outsw(IDE_DATA, buffer, 256);

    if (io_inb(IDE_STATUS) & IDE_STATUS_ERR) return FALSE;
    return TRUE;
}

BOOL ide_identify_total_sectors(UINT32 *out_total_sectors) {
    static UINT16 identify_words[256];
    UINT8 status;
    UINT16 lo;
    UINT16 hi;

    if (!out_total_sectors) return FALSE;
    if (!ide_wait_ready()) return FALSE;

    /* Select master drive. */
    io_outb(IDE_HDDEVSEL, 0xA0);

    /* Per ATA spec, clear these registers before IDENTIFY. */
    io_outb(IDE_SECCOUNT, 0);
    io_outb(IDE_LBA0, 0);
    io_outb(IDE_LBA1, 0);
    io_outb(IDE_LBA2, 0);
    io_outb(IDE_COMMAND, 0xEC); /* IDENTIFY */

    status = io_inb(IDE_STATUS);
    if (status == 0) return FALSE;

    if (!ide_wait(IDE_STATUS_BSY, 0, 0xFFFF)) return FALSE;
    if (io_inb(IDE_STATUS) & IDE_STATUS_ERR) return FALSE;
    if (!ide_wait_drq()) return FALSE;

    io_insw(IDE_DATA, identify_words, 256);

    lo = identify_words[60];
    hi = identify_words[61];
    *out_total_sectors = (UINT32)lo | ((UINT32)hi << 16);
    return TRUE;
}
