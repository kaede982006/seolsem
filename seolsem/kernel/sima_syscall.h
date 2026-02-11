#ifndef __SIMA_SYSCALL__
#define __SIMA_SYSCALL__

#include "sima_type.h"

#define SIMA_SYSCALL_PRINT 1
#define SIMA_SYSCALL_CLS   2

typedef struct {
    const char *text;
} SYSCALL_PRINT_IN;

BOOL syscall_dispatch(UINT16 call_no, const void *in, UINT16 *out_status);
void syscall_print(const char *text);
void syscall_cls(void);

#endif
