/*
 * io.h - Low-level I/O header for vim on seolsem
 */

#ifndef _IO_H_SEOLSEM
#define _IO_H_SEOLSEM

#include "fcntl.h"
#include "sys/types.h"

/* File access modes */
#define F_OK    0       /* Test for existence */
#define X_OK    1       /* Test for execute permission */
#define W_OK    2       /* Test for write permission */
#define R_OK    4       /* Test for read permission */

/* File creation mode bits */
#define S_IREAD     0400    /* Read by owner */
#define S_IWRITE    0200    /* Write by owner */
#define S_IEXEC     0100    /* Execute by owner */

/* POSIX mode names */
#define S_IRUSR     S_IREAD
#define S_IWUSR     S_IWRITE
#define S_IXUSR     S_IEXEC

/* low-level I/O functions */
int open(const char *pathname, int flags, ...);
int close(int fd);
int read(int fd, void *buf, unsigned int count);
int write(int fd, const void *buf, unsigned int count);
long lseek(int fd, long offset, int whence);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int access(const char *pathname, int mode);
int unlink(const char *pathname);
int isatty(int fd);
int chmod(const char *pathname, int mode);

/* Stub implementations */
#define access(path, mode)  (0)
#define isatty(fd)          (1)
#define chmod(path, mode)   (0)
#define unlink(path)        remove(path)

#endif /* _IO_H_SEOLSEM */
