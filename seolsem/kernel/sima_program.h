#ifndef __SIMA_PROGRAM__
#define __SIMA_PROGRAM__

#include "sima_type.h"

typedef UINT16 PROGRAM_HANDLE;
#define PROGRAM_HANDLE_INVALID 0U

typedef enum {
    PROGRAM_OK = 0,
    PROGRAM_ERR_INVALID_ARG = 1,
    PROGRAM_ERR_PERMISSION = 2,
    PROGRAM_ERR_NOT_FOUND = 3,
    PROGRAM_ERR_NOT_EXECUTABLE = 4,
    PROGRAM_ERR_FILE_TOO_LARGE = 5,
    PROGRAM_ERR_NO_MEMORY = 6,
    PROGRAM_ERR_IO = 7,
    PROGRAM_ERR_BAD_FORMAT = 8,
    PROGRAM_ERR_IMPORT = 9,
    PROGRAM_ERR_RELOC = 10,
    PROGRAM_ERR_EXEC = 11,
    PROGRAM_ERR_BAD_HANDLE = 12
} PROGRAM_RESULT;

typedef struct {
    UINT16 image_capacity;
    UINT16 stack_reserve;
} PROGRAM_LOADER_INFO;

PROGRAM_RESULT program_loader_open(const char *path, PROGRAM_HANDLE *out_handle);
PROGRAM_RESULT program_loader_open_in_dir(const char *dir_name, const char *name, PROGRAM_HANDLE *out_handle);
PROGRAM_RESULT program_loader_exec(PROGRAM_HANDLE handle, const char *arg, UINT16 *out_exit_code);
PROGRAM_RESULT program_loader_close(PROGRAM_HANDLE handle);
PROGRAM_RESULT program_loader_result_message(PROGRAM_RESULT rc, char *out, UINT16 out_cap);
PROGRAM_RESULT program_loader_get_info(PROGRAM_LOADER_INFO *out);

#endif
