/*
 * dos.h - Minimal DOS header stub for vim on seolsem  
 * Provides just enough to satisfy vim's includes
 */

#ifndef _DOS_H_SEOLSEM
#define _DOS_H_SEOLSEM

/* Basic DOS types */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;

/* Union REGS - for int86() calls (stubbed) */
struct WORDREGS {
    unsigned short ax, bx, cx, dx;
    unsigned short si, di;
    unsigned short cflag;
};

struct BYTEREGS {
    unsigned char al, ah, bl, bh;
    unsigned char cl, ch, dl, dh;
};

union REGS {
    struct WORDREGS x;
    struct BYTEREGS h;
};

struct SREGS {
    unsigned short es, cs, ss, ds;
};

/* int86 stubs - not functional on seolsem PM */
static int int86(int intno, union REGS *inregs, union REGS *outregs) {
    (void)intno;
    (void)inregs;
    if (outregs) outregs->x.cflag = 1; /* Indicate error */
    return -1;
}

static int int86x(int intno, union REGS *inregs, union REGS *outregs, struct SREGS *segregs) {
    (void)intno;
    (void)inregs;
    (void)segregs;
    if (outregs) outregs->x.cflag = 1;
    return -1;
}

/* BIOS key codes for bioskey() */
#define _NKEYBRD_READ   0x00
#define _NKEYBRD_READY  0x01
#define _KEYBRD_SHIFTSTATUS 0x02

/* bioskey stub */
static int bioskey(int cmd) {
    (void)cmd;
    return 0;
}

/* biostime stub */
static long biostime(int cmd, long newtime) {
    (void)cmd;
    (void)newtime;
    return 0;
}

/* File attributes */
#define _A_NORMAL   0x00
#define _A_RDONLY   0x01
#define _A_HIDDEN   0x02
#define _A_SYSTEM   0x04
#define _A_VOLID    0x08
#define _A_SUBDIR   0x10
#define _A_ARCH     0x20

/* File find structures */
struct find_t {
    char reserved[21];
    char attrib;
    unsigned short wr_time;
    unsigned short wr_date;
    unsigned long size;
    char name[13];
};

#define _dos_findfirst(path, attr, buf)  (-1)
#define _dos_findnext(buf)               (-1)
#define _dos_findclose(buf)              (0)

/* Memory functions (stubbed) */
#define _dos_allocmem(size, seg)   (-1)
#define _dos_freemem(seg)          (-1)

/* Interrupt control */
#define _disable()  /* no-op */
#define _enable()   /* no-op */

#endif /* _DOS_H_SEOLSEM */
