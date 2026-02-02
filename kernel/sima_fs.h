#ifndef __SIMA_FS__
#define __SIMA_FS__

#include "sima_type.h"

#define FS_MAX_FILES     16
#define FS_FILENAME_MAX  12
#define FS_BLOCK_SIZE    1024

typedef struct {
    BOOL used;
    BOOL executable;
    char name[FS_FILENAME_MAX];
    UINT16 size;
} FS_FILE;

BOOL fs_init(void);
BOOL fs_format(void);
BOOL fs_write(const char *name, const UINT8 *data, UINT16 size);
BOOL fs_read(const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size);
BOOL fs_delete(const char *name);
BOOL fs_get_entry(UINT16 index, FS_FILE *out);
BOOL fs_is_executable(const char *name);
BOOL fs_sync(void);

#endif
