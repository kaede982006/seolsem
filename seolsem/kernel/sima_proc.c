#include "sima_proc.h"
#include "sima_program.h"

#define PROC_PID_SINGLE 1

static UINT16 g_last_status = 0;
static BOOL g_wait_pending = FALSE;
static UINT16 g_wait_status = 0;

static BOOL proc_exec_image(const char *path, const char *arg, UINT16 *out_status) {
    PROGRAM_HANDLE handle = PROGRAM_HANDLE_INVALID;
    PROGRAM_RESULT rc;
    UINT16 status = 1;

    if (!path || path[0] == '\0') return FALSE;
    rc = program_loader_open(path, &handle);
    if (rc != PROGRAM_OK) return FALSE;
    rc = program_loader_exec(handle, arg, &status);
    (void)program_loader_close(handle);
    if (rc != PROGRAM_OK) return FALSE;

    g_last_status = status;
    if (out_status) *out_status = status;
    return TRUE;
}

BOOL proc_spawn(const char *path, const char *arg, UINT16 *out_pid) {
    UINT16 pid;
    UINT16 status;

    /* RTOS profile: only one completion slot exists (no concurrent children). */
    if (g_wait_pending) return FALSE;

    pid = PROC_PID_SINGLE;
    if (!proc_exec_image(path, arg, &status)) return FALSE;

    g_wait_status = status;
    g_wait_pending = TRUE;
    if (out_pid) *out_pid = pid;
    return TRUE;
}

BOOL proc_exec(const char *path, const char *arg, UINT16 *out_status) {
    g_wait_pending = FALSE;
    return proc_exec_image(path, arg, out_status);
}

BOOL proc_wait(UINT16 pid, UINT16 *out_status) {
    if (pid != PROC_PID_SINGLE || !g_wait_pending) return FALSE;

    if (out_status) *out_status = g_wait_status;
    g_last_status = g_wait_status;
    g_wait_pending = FALSE;
    return TRUE;
}

UINT16 proc_get_last_status(void) {
    return g_last_status;
}
