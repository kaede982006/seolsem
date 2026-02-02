#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_ide.h"

#define FS_SIGNATURE "SFS1"
#define FS_VERSION   2
#define FS_ENTRY_SIZE 20
#define FS_HEADER_SIZE 16

#define FS_TABLE_BYTES ((unsigned long)FS_MAX_FILES * (unsigned long)FS_ENTRY_SIZE)
#define FS_DATA_BYTES  ((unsigned long)FS_MAX_FILES * (unsigned long)FS_BLOCK_SIZE)
#define FS_RAW_BYTES   ((unsigned long)FS_HEADER_SIZE + FS_TABLE_BYTES + FS_DATA_BYTES)
#define FS_DISK_SECTORS ((FS_RAW_BYTES + 511UL) / 512UL)
#define FS_DISK_LBA_START 10

static FS_FILE fs_table[FS_MAX_FILES];
static UINT8 fs_data[FS_DATA_BYTES];

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

static INT16 fs_find_child(const char *name, UINT16 parent) {
    UINT16 i;
    if (!name) return -1;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        if (fs_table[i].used &&
            fs_table[i].parent == parent &&
            sima_strcmp(fs_table[i].name, name) == STRC_SAME) {
            return (INT16)i;
        }
    }
    return -1;
}

static INT16 fs_find_root_dir(const char *name) {
    INT16 index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) return -1;
    return fs_table[index].is_dir ? index : -1;
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
    static const char memo_program[] =
        "PRINT Memo Pad\n"
        "PRINT Use: EDIT <file> to write a memo.\n"
        "EXIT\n";
    static const char xenv[] =
        "PATH=BIN\n"
        "HOME=/\n";

    fs_write("XENV.ENV", (const UINT8*)xenv, (UINT16)sima_strlen(xenv));
    fs_create_dir("BIN");
    fs_write_in_dir("BIN", "hello.prg", (const UINT8*)program, (UINT16)sima_strlen(program));
    fs_write_in_dir("BIN", "memo.prg", (const UINT8*)memo_program, (UINT16)sima_strlen(memo_program));
}

static void fs_build_header_table(UINT8 *header_table) {
    UINT16 i;
    UINT16 offset;

    sima_memset(header_table, 0, (UINT16)(FS_HEADER_SIZE + FS_TABLE_BYTES));
    header_table[0] = 'S';
    header_table[1] = 'F';
    header_table[2] = 'S';
    header_table[3] = '1';
    fs_write_u16(header_table, 4, FS_VERSION);
    fs_write_u16(header_table, 6, FS_BLOCK_SIZE);
    fs_write_u16(header_table, 8, FS_MAX_FILES);
    fs_write_u16(header_table, 10, FS_DATA_BYTES);
    fs_write_u16(header_table, 12, FS_TABLE_BYTES);

    offset = FS_HEADER_SIZE;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        UINT8 flags = 0;
        if (fs_table[i].used) flags |= 0x01;
        if (fs_table[i].executable) flags |= 0x02;
        if (fs_table[i].is_dir) flags |= 0x04;

        if (fs_table[i].used) {
            sima_memcpy(&header_table[offset], fs_table[i].name, FS_FILENAME_MAX);
            fs_write_u16(header_table, (UINT16)(offset + 12), fs_table[i].size);
            fs_write_u16(header_table, (UINT16)(offset + 16), fs_table[i].parent);
        }
        header_table[offset + 14] = flags;
        offset = (UINT16)(offset + FS_ENTRY_SIZE);
    }
}

static BOOL fs_read_range(UINT32 offset, UINT8 *out, UINT16 size) {
    UINT32 pos = offset;
    UINT16 remaining = size;
    UINT8 sector[512];

    while (remaining > 0) {
        UINT32 sector_index = pos / 512UL;
        UINT16 sector_offset = (UINT16)(pos % 512UL);
        UINT16 chunk = (UINT16)(512U - sector_offset);

        if (chunk > remaining) chunk = remaining;
        if (!ide_read_sector(FS_DISK_LBA_START + sector_index, sector)) {
            return FALSE;
        }
        sima_memcpy(out, &sector[sector_offset], chunk);
        out += chunk;
        pos += chunk;
        remaining = (UINT16)(remaining - chunk);
    }
    return TRUE;
}

