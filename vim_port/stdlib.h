/*
 * stdlib.h - Standard library stub for vim on seolsem
 */

#ifndef _STDLIB_H_SEOLSEM
#define _STDLIB_H_SEOLSEM

#include "sys/types.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Memory functions */
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);

/* Conversion functions */
int atoi(const char *nptr);
long atol(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* Random number (stubbed) */
static int rand(void) { return 0; }
static void srand(unsigned int seed) { (void)seed; }
#define RAND_MAX 32767

/* Environment */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);

/* Sorting and searching */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *));

/* Exit functions */
void exit(int status);
static void abort(void) { for(;;); }

/* Misc */
static int abs(int j) { return (j < 0) ? -j : j; }
static long labs(long j) { return (j < 0) ? -j : j; }

/* Path splitting */
void _makepath(char *path, const char *drive, const char *dir,
               const char *fname, const char *ext);
void _splitpath(const char *path, char *drive, char *dir,
                char *fname, char *ext);

#endif /* _STDLIB_H_SEOLSEM */
