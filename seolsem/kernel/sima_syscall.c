#include "sima_syscall.h"
#include "sima_io.h"

BOOL syscall_dispatch(UINT16 call_no, const void *in, UINT16 *out_status) {
    if (out_status) *out_status = 0;

    switch (call_no) {
        case SIMA_SYSCALL_PRINT:
            if (!in) return FALSE;
            syscall_print(((const SYSCALL_PRINT_IN*)in)->text);
            return TRUE;

        case SIMA_SYSCALL_CLS:
            syscall_cls();
            return TRUE;

        default:
            return FALSE;
    }
}

void syscall_print(const char *text) {
    if (!text) return;
    print_message(text);
}

void syscall_cls(void) {
    clear_screen();
}