static BOOL fs_load_from_disk(void) {
    UINT16 i;
    UINT16 offset;
    UINT8 header_table[FS_HEADER_SIZE + FS_TABLE_BYTES];

    if (!fs_read_range(0, header_table, (UINT16)sizeof(header_table))) return FALSE;
    if (header_table[0] != 'S' || header_table[1] != 'F' || header_table[2] != 'S' || header_table[3] != '1') return FALSE;
    if (fs_read_u16(header_table, 4) != FS_VERSION) return FALSE;
    if (fs_read_u16(header_table, 6) != FS_BLOCK_SIZE) return FALSE;
    if (fs_read_u16(header_table, 8) != FS_MAX_FILES) return FALSE;

    sima_memclr((char*)fs_table, (UINT16)sizeof(fs_table));
    sima_memclr((char*)fs_data, (UINT16)sizeof(fs_data));

    offset = FS_HEADER_SIZE;
    for (i = 0; i < FS_MAX_FILES; ++i) {
        UINT8 flags = header_table[offset + 14];
        if (flags & 0x01) {
            sima_memcpy(fs_table[i].name, &header_table[offset], FS_FILENAME_MAX);
            fs_table[i].name[FS_FILENAME_MAX - 1] = '\0';
            fs_table[i].size = fs_read_u16(header_table, (UINT16)(offset + 12));
            fs_table[i].parent = fs_read_u16(header_table, (UINT16)(offset + 16));
            fs_table[i].used = TRUE;
            fs_table[i].executable = (flags & 0x02) ? TRUE : FALSE;
            fs_table[i].is_dir = (flags & 0x04) ? TRUE : FALSE;
        }
        offset = (UINT16)(offset + FS_ENTRY_SIZE);
    }

    return fs_read_range(FS_HEADER_SIZE + FS_TABLE_BYTES, fs_data, FS_DATA_BYTES);
}

