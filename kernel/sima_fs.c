#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_ide.h"

#define FS_SIGNATURE "SFS1"
#define FS_VERSION   1
#define FS_ENTRY_SIZE 16
#define FS_HEADER_SIZE 16

#define FS_TABLE_BYTES (FS_MAX_FILES * FS_ENTRY_SIZE)
#define FS_DATA_BYTES  (FS_MAX_FILES * FS_BLOCK_SIZE)
#define FS_RAW_BYTES   (FS_HEADER_SIZE + FS_TABLE_BYTES + FS_DATA_BYTES)
#define FS_DISK_SECTORS ((FS_RAW_BYTES + 511) / 512)
#define FS_IMAGE_BYTES (FS_DISK_SECTORS * 512)
#define FS_DISK_LBA_START 10

static FS_FILE fs_table[FS_MAX_FILES];
static UINT8 fs_data[FS_DATA_BYTES];
static UINT8 fs_disk_image[FS_IMAGE_BYTES];

static void fs_write_u16(UINT8 *buf, UINT16 offset, UINT16 value) {
    buf[offset] = (UINT8)(value & 0xFF);
    buf[offset + 1] = (UINT8)((value >> 8) & 0xFF);
}

static UINT16 fs_read_u16(const UINT8 *buf, UINT16 offset) {
    return (UINT16)(buf[offset] | ((UINT16)buf[offset + 1] << 8));
}

static BOOL fs_name_valid(const char *name) {
    UINT16 len;
    if (!name || name[0] == '\0') return FALSE;
    len = sima_strlen(name);
    return (len < FS_FILENAME_MAX);
}

static BOOL fs_is_executable_name(const char *name) {
    const char *ext;
    if (!name) return FALSE;
    ext = sima_strchr(name, '.');
    if (!ext) return FALSE;
    if (sima_strcmp(ext, ".prg") == STRC_SAME) return TRUE;
    if (sima_strcmp(ext, ".bin") == STRC_SAME) return TRUE;
    return FALSE;
}

static INT16 fs_find_index(const char *name) {
    UINT16 i;
    if (!name) return -1;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        if (fs_table[i].used && sima_strcmp(fs_table[i].name, name) == STRC_SAME) {
            return (INT16)i;
        }
    }
    return -1;
}

static INT16 fs_find_free(void) {
    UINT16 i;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        if (!fs_table[i].used) return (INT16)i;
    }
    return -1;
}

static void fs_seed_defaults(void) {
    static const char program[] =
        "PRINT Welcome to Seolsem OS\n"
        "PRINT Type HELP to see available commands.\n"
        "EXIT\n";

    fs_write("hello.prg", (const UINT8*)program, (UINT16)sima_strlen(program));
}

static void fs_build_image(void) {
    UINT16 i;
    UINT16 offset;

    sima_memset(fs_disk_image, 0, (UINT16)FS_IMAGE_BYTES);
    fs_disk_image[0] = 'S';
    fs_disk_image[1] = 'F';
    fs_disk_image[2] = 'S';
    fs_disk_image[3] = '1';
    fs_write_u16(fs_disk_image, 4, FS_VERSION);
    fs_write_u16(fs_disk_image, 6, FS_BLOCK_SIZE);
    fs_write_u16(fs_disk_image, 8, FS_MAX_FILES);
    fs_write_u16(fs_disk_image, 10, FS_DATA_BYTES);
    fs_write_u16(fs_disk_image, 12, FS_TABLE_BYTES);

    offset = FS_HEADER_SIZE;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        UINT8 flags = 0;
        if (fs_table[i].used) flags |= 0x01;
        if (fs_table[i].executable) flags |= 0x02;

        if (fs_table[i].used) {
            sima_memcpy(&fs_disk_image[offset], fs_table[i].name, FS_FILENAME_MAX);
            fs_write_u16(fs_disk_image, (UINT16)(offset + 12), fs_table[i].size);
        }
        fs_disk_image[offset + 14] = flags;
        offset = (UINT16)(offset + FS_ENTRY_SIZE);
    }

    sima_memcpy(&fs_disk_image[FS_HEADER_SIZE + FS_TABLE_BYTES], fs_data, FS_DATA_BYTES);
}

