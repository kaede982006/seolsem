#ifndef __SIMA_FS__
#define __SIMA_FS__

#include "sima_type.h"

#define FS_BLOCK_SIZE 512
#define FS_NAME_MAX 13
#define FS_PATH_MAX 128

typedef struct {
    char name[FS_NAME_MAX];
    UINT32 size;
    UINT32 first_cluster;    /* Changed to UINT32 for FAT32 */
    BOOL is_dir;
    BOOL is_volume;
    BOOL is_hidden;
    BOOL is_system;
    BOOL is_exec;
} FS_DIRENT;

typedef struct {
    UINT32 cluster;          /* directory start cluster (0 = root in FAT12/16 context, but FAT32 uses a cluster) */
    UINT32 cur_cluster;      /* current cluster while iterating */
    UINT32 offset;           /* byte offset within current cluster */
    UINT16 root_index;       /* entry index for root directory (legacy FAT12/16 root) */
    BOOL root;               /* TRUE if iterating root directory (legacy FAT12/16) */
    UINT32 guard_clusters;   /* safety guard for corrupted FAT chains */
    BOOL error;              /* TRUE if an I/O or consistency error occurred */
} FS_DIR;

BOOL fs_init(void);
BOOL fs_cd(const char *path);
BOOL fs_get_cwd(char *out, UINT16 out_cap);
BOOL fs_make_abs_path(const char *path, char *out, UINT16 out_cap);

BOOL fs_dir_open(const char *path, FS_DIR *out_dir);
BOOL fs_dir_read(FS_DIR *dir, FS_DIRENT *out);

BOOL fs_read(const char *path, UINT8 *out, UINT32 out_cap, UINT32 *out_size);
BOOL fs_read_in_dir(const char *dir_name, const char *name, UINT8 *out, UINT32 out_cap, UINT32 *out_size);
BOOL fs_stat(const char *path, FS_DIRENT *out_ent);
BOOL fs_stat_in_dir(const char *dir_name, const char *name, FS_DIRENT *out_ent);

BOOL fs_is_executable(const char *path);
BOOL fs_is_executable_in_dir(const char *dir_name, const char *name);
BOOL fs_is_dir(const char *path);

BOOL fs_get_volume_info(UINT32 *out_total_sectors, UINT8 *out_sec_per_clus);
BOOL fs_get_volume_info32(UINT32 *out_total_sectors, UINT8 *out_sec_per_clus, UINT32 *out_base_lba);

/* File system write operations */
BOOL fs_format(void);
BOOL fs_create_dir(const char *name);
BOOL fs_write(const char *name, const UINT8 *data, UINT32 size); /** Changed size to UINT32 */
BOOL fs_write_in_dir(const char *dir_name, const char *name, const UINT8 *data, UINT32 size); /** Changed size to UINT32 */
BOOL fs_delete(const char *name);
BOOL fs_delete_dir(const char *name);
BOOL fs_sync(void);

#endif