BOOL fs_sync(void) {
    UINT16 i;
    UINT32 sector_offset;
    UINT16 header_table_size = (UINT16)(FS_HEADER_SIZE + FS_TABLE_BYTES);
    UINT8 header_table[FS_HEADER_SIZE + FS_TABLE_BYTES];
    UINT8 sector[512];

    fs_build_header_table(header_table);

    for (i = 0; i < FS_DISK_SECTORS; ++i) {
        UINT16 copy_size;
        sima_memset(sector, 0, sizeof(sector));
        sector_offset = (UINT32)i * 512UL;

        if (sector_offset < header_table_size) {
            copy_size = (UINT16)(header_table_size - sector_offset);
            if (copy_size > 512U) copy_size = 512U;
            sima_memcpy(sector, &header_table[sector_offset], copy_size);
        }

        if (sector_offset + 512UL > (UINT32)(FS_HEADER_SIZE + FS_TABLE_BYTES)) {
            UINT32 data_start = (UINT32)(FS_HEADER_SIZE + FS_TABLE_BYTES);
            UINT32 data_end = data_start + FS_DATA_BYTES;
            UINT32 overlap_start = sector_offset > data_start ? sector_offset : data_start;
            UINT32 overlap_end = (sector_offset + 512UL) < data_end ? (sector_offset + 512UL) : data_end;

            if (overlap_end > overlap_start) {
                UINT16 sector_pos = (UINT16)(overlap_start - sector_offset);
                UINT16 data_pos = (UINT16)(overlap_start - data_start);
                UINT16 overlap_size = (UINT16)(overlap_end - overlap_start);
                sima_memcpy(&sector[sector_pos], &fs_data[data_pos], overlap_size);
            }
        }

        if (!ide_write_sector((UINT32)(FS_DISK_LBA_START + i), sector)) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL fs_format(void) {
    sima_memclr((char*)fs_table, (UINT16)sizeof(fs_table));
    sima_memclr((char*)fs_data, (UINT16)sizeof(fs_data));
    fs_table[FS_ROOT_INDEX].used = TRUE;
    fs_table[FS_ROOT_INDEX].is_dir = TRUE;
    fs_table[FS_ROOT_INDEX].parent = FS_ROOT_INDEX;
    sima_strcpy(fs_table[FS_ROOT_INDEX].name, FS_FILENAME_MAX, "/");
    return TRUE;
}

BOOL fs_create_dir(const char *name) {
    INT16 index;
    if (!fs_name_valid(name)) return FALSE;
    if (fs_find_root_dir(name) >= 0) return FALSE;

    index = fs_find_free();
    if (index < 0) return FALSE;
    fs_table[index].used = TRUE;
    fs_table[index].executable = FALSE;
    fs_table[index].is_dir = TRUE;
    fs_table[index].parent = FS_ROOT_INDEX;
    fs_table[index].size = 0;
    sima_strcpy(fs_table[index].name, FS_FILENAME_MAX, name);
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

    index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) index = fs_find_free();
    if (index < 0) return FALSE;

    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memset(slot, 0, FS_BLOCK_SIZE);
    sima_memcpy(slot, data, size);

    fs_table[index].used = TRUE;
    fs_table[index].executable = fs_is_executable_name(name);
    fs_table[index].is_dir = FALSE;
    fs_table[index].parent = FS_ROOT_INDEX;
    fs_table[index].size = size;
    sima_strcpy(fs_table[index].name, FS_FILENAME_MAX, name);
    return TRUE;
}

BOOL fs_write_in_dir(const char *dir_name, const char *name, const UINT8 *data, UINT16 size) {
    INT16 dir_index;
    INT16 index;
    UINT8 *slot;

    if (!fs_name_valid(name) || !data) return FALSE;
    if (size > FS_BLOCK_SIZE) return FALSE;
    dir_index = fs_find_root_dir(dir_name);
    if (dir_index < 0) return FALSE;

    index = fs_find_child(name, (UINT16)dir_index);
    if (index < 0) index = fs_find_free();
    if (index < 0) return FALSE;

    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memset(slot, 0, FS_BLOCK_SIZE);
    sima_memcpy(slot, data, size);

    fs_table[index].used = TRUE;
    fs_table[index].executable = fs_is_executable_name(name);
    fs_table[index].is_dir = FALSE;
    fs_table[index].parent = (UINT16)dir_index;
    fs_table[index].size = size;
    sima_strcpy(fs_table[index].name, FS_FILENAME_MAX, name);
    return TRUE;
}

BOOL fs_read(const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    INT16 index;
    UINT8 *slot;

    if (!name || !out || !out_size) return FALSE;
    index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) return FALSE;
    if (fs_table[index].is_dir) return FALSE;

    if (fs_table[index].size > out_cap) return FALSE;
    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memcpy(out, slot, fs_table[index].size);
    *out_size = fs_table[index].size;
    return TRUE;
}

BOOL fs_read_in_dir(const char *dir_name, const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    INT16 dir_index;
    INT16 index;
    UINT8 *slot;

    if (!name || !out || !out_size) return FALSE;
    dir_index = fs_find_root_dir(dir_name);
    if (dir_index < 0) return FALSE;
    index = fs_find_child(name, (UINT16)dir_index);
    if (index < 0) return FALSE;
    if (fs_table[index].is_dir) return FALSE;
    if (fs_table[index].size > out_cap) return FALSE;

    slot = &fs_data[(UINT16)index * FS_BLOCK_SIZE];
    sima_memcpy(out, slot, fs_table[index].size);
    *out_size = fs_table[index].size;
    return TRUE;
}
BOOL fs_delete(const char *name) {
    INT16 index;
    if (!name) return FALSE;
    index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) return FALSE;
    if (fs_table[index].is_dir) return FALSE;

    fs_table[index].used = FALSE;
    fs_table[index].executable = FALSE;
    fs_table[index].is_dir = FALSE;
    fs_table[index].parent = 0;
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
    INT16 index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) return FALSE;
    return fs_table[index].executable;
}

BOOL fs_is_executable_in_dir(const char *dir_name, const char *name) {
    INT16 dir_index = fs_find_root_dir(dir_name);
    INT16 index;
    if (dir_index < 0) return FALSE;
    index = fs_find_child(name, (UINT16)dir_index);
    if (index < 0) return FALSE;
    return fs_table[index].executable;
}

BOOL fs_is_dir(const char *name) {
    INT16 index = fs_find_child(name, FS_ROOT_INDEX);
    if (index < 0) return FALSE;
    return fs_table[index].is_dir;
}