static BOOL fs_load_image(const UINT8 *image) {
    UINT16 i;
    UINT16 offset;

    if (!image) return FALSE;
    if (image[0] != 'S' || image[1] != 'F' || image[2] != 'S' || image[3] != '1') return FALSE;
    if (fs_read_u16(image, 6) != FS_BLOCK_SIZE) return FALSE;
    if (fs_read_u16(image, 8) != FS_MAX_FILES) return FALSE;

    sima_memclr((char*)fs_table, (UINT16)sizeof(fs_table));
    sima_memclr((char*)fs_data, (UINT16)sizeof(fs_data));

    offset = FS_HEADER_SIZE;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        UINT8 flags = image[offset + 14];
        if (flags & 0x01) {
            sima_memcpy(fs_table[i].name, &image[offset], FS_FILENAME_MAX);
            fs_table[i].name[FS_FILENAME_MAX - 1] = '\0';
            fs_table[i].size = fs_read_u16(image, (UINT16)(offset + 12));
            fs_table[i].used = TRUE;
            fs_table[i].executable = (flags & 0x02) ? TRUE : FALSE;
        }
        offset = (UINT16)(offset + FS_ENTRY_SIZE);
    }

    sima_memcpy(fs_data, &image[FS_HEADER_SIZE + FS_TABLE_BYTES], FS_DATA_BYTES);
    return TRUE;
}

static BOOL fs_load_from_disk(void) {
    UINT16 i;
    for (i = 0; i < FS_DISK_SECTORS; ++i) {
        if (!ide_read_sector((UINT32)(FS_DISK_LBA_START + i),
                             &fs_disk_image[i * 512])) {
            return FALSE;
        }
    }
    return fs_load_image(fs_disk_image);
}

BOOL fs_sync(void) {
    UINT16 i;
    fs_build_image();
    for (i = 0; i < FS_DISK_SECTORS; ++i) {
        if (!ide_write_sector((UINT32)(FS_DISK_LBA_START + i),
                              &fs_disk_image[i * 512])) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL fs_format(void) {
    sima_memclr((char*)fs_table, (UINT16)sizeof(fs_table));
    sima_memclr((char*)fs_data, (UINT16)sizeof(fs_data));
    return TRUE;
}

BOOL fs_init(void) {
    if (!fs_load_from_disk()) {
        fs_format();
        fs_seed_defaults();
    }
    return TRUE;
}

BOOL fs_write(const char *name, const UINT8 *data, UINT16 size) {
    INT16 index;
    UINT8 *slot;

    if (!fs_name_valid(name) || !data) return FALSE;
    if (size > FS_BLOCK_SIZE) return FALSE;

    index = fs_find_index(name);
    if (index < 0) index = fs_find_free();
    if (index < 0) return FALSE;

    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memset(slot, 0, FS_BLOCK_SIZE);
    sima_memcpy(slot, data, size);

    fs_table[index].used = TRUE;
    fs_table[index].executable = fs_is_executable_name(name);
    fs_table[index].size = size;
    sima_strcpy(fs_table[index].name, FS_FILENAME_MAX, name);
    return TRUE;
}

BOOL fs_read(const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    INT16 index;
    UINT8 *slot;

    if (!name || !out || !out_size) return FALSE;
    index = fs_find_index(name);
    if (index < 0) return FALSE;

    if (fs_table[index].size > out_cap) return FALSE;
    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memcpy(out, slot, fs_table[index].size);
    *out_size = fs_table[index].size;
    return TRUE;
}

BOOL fs_delete(const char *name) {
    INT16 index;
    if (!name) return FALSE;
    index = fs_find_index(name);
    if (index < 0) return FALSE;

    fs_table[index].used = FALSE;
    fs_table[index].executable = FALSE;
    fs_table[index].size = 0;
    sima_memset(&fs_data[(UINT16)index * FS_BLOCK_SIZE], 0, FS_BLOCK_SIZE);
    return TRUE;
}

BOOL fs_get_entry(UINT16 index, FS_FILE *out) {
    if (!out || index >= FS_MAX_FILES) return FALSE;
    sima_memcpy(out, &fs_table[index], (UINT16)sizeof(FS_FILE));
    return TRUE;
}

BOOL fs_is_executable(const char *name) {
    INT16 index = fs_find_index(name);
    if (index < 0) return FALSE;
    return fs_table[index].executable;
}
