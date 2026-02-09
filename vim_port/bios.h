/*
 * bios.h - BIOS header stub for vim on seolsem
 */

#ifndef _BIOS_H_SEOLSEM
#define _BIOS_H_SEOLSEM

/* BIOS keyboard services */
#define _KEYBRD_READ        0x00
#define _KEYBRD_READY       0x01
#define _KEYBRD_SHIFTSTATUS 0x02

#define _NKEYBRD_READ       0x10
#define _NKEYBRD_READY      0x11
#define _NKEYBRD_SHIFTSTATUS 0x12

/* BIOS disk services */
#define _DISK_RESET     0
#define _DISK_STATUS    1
#define _DISK_READ      2
#define _DISK_WRITE     3
#define _DISK_VERIFY    4
#define _DISK_FORMAT    5

/* BIOS time services */
#define _TIME_GETCLOCK  0
#define _TIME_SETCLOCK  1

/* Shift key flags */
#define RIGHTSHIFT  0x01
#define LEFTSHIFT   0x02
#define CTRL        0x04
#define ALT         0x08
#define SCROLLLOCK  0x10
#define NUMLOCK     0x20
#define CAPSLOCK    0x40
#define INSERT      0x80

/* bioskey() function - stubbed */
static int _bios_keybrd(int cmd) {
    (void)cmd;
    return 0;
}

/* Alias */
#define bioskey(cmd)    _bios_keybrd(cmd)

/* biostime() function - stubbed */
static long _bios_timeofday(int cmd, long *newtime) {
    (void)cmd;
    (void)newtime;
    return 0;
}

#define biostime(cmd, newtime)  _bios_timeofday(cmd, (long*)&(newtime))

/* biosequip() function - stubbed */
static int biosequip(void) {
    return 0;
}

/* biosmemory() function - stubbed */
static int biosmemory(void) {
    return 640;  /* Report 640KB of conventional memory */
}

#endif /* _BIOS_H_SEOLSEM */
