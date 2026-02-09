/*
 * seolsem_libc.c - Minimal C library implementation for vim port
 * Implements standard C functions using seolsem kernel APIs
 */

#include "seolsem_libc.h"

/* Import seolsem kernel APIs */
extern void *sima_heap_alloc(UINT16 size);
extern void sima_heap_free(void *ptr);
extern void sima_memmove(void *dest, const void *src, UINT16 size);
extern void sima_memclr(void *dest, UINT16 size);
extern void sima_memset(void *dest, UINT8 val, UINT16 size);
extern UINT16 sima_strlen(const char *s);
extern void sima_strcpy(char *dest, UINT16 max_len, const char *src);
extern void sima_strcat(char *dest, UINT16 max_len, const char *src);
extern INT16 sima_strcmp(const char *s1, const char *s2);
extern INT16 sima_strncmp(const char *s1, const char *s2, UINT16 n);

extern UINT16 read_key(void);
extern void write_char(UINT16 row, UINT16 col, char c, UINT8 attr);
extern void print_message(const char *msg);

extern int fs_read(const char *path, UINT8 *buf, UINT32 max_size, UINT32 *read_size);
extern int fs_write(const char *path, const UINT8 *buf, UINT32 size);
extern int fs_delete(const char *path);
extern int fs_exists(const char *path);
extern UINT32 fs_get_size(const char *path);

/* ============================================================
 * Memory functions
 * ============================================================ */

void *malloc(size_t size) {
    if (size == 0) return NULL;
    if (size > 0xFFFF) return NULL; /* 16-bit limit */
    return sima_heap_alloc((UINT16)size);
}

void free(void *ptr) {
    if (ptr != NULL) {
        sima_heap_free(ptr);
    }
}

void *realloc(void *ptr, size_t size) {
    void *new_ptr;
    if (ptr == NULL) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    new_ptr = malloc(size);
    if (new_ptr != NULL) {
        /* Copy old data - we don't know old size, so copy size bytes */
        memmove(new_ptr, ptr, size);
        free(ptr);
    }
    return new_ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *memcpy(void *dest, const void *src, size_t n) {
    sima_memmove(dest, src, (UINT16)n);
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    sima_memmove(dest, src, (UINT16)n);
    return dest;
}

void *memset(void *s, int c, size_t n) {
    sima_memset(s, (UINT8)c, (UINT16)n);
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n-- > 0) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

/* ============================================================
 * String functions
 * ============================================================ */

size_t strlen(const char *s) {
    return sima_strlen(s);
}

char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++) != '\0');
    return ret;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *ret = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++) != '\0');
    return ret;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *ret = dest;
    while (*dest) dest++;
    while (n-- > 0 && *src) {
        *dest++ = *src++;
    }
    *dest = '\0';
    return ret;
}

int strcmp(const char *s1, const char *s2) {
    return sima_strcmp(s1, s2);
}

int strncmp(const char *s1, const char *s2, size_t n) {
    return sima_strncmp(s1, s2, (UINT16)n);
}

int stricmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strnicmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t needle_len;
    if (*needle == '\0') return (char *)haystack;
    needle_len = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        s++;
        count++;
    }
    return count;
}

/* ============================================================
 * Character functions
 * ============================================================ */

