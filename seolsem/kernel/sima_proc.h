#ifndef __SIMA_PROC__
#define __SIMA_PROC__

#include "sima_type.h"

/* RTOS profile: single active execution context, no true multi-process scheduler. */
BOOL proc_spawn(const char *path, const char *arg, UINT16 *out_pid);
BOOL proc_exec(const char *path, const char *arg, UINT16 *out_status);
BOOL proc_wait(UINT16 pid, UINT16 *out_status);
UINT16 proc_get_last_status(void);

#endif
