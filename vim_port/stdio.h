/*
 * stdio.h - Standard I/O header for vim on seolsem
 */

#ifndef _STDIO_H_SEOLSEM
#define _STDIO_H_SEOLSEM

#include "sys/types.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1  
#define SEEK_END 2

#define BUFSIZ 512
#define FILENAME_MAX 64
#define FOPEN_MAX 8

/* FILE type */
typedef struct {
    unsigned short handle;
    unsigned short flags;
    unsigned long pos;
    unsigned long size;
    char name[64];
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File operations */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fflush(FILE *stream);

/* Character I/O */
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);
int ungetc(int c, FILE *stream);

/* Macros */
#define getc(stream) fgetc(stream)
#define putc(c, stream) fputc(c, stream)
#define getchar() fgetc(stdin)
#define putchar(c) fputc(c, stdout)

/* Error handling */
int ferror(FILE *stream);
int feof(FILE *stream);
void clearerr(FILE *stream);
void perror(const char *s);

/* Formatted I/O */
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);

int scanf(const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *str, const char *format, ...);

/* vsprintf for vim */
#include <stdarg.h>
int vsprintf(char *str, const char *format, va_list ap);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int vfprintf(FILE *stream, const char *format, va_list ap);

/* File management */
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
FILE *tmpfile(void);
char *tmpnam(char *s);

#endif /* _STDIO_H_SEOLSEM */
