/*
 * fcntl.h - File control header for vim on seolsem
 */

#ifndef _FCNTL_H_SEOLSEM
#define _FCNTL_H_SEOLSEM

/* Open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_APPEND    0x0008
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_EXCL      0x0400
#define O_TEXT      0x4000
#define O_BINARY    0x8000

/* Special values */
#define O_NOINHERIT 0x0080

/* fcntl commands */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4

/* Low-level I/O (stubs) */
int open(const char *pathname, int flags, ...);
int close(int fd);
int read(int fd, void *buf, unsigned int count);
int write(int fd, const void *buf, unsigned int count);
long lseek(int fd, long offset, int whence);
int dup(int oldfd);
int dup2(int oldfd, int newfd);

/* fcntl (stub) */
static int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    return 0;
}

#endif /* _FCNTL_H_SEOLSEM */
