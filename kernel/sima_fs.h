#ifndef __SIMA_FS__
#define __SIMA_FS__

#include "sima_type.h"

#define FS_BLOCK_SIZE 512
#define FS_NAME_MAX 13
#define FS_PATH_MAX 128

typedef struct {
    char name[FS_NAME_MAX];
    UINT32 size;
    UINT16 first_cluster;
    BOOL is_dir;
    BOOL is_volume;
    BOOL is_hidden;
    BOOL is_system;
    BOOL is_exec;
} FS_DIRENT;

typedef struct {
    UINT16 cluster;          /* directory start cluster (0 = root) */
    UINT16 cur_cluster;      /* current cluster while iterating */
    UINT16 offset;           /* byte offset within current cluster */
    UINT16 root_index;       /* entry index for root directory */
    BOOL root;               /* TRUE if iterating root directory */
} FS_DIR;

BOOL fs_init(void);
BOOL fs_cd(const char *path);
BOOL fs_get_cwd(char *out, UINT16 out_cap);

BOOL fs_dir_open(const char *path, FS_DIR *out_dir);
BOOL fs_dir_read(FS_DIR *dir, FS_DIRENT *out);

BOOL fs_read(const char *path, UINT8 *out, UINT16 out_cap, UINT16 *out_size);
BOOL fs_read_in_dir(const char *dir_name, const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size);

BOOL fs_is_executable(const char *path);
BOOL fs_is_executable_in_dir(const char *dir_name, const char *name);
BOOL fs_is_dir(const char *path);

/* FAT12 write operations (8.3 only; no LFN). */
BOOL fs_format(void);
BOOL fs_create_dir(const char *name);
BOOL fs_write(const char *name, const UINT8 *data, UINT16 size);
BOOL fs_write_in_dir(const char *dir_name, const char *name, const UINT8 *data, UINT16 size);
BOOL fs_delete(const char *name);
BOOL fs_sync(void);

#endif
