/*
 * sys/stat.h - File status stub for vim on seolsem
 */

#ifndef _SYS_STAT_H_SEOLSEM
#define _SYS_STAT_H_SEOLSEM

#include "types.h"

/* File type bits */
#define S_IFMT   0170000  /* Mask for file type */
#define S_IFREG  0100000  /* Regular file */
#define S_IFDIR  0040000  /* Directory */
#define S_IFCHR  0020000  /* Character device */
#define S_IFBLK  0060000  /* Block device */
#define S_IFIFO  0010000  /* FIFO */

/* File mode bits */
#define S_IRUSR  0400     /* Owner read */
#define S_IWUSR  0200     /* Owner write */
#define S_IXUSR  0100     /* Owner execute */
#define S_IRGRP  0040     /* Group read */
#define S_IWGRP  0020     /* Group write */
#define S_IXGRP  0010     /* Group execute */
#define S_IROTH  0004     /* Others read */
#define S_IWOTH  0002     /* Others write */
#define S_IXOTH  0001     /* Others execute */

/* DOS/Watcom compatibility */
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR

/* Test macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

/* stat structure */
struct stat {
    dev_t   st_dev;
    ino_t   st_ino;
    mode_t  st_mode;
    nlink_t st_nlink;
    uid_t   st_uid;
    gid_t   st_gid;
    dev_t   st_rdev;
    off_t   st_size;
    time_t  st_atime;
    time_t  st_mtime;
    time_t  st_ctime;
};

/* stat function - stubbed */
static int stat(const char *path, struct stat *buf) {
    (void)path;
    if (buf) {
        buf->st_dev = 0;
        buf->st_ino = 0;
        buf->st_mode = S_IFREG | 0666;
        buf->st_nlink = 1;
        buf->st_uid = 0;
        buf->st_gid = 0;
        buf->st_rdev = 0;
        buf->st_size = 0;
        buf->st_atime = 0;
        buf->st_mtime = 0;
        buf->st_ctime = 0;
    }
    return 0;
}

static int fstat(int fd, struct stat *buf) {
    (void)fd;
    return stat("", buf);
}

static int lstat(const char *path, struct stat *buf) {
    return stat(path, buf);
}

#endif /* _SYS_STAT_H_SEOLSEM */
