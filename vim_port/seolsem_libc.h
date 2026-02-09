/*
 * seolsem_libc.h - Minimal C library shim for vim port to seolsem
 * Maps standard C library functions to seolsem kernel APIs
 */

#ifndef SEOLSEM_LIBC_H
#define SEOLSEM_LIBC_H

/* Seolsem type definitions */
typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned long  UINT32;
typedef signed char    INT8;
typedef signed short   INT16;
typedef signed long    INT32;

/* Standard C types */
typedef UINT16 size_t;
typedef INT32 off_t;
typedef INT32 time_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

/* ============================================================
 * Memory functions
 * ============================================================ */

/* Forward declarations - implemented in seolsem_libc.c */
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);

/* Memory operations - map to sima_* */
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

/* ============================================================
 * String functions
 * ============================================================ */

size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int stricmp(const char *s1, const char *s2);
int strnicmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *s, const char *reject);

/* ============================================================
 * Character functions
 * ============================================================ */

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

/* ============================================================
 * I/O functions (stubbed or minimal)
 * ============================================================ */

/* FILE type - simplified for seolsem */
typedef struct {
    UINT16 handle;
    UINT16 flags;
    UINT32 pos;
    UINT32 size;
    char   name[64];
} FILE;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Standard streams - stubs */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);
int ungetc(int c, FILE *stream);
int ferror(FILE *stream);
int feof(FILE *stream);
void clearerr(FILE *stream);

/* printf family - minimal implementation */
int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);

/* Low-level I/O */
int open(const char *pathname, int flags);
int close(int fd);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int dup(int oldfd);

/* File operations */
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int stat(const char *path, void *buf);

/* ============================================================
 * Conversion functions
 * ============================================================ */

int atoi(const char *nptr);
long atol(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* ============================================================
 * Miscellaneous
 * ============================================================ */

time_t time(time_t *tloc);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void exit(int status);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);

/* ============================================================
 * Assertions and errors
 * ============================================================ */

#define assert(expr) ((void)0)
#define errno 0

#endif /* SEOLSEM_LIBC_H */
