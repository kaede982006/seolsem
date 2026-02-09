/*
 * os_seolsem.h - Platform header for vim on seolsem
 * Defines MSDOS-compatible macros and seolsem-specific settings
 */

#ifndef OS_SEOLSEM_H
#define OS_SEOLSEM_H

#include "os_dos.h"     /* Common MS-DOS and Win32 stuff */

/* Seolsem-specific overrides */
#define SEOLSEM         1
#define BINARY_FILE_IO  1
#define USE_EXE_NAME    1
#define USE_TERM_CONSOLE 1
#define SHORT_FNAME     1       /* 8.3 filenames */

/* Available headers */
#define HAVE_STDLIB_H   1
#define HAVE_STRING_H   1
#define HAVE_STRCSPN    1
#define HAVE_STRICMP    1
#define HAVE_STRNICMP   1
#define HAVE_MEMSET     1
#define HAVE_QSORT      1

/* Memory limits for 16-bit */
#ifndef DFLT_MAXMEM
# define DFLT_MAXMEM    64      /* 64KB for buffer */
#endif
#ifndef DFLT_MAXMEMTOT
# define DFLT_MAXMEMTOT 128     /* 128KB total */
#endif

#define BASENAMELEN     8       /* 8.3 filename base */

#define FNAME_ILLEGAL "\"*?><|" /* Illegal filename chars */

/* Temp file handling */
#define TEMPDIRNAMES    "", ""
#define TEMPNAMELEN     64

/* Screen dimensions */
#define DFLT_COLS       80
#define DFLT_ROWS       25

/* Breakcheck is cheap on seolsem */
#define BREAKCHECK_SKIP 1

/* Use our custom memory functions */
#define mch_memmove(to, from, len)  memmove((char *)(to), (char *)(from), len)

/* Rename/mkdir macros */
#define mch_rename(src, dst)    rename(src, dst)
#define vim_mkdir(x, y)         (-1)  /* Not supported */
#define mch_rmdir(x)            (-1)  /* Not supported */
#define mch_setenv(n, v, x)     setenv(n, v, x)

#endif /* OS_SEOLSEM_H */
