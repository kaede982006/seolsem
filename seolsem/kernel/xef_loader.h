#ifndef __XEF_LOADER__
#define __XEF_LOADER__

#include "sima_type.h"

#define XEFN_VERSION      0x0001
#define XEFN_HEADER_SIZE  28

#define XEFN_FLAG_NONE    0x0000

#define XEFN_RELOC_BASE16   0x01
#define XEFN_RELOC_IMPORT16 0x02

typedef struct {
    UINT8 *image;
    UINT16 image_size;
    UINT16 image_capacity;
    const char *arg;
    UINT16 stack_top;
} XEF_EXEC_CONTEXT;

typedef enum {
    XEF_OK = 0,
    XEF_ERR_BAD_HEADER = 1,
    XEF_ERR_IMAGE_TOO_LARGE = 2,
    XEF_ERR_NO_MEMORY = 3,
    XEF_ERR_IMPORT = 4,
    XEF_ERR_RELOC = 5,
    XEF_ERR_EXEC = 6
} XEF_RESULT;

XEF_RESULT xef_native_exec_ctx(XEF_EXEC_CONTEXT *ctx, UINT16 *exit_code);

#endif