int isdigit(int c) { return (c >= '0' && c <= '9'); }
int isalpha(int c) { return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')); }
int isalnum(int c) { return (isalpha(c) || isdigit(c)); }
int isspace(int c) { return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'); }
int isupper(int c) { return (c >= 'A' && c <= 'Z'); }
int islower(int c) { return (c >= 'a' && c <= 'z'); }
int isprint(int c) { return (c >= 32 && c < 127); }
int toupper(int c) { return (islower(c) ? c - 32 : c); }
int tolower(int c) { return (isupper(c) ? c + 32 : c); }

/* ============================================================
 * FILE I/O (minimal implementation)
 * ============================================================ */

#define MAX_FILES 8
static FILE file_table[MAX_FILES];
static int file_table_init = 0;

static FILE stdin_file = {0, 0, 0, 0, "stdin"};
static FILE stdout_file = {1, 0, 0, 0, "stdout"};
static FILE stderr_file = {2, 0, 0, 0, "stderr"};

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static void init_file_table(void) {
    int i;
    if (file_table_init) return;
    for (i = 0; i < MAX_FILES; i++) {
        file_table[i].handle = 0xFFFF;
    }
    file_table_init = 1;
}

static FILE *alloc_file(void) {
    int i;
    init_file_table();
    for (i = 0; i < MAX_FILES; i++) {
        if (file_table[i].handle == 0xFFFF) {
            file_table[i].handle = (UINT16)i;
            return &file_table[i];
        }
    }
    return NULL;
}

FILE *fopen(const char *path, const char *mode) {
    FILE *f = alloc_file();
    if (f == NULL) return NULL;
    
    strncpy(f->name, path, sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    f->pos = 0;
    f->flags = 0;
    
    if (mode[0] == 'r') {
        if (!fs_exists(path)) {
            f->handle = 0xFFFF;
            return NULL;
        }
        f->size = fs_get_size(path);
    } else if (mode[0] == 'w') {
        f->size = 0;
        f->flags = 1; /* write mode */
    } else if (mode[0] == 'a') {
        f->size = fs_exists(path) ? fs_get_size(path) : 0;
        f->pos = f->size;
        f->flags = 2; /* append mode */
    }
    
    return f;
}

int fclose(FILE *stream) {
    if (stream == NULL) return EOF;
    if (stream == stdin || stream == stdout || stream == stderr) return 0;
    stream->handle = 0xFFFF;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    UINT32 total = (UINT32)size * nmemb;
    UINT32 read_size = 0;
    UINT8 *buf;
    
    if (stream == NULL || total == 0) return 0;
    
    /* Read entire file into temp buffer, then copy requested portion */
    buf = (UINT8 *)malloc((size_t)stream->size);
    if (buf == NULL) return 0;
    
    if (fs_read(stream->name, buf, stream->size, &read_size)) {
        if (stream->pos < read_size) {
            UINT32 avail = read_size - stream->pos;
            if (total > avail) total = avail;
            memmove(ptr, buf + stream->pos, (size_t)total);
            stream->pos += total;
            free(buf);
            return total / size;
        }
    }
    
    free(buf);
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    /* Simplified: only support full file write */
    UINT32 total = (UINT32)size * nmemb;
    if (stream == NULL || total == 0) return 0;
    
    if (stream == stdout || stream == stderr) {
        /* Output to console */
        const char *s = (const char *)ptr;
        size_t i;
        for (i = 0; i < total && s[i]; i++) {
            /* Use print_message for now */
        }
        return nmemb;
    }
    
    /* For file write - accumulate and write on close */
    /* Simplified: direct write */
    if (fs_write(stream->name, (const UINT8 *)ptr, total)) {
        stream->size = total;
        stream->pos = total;
        return nmemb;
    }
    
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (stream == NULL) return -1;
    switch (whence) {
        case SEEK_SET: stream->pos = (UINT32)offset; break;
        case SEEK_CUR: stream->pos += offset; break;
        case SEEK_END: stream->pos = stream->size + offset; break;
        default: return -1;
    }
    return 0;
}

long ftell(FILE *stream) {
    if (stream == NULL) return -1;
    return (long)stream->pos;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int fgetc(FILE *stream) {
    char c;
    if (fread(&c, 1, 1, stream) == 1) return (unsigned char)c;
    return EOF;
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    if (fwrite(&ch, 1, 1, stream) == 1) return (unsigned char)c;
    return EOF;
}

char *fgets(char *s, int size, FILE *stream) {
    int i = 0;
    int c;
    while (i < size - 1) {
        c = fgetc(stream);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) == len) return 1;
    return EOF;
}

int ungetc(int c, FILE *stream) {
    if (stream == NULL || c == EOF) return EOF;
    if (stream->pos > 0) stream->pos--;
    return c;
}

int ferror(FILE *stream) {
    (void)stream;
    return 0;
}

int feof(FILE *stream) {
    if (stream == NULL) return 1;
    return (stream->pos >= stream->size);
}

void clearerr(FILE *stream) {
    (void)stream;
}

/* ============================================================
 * Printf (minimal - for debugging)
 * ============================================================ */

static char printf_buf[256];

int printf(const char *format, ...) {
    /* Minimal: just print the format string */
    print_message(format);
    return (int)strlen(format);
}

int sprintf(char *str, const char *format, ...) {
    /* Minimal: just copy format */
    strcpy(str, format);
    return (int)strlen(format);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    strncpy(str, format, size - 1);
    str[size - 1] = '\0';
    return (int)strlen(str);
}

int fprintf(FILE *stream, const char *format, ...) {
    return fputs(format, stream);
}

/* ============================================================
 * Low-level I/O stubs
 * ============================================================ */

int open(const char *pathname, int flags) {
    (void)pathname;
    (void)flags;
    return -1;
}

int close(int fd) {
    (void)fd;
    return 0;
}

int read(int fd, void *buf, size_t count) {
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

int write(int fd, const void *buf, size_t count) {
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

off_t lseek(int fd, off_t offset, int whence) {
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

int dup(int oldfd) {
    (void)oldfd;
    return -1;
}

int remove(const char *pathname) {
    return fs_delete(pathname) ? 0 : -1;
}

int rename(const char *oldpath, const char *newpath) {
    /* Not directly supported - would need read/write/delete */
    (void)oldpath;
    (void)newpath;
    return -1;
}

int stat(const char *path, void *buf) {
    (void)path;
    (void)buf;
    return -1;
}

/* ============================================================
 * Conversion functions
 * ============================================================ */

int atoi(const char *nptr) {
    int result = 0;
    int sign = 1;
    while (isspace(*nptr)) nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    while (isdigit(*nptr)) {
        result = result * 10 + (*nptr - '0');
        nptr++;
    }
    return sign * result;
}

long atol(const char *nptr) {
    return (long)atoi(nptr);
}

long strtol(const char *nptr, char **endptr, int base) {
    long result = 0;
    int sign = 1;
    
    while (isspace(*nptr)) nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    
    if (base == 0 || base == 16) {
        if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
            base = 16;
            nptr += 2;
        } else if (base == 0) {
            base = (nptr[0] == '0') ? 8 : 10;
        }
    }
    
    while (*nptr) {
        int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = *nptr - '0';
        else if (*nptr >= 'a' && *nptr <= 'f') digit = *nptr - 'a' + 10;
        else if (*nptr >= 'A' && *nptr <= 'F') digit = *nptr - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        nptr++;
    }
    
    if (endptr) *endptr = (char *)nptr;
    return sign * result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

/* ============================================================
 * Miscellaneous
 * ============================================================ */

time_t time(time_t *tloc) {
    /* Return 0 - no RTC support yet */
    time_t t = 0;
    if (tloc) *tloc = t;
    return t;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    /* Simple bubble sort - inefficient but works */
    size_t i, j;
    char *arr = (char *)base;
    char *temp = (char *)malloc(size);
    
    if (temp == NULL) return;
    
    for (i = 0; i < nmemb - 1; i++) {
        for (j = 0; j < nmemb - i - 1; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
                memcpy(temp, arr + j * size, size);
                memcpy(arr + j * size, arr + (j + 1) * size, size);
                memcpy(arr + (j + 1) * size, temp, size);
            }
        }
    }
    
    free(temp);
}

void exit(int status) {
    /* Return to shell - handled by mch_exit in os_seolsem.c */
    (void)status;
    for (;;); /* Halt if called directly */
}

char *getenv(const char *name) {
    /* Stub - could connect to sima_env */
    (void)name;
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    (void)name;
    (void)value;
    (void)overwrite;
    return -1;
}
