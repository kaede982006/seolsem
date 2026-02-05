#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_ide.h"

#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

/* FAT32 EOC marks */
#define FAT_EOC_MIN        0x0FFFFFF8
#define FAT_EOC_MAX        0x0FFFFFFF
#define FAT_BAD_CLUSTER    0x0FFFFFF7
#define FAT_FREE_CLUSTER   0x00000000

#define FAT_CACHE_MAX_BYTES 8192
#define SECTOR_SIZE         512

typedef enum {
    FAT_TYPE_UNKNOWN = 0,
    FAT_TYPE_FAT12,
    FAT_TYPE_FAT16,
    FAT_TYPE_FAT32
} FAT_TYPE;

typedef struct {
    UINT16 bytes_per_sec;
    UINT8  sec_per_clus;
    UINT16 res_sectors;
    UINT8  fats;
    UINT16 root_ents;
    UINT16 total_sectors16;
    UINT8  media;
    UINT16 fat_secs16;
    UINT16 sec_per_track;
    UINT16 heads;
    UINT32 hidden_sectors;
    UINT32 total_sectors32;
    /* FAT32 specific fields */
    UINT32 fat_secs32;
    UINT16 ext_flags;
    UINT16 fs_ver;
    UINT32 root_cluster;
    UINT16 fs_info;
    UINT16 backup_boot;
} FAT_BPB;

typedef struct {
    UINT8 name[8];
    UINT8 ext[3];
    UINT8 attr;
    UINT8 reserved;
    UINT8 ctime_tenths;
    UINT8 ctime[2];
    UINT8 cdate[2];
    UINT8 adate[2];
    UINT8 start_cluster_hi[2];
    UINT8 mtime[2];
    UINT8 mdate[2];
    UINT8 start_cluster_lo[2];
    UINT8 size[4];
} FAT_DIR_RAW;

typedef struct {
    BOOL root;
    UINT16 root_index; /* valid if root == TRUE (FAT12/16 only) */
    UINT32 cluster;    /* valid if root == FALSE or FAT32 */
    UINT32 offset;     /* byte offset within cluster */
} FAT_DIR_LOC;

static FAT_BPB g_bpb;
static FAT_TYPE g_fat_type = FAT_TYPE_UNKNOWN;
static UINT32 g_base_lba;
static UINT8  g_spc_shift;
static UINT32 g_fat_start_lba;
static UINT32 g_root_start_lba; /* For FAT12/16, or cluster 2 LBA for FAT32 calculation base */
static UINT32 g_data_start_lba;
static UINT32 g_total_sectors;
static UINT32 g_sectors_per_fat;
static UINT32 g_root_dir_sectors; /* 0 for FAT32 */

/* Since FAT32 tables are large, we only cache one sector of the FAT at a time */
static UINT8  g_fat_sector[SECTOR_SIZE];
static UINT32 g_cur_fat_sector_lba = 0; /* 0 means invalid/none loaded */
static BOOL   g_fat_dirty = FALSE;

/* General sector buffer */
static UINT8  g_sector[SECTOR_SIZE];

static UINT32 g_count_of_clusters; // Total count of data clusters

static UINT32 g_cwd_cluster = 0; /* 0 means root directory */
static char   g_cwd_path[FS_PATH_MAX] = "/";

/* Forward declarations used by prompt path normalization. */
static UINT8 to_upper(UINT8 ch);
static UINT32 fat_max_cluster(void);
static BOOL fat_cluster_is_valid(UINT32 cluster);
static const char *fat_skip_separators(const char *cursor);
static BOOL fat_next_segment(const char **path_cursor, char *segment, UINT16 segment_cap);

static void fs_upper_inplace(char *s) {
    UINT16 i;
    if (!s) return;
    for (i = 0; s[i] != '\0'; ++i) {
        s[i] = (char)to_upper((UINT8)s[i]);
    }
}

static BOOL fs_normalize_path(const char *base_abs, const char *path, char *out, UINT16 out_cap) {
    /* Build canonical absolute path for prompt/UI purposes only. */
    enum { FS_MAX_DEPTH = 32 };
    char segs[FS_MAX_DEPTH][FS_NAME_MAX];
    UINT16 depth = 0;
    const char *cursor;
    char segment[FS_NAME_MAX];
    UINT16 i;

    if (!out || out_cap == 0) return FALSE;
    out[0] = '\0';

    if (!base_abs || base_abs[0] == '\0') base_abs = "/";
    if (!path || path[0] == '\0') {
        /* Best-effort copy (truncation is acceptable for UI). */
        (void)sima_strcpy(out, out_cap, base_abs);
        if (out[0] == '\0') (void)sima_strcpy(out, out_cap, "/");
        return TRUE;
    }

    /* If relative, seed from base path */
    if (!(path[0] == '/' || path[0] == '\\')) {
        cursor = base_abs;
        while (fat_next_segment(&cursor, segment, (UINT16)sizeof(segment))) {
            if (segment[0] == '\0') break;
            if (depth >= FS_MAX_DEPTH) return FALSE;
            sima_strcpy(segs[depth], (UINT16)sizeof(segs[depth]), segment);
            fs_upper_inplace(segs[depth]);
            ++depth;
            cursor = fat_skip_separators(cursor);
        }
    }

    cursor = path;
    while (fat_next_segment(&cursor, segment, (UINT16)sizeof(segment))) {
        if (segment[0] == '.' && segment[1] == '\0') {
            cursor = fat_skip_separators(cursor);
            continue;
        }
        if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
            if (depth > 0) --depth;
            cursor = fat_skip_separators(cursor);
            continue;
        }
        if (depth >= FS_MAX_DEPTH) return FALSE;
        sima_strcpy(segs[depth], (UINT16)sizeof(segs[depth]), segment);
        fs_upper_inplace(segs[depth]);
        ++depth;
        cursor = fat_skip_separators(cursor);
    }

    /* Best-effort build (truncation is acceptable for UI). */
    (void)sima_strcpy(out, out_cap, "/");
    for (i = 0; i < depth; ++i) {
        (void)sima_strcat(out, out_cap, segs[i]);
        if (i + 1 < depth) {
            (void)sima_strcat(out, out_cap, "/");
        }
    }
    if (out[0] == '\0') (void)sima_strcpy(out, out_cap, "/");
    return TRUE;
}

static UINT16 le16(const UINT8 *p) {
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static UINT32 le32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static void wr16(UINT8 *p, UINT16 v) {
    p[0] = (UINT8)(v & 0xFF);
    p[1] = (UINT8)((v >> 8) & 0xFF);
}

static void wr32(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)(v & 0xFF);
    p[1] = (UINT8)((v >> 8) & 0xFF);
    p[2] = (UINT8)((v >> 16) & 0xFF);
    p[3] = (UINT8)((v >> 24) & 0xFF);
}

static UINT8 to_upper(UINT8 ch) {
    if (ch >= 'a' && ch <= 'z') return (UINT8)(ch - 'a' + 'A');
    return ch;
}

static BOOL read_sector(UINT32 lba, UINT8 *buf) {
    return ide_read_sector(lba, buf);
}

static BOOL write_sector(UINT32 lba, const UINT8 *buf) {
    return ide_write_sector(lba, buf);
}

static BOOL fat_calc_spc_shift(UINT8 sec_per_clus, UINT8 *out_shift) {
    UINT8 shift = 0;
    if (!out_shift) return FALSE;
    if (sec_per_clus == 0) return FALSE;
    while ((sec_per_clus & 1U) == 0) {
        sec_per_clus >>= 1;
        ++shift;
    }
    if (sec_per_clus != 1U) return FALSE;
    if (shift > 7U) return FALSE; /* 2^7 = 128 sectors/cluster max usually */
    *out_shift = shift;
    return TRUE;
}

static BOOL fat_flush_fat_sector(void) {
    if (g_cur_fat_sector_lba == 0 || !g_fat_dirty) return TRUE;
    {
        UINT8 fat_index;
        UINT32 sector_offset = g_cur_fat_sector_lba - g_fat_start_lba;
        for (fat_index = 0; fat_index < g_bpb.fats; ++fat_index) {
            UINT32 lba = g_fat_start_lba + sector_offset + ((UINT32)fat_index * g_sectors_per_fat);
            if (!write_sector(lba, g_fat_sector)) return FALSE;
        }
    }
    g_fat_dirty = FALSE;
    return TRUE;
}

static BOOL fat_load_fat_sector(UINT32 sector_offset_in_fat) {
    UINT32 target_lba = g_fat_start_lba + sector_offset_in_fat;
    if (target_lba == g_cur_fat_sector_lba) return TRUE;

    if (g_fat_dirty) {
        if (!fat_flush_fat_sector()) return FALSE;
    }

    if (!read_sector(target_lba, g_fat_sector)) {
        g_cur_fat_sector_lba = 0;
        return FALSE;
    }

    g_cur_fat_sector_lba = target_lba;
    g_fat_dirty = FALSE;
    return TRUE;
}

static BOOL mbr_find_partition_start(const UINT8 *mbr, UINT32 *out_start_lba) {
    UINT16 i;
    UINT32 start_lba = 0;
    BOOL found_active = FALSE;

    if (!mbr || !out_start_lba) return FALSE;
    *out_start_lba = 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return FALSE;

    /* Prefer active partition (status 0x80), otherwise first non-empty entry. */
    for (i = 0; i < 4; ++i) {
        const UINT8 *e = &mbr[0x1BE + (i << 4)];
        UINT8 status = e[0];
        UINT8 type = e[4];
        if (type == 0) continue;
        start_lba = le32(&e[8]);
        if (start_lba == 0) continue;
        if (status == 0x80) {
            found_active = TRUE;
            break;
        }
        if (!found_active) {
            /* Keep as fallback. */
            *out_start_lba = start_lba;
        }
    }

    if (found_active) {
        *out_start_lba = start_lba;
        return TRUE;
    }

    return (*out_start_lba != 0);
}

static FAT_TYPE determine_fat_type(UINT32 clusters) {
    if (clusters < 4085) return FAT_TYPE_FAT12;
    if (clusters < 65525) return FAT_TYPE_FAT16;
    return FAT_TYPE_FAT32;
}

static BOOL fat_load_bpb(void) {
    UINT8 spc_shift;
    UINT32 base_lba = 0;

    if (!read_sector(0, g_sector)) return FALSE;

    /* Simple check for MBR vs Boot Sector */
    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA) return FALSE;

    /* Check if it looks like a BPB (basic check) */
    {
        UINT16 bps = le16(&g_sector[0x0B]);
        if (bps != SECTOR_SIZE) {
            /* Likely MBR, try to find partition */
            UINT32 part_start = 0;
            if (!mbr_find_partition_start(g_sector, &part_start)) return FALSE;
            if (!read_sector(part_start, g_sector)) return FALSE;
            if (le16(&g_sector[0x0B]) != SECTOR_SIZE) return FALSE;
            base_lba = part_start;
        }
    }

    g_base_lba = base_lba;

    g_bpb.bytes_per_sec = le16(&g_sector[0x0B]);
    g_bpb.sec_per_clus = g_sector[0x0D];
    g_bpb.res_sectors = le16(&g_sector[0x0E]);
    g_bpb.fats = g_sector[0x10];
    g_bpb.root_ents = le16(&g_sector[0x11]);
    g_bpb.total_sectors16 = le16(&g_sector[0x13]);
    g_bpb.media = g_sector[0x15];
    g_bpb.fat_secs16 = le16(&g_sector[0x16]);
    g_bpb.sec_per_track = le16(&g_sector[0x18]);
    g_bpb.heads = le16(&g_sector[0x1A]);
    g_bpb.hidden_sectors = le32(&g_sector[0x1C]);
    g_bpb.total_sectors32 = le32(&g_sector[0x20]);

    if (!fat_calc_spc_shift(g_bpb.sec_per_clus, &spc_shift)) return FALSE;
    g_spc_shift = spc_shift;

    g_total_sectors = (g_bpb.total_sectors16 != 0) ? (UINT32)g_bpb.total_sectors16 : g_bpb.total_sectors32;
    if (g_total_sectors == 0) return FALSE;

    if (g_bpb.fat_secs16 != 0) {
        g_sectors_per_fat = (UINT32)g_bpb.fat_secs16;
    } else {
        /* FAT32 specific */
        g_bpb.fat_secs32 = le32(&g_sector[0x24]);
        g_bpb.ext_flags = le16(&g_sector[0x28]);
        g_bpb.fs_ver = le16(&g_sector[0x2A]);
        g_bpb.root_cluster = le32(&g_sector[0x2C]);
        g_bpb.fs_info = le16(&g_sector[0x30]);
        g_bpb.backup_boot = le16(&g_sector[0x32]);
        g_sectors_per_fat = g_bpb.fat_secs32;
    }

    g_root_dir_sectors = ((UINT32)g_bpb.root_ents * 32U + (UINT32)(SECTOR_SIZE - 1)) / (UINT32)SECTOR_SIZE;

    g_fat_start_lba = g_base_lba + g_bpb.res_sectors;
    g_root_start_lba = g_fat_start_lba + (g_sectors_per_fat * g_bpb.fats);
    g_data_start_lba = g_root_start_lba + g_root_dir_sectors;

    {
        UINT32 data_sectors = g_total_sectors - (g_data_start_lba - g_base_lba);
        g_count_of_clusters = data_sectors >> g_spc_shift;
    }

    g_fat_type = determine_fat_type(g_count_of_clusters);

    if (g_fat_type == FAT_TYPE_FAT32 && g_bpb.root_cluster < 2) {
        /* Sanity check for FAT32 */
        return FALSE;
    }

    /* In FAT32, root dir is a cluster chain. In FAT12/16 it is a fixed area. 
       Our FS usage will handle root dir as cluster 0 initially for FAT12/16, 
       and use specific cluster for FAT32. */
    if (g_fat_type == FAT_TYPE_FAT32) {
        g_cwd_cluster = g_bpb.root_cluster;
    } else {
        g_cwd_cluster = 0; /* 0 represents fixed root area in FAT12/16 */
    }

    /* Reset FAT cache */
    g_cur_fat_sector_lba = 0;
    g_fat_dirty = FALSE;

    return TRUE;
}

static UINT32 fat_get_entry(UINT32 cluster) {
    UINT32 fat_offset;
    UINT32 sector_val_offset;
    UINT32 entry_val = 0;

    if (!fat_cluster_is_valid(cluster)) return FAT_BAD_CLUSTER;

    if (g_fat_type == FAT_TYPE_FAT12) {
        fat_offset = cluster + (cluster / 2);
    } else if (g_fat_type == FAT_TYPE_FAT16) {
        fat_offset = cluster * 2;
    } else {
        fat_offset = cluster * 4;
    }

    sector_val_offset = fat_offset % SECTOR_SIZE;
    if (!fat_load_fat_sector(fat_offset / SECTOR_SIZE)) return FAT_BAD_CLUSTER; /* error */

    if (g_fat_type == FAT_TYPE_FAT12) {
        /* We need 2 bytes, might straddle sector boundary */
        UINT16 val16;
        if (sector_val_offset == SECTOR_SIZE - 1) {
            UINT8 b1 = g_fat_sector[SECTOR_SIZE - 1];
            if (!fat_load_fat_sector((fat_offset / SECTOR_SIZE) + 1)) return FAT_BAD_CLUSTER;
            val16 = (UINT16)b1 | ((UINT16)g_fat_sector[0] << 8);
        } else {
            val16 = le16(&g_fat_sector[sector_val_offset]);
        }
        if (cluster & 1) {
            entry_val = val16 >> 4;
        } else {
            entry_val = val16 & 0xFFF;
        }
    } else if (g_fat_type == FAT_TYPE_FAT16) {
        entry_val = le16(&g_fat_sector[sector_val_offset]);
        if (entry_val >= 0xFFF8) entry_val |= 0x0FFFF000; /* Sign extend EOC markers logic? No, just map to 32bit EOC range */
        /* FAT16 EOC is FFF8-FFFF. */
    } else {
        entry_val = le32(&g_fat_sector[sector_val_offset]) & 0x0FFFFFFF;
    }

    return entry_val;
}

static BOOL fat_set_entry(UINT32 cluster, UINT32 value) {
    UINT32 fat_offset;
    UINT32 sector_val_offset;

    if (!fat_cluster_is_valid(cluster)) return FALSE;

    if (g_fat_type == FAT_TYPE_FAT12) {
        fat_offset = cluster + (cluster / 2);
    } else if (g_fat_type == FAT_TYPE_FAT16) {
        fat_offset = cluster * 2;
    } else {
        fat_offset = cluster * 4;
    }

    sector_val_offset = fat_offset % SECTOR_SIZE;
    if (!fat_load_fat_sector(fat_offset / SECTOR_SIZE)) return FALSE;

    if (g_fat_type == FAT_TYPE_FAT12) {
        /* Complex logic for straddling sectors not fully implemented for write in this simplified update if it straddles */
        /* Assuming we don't heavily write FAT12 in this 4GB upgrade context, but implementing basic support */
        UINT16 val16;
        /* Read existing 16 bits to preserve the other nibble */
        if (sector_val_offset == SECTOR_SIZE - 1) {
            /* Straddles boundary. Load next sector is expensive. 
               This is a limitation of this simple cache. 
               TODO: robust FAT12 write support needs better caching mechanism (2 sectors).
               For now, we support standard FAT32 fully. */
             return FALSE; 
        } else {
            val16 = le16(&g_fat_sector[sector_val_offset]);
        }
        
        value &= 0xFFF;
        if (cluster & 1) {
             val16 = (val16 & 0x000F) | (UINT16)(value << 4);
        } else {
             val16 = (val16 & 0xF000) | (UINT16)value;
        }
        wr16(&g_fat_sector[sector_val_offset], val16);
    } else if (g_fat_type == FAT_TYPE_FAT16) {
        wr16(&g_fat_sector[sector_val_offset], (UINT16)value);
    } else {
        /* Preserve high 4 bits */
        UINT32 current = le32(&g_fat_sector[sector_val_offset]);
        value = (current & 0xF0000000) | (value & 0x0FFFFFFF);
        wr32(&g_fat_sector[sector_val_offset], value);
    }

    g_fat_dirty = TRUE;
    return TRUE;
}

static UINT32 fat_cluster_to_lba(UINT32 cluster) {
    return g_data_start_lba + ((cluster - 2) << g_spc_shift);
}

/* 
 * Returns next cluster in chain.
 * Returns 0 on end of chain (or error/bad cluster mapped to 0 for end logic).
 */
static UINT32 fat_next_cluster(UINT32 cluster) {
    UINT32 val = fat_get_entry(cluster);
    UINT32 max = fat_max_cluster();

    if (!fat_cluster_is_valid(cluster)) return 0;
    
    if (g_fat_type == FAT_TYPE_FAT12) {
        if (val >= 0xFF8) return 0;
    } else if (g_fat_type == FAT_TYPE_FAT16) {
        if (val >= 0xFFF8) return 0;
    } else {
        if (val >= 0x0FFFFFF8) return 0;
    }
    if (val == 0) return 0; /* Free cluster shouldn't happen in valid chain */
    if (val == FAT_BAD_CLUSTER) return 0;
    if (val < 2) return 0;
    if (val > max) return 0;

    return val;
}

static BOOL fat_read_root_entry(UINT16 index, FAT_DIR_RAW *out) {
    UINT16 sector = index / (SECTOR_SIZE / 32);
    UINT16 off_in_sector = (index % (SECTOR_SIZE / 32)) * 32;

    if (g_fat_type == FAT_TYPE_FAT32) return FALSE; /* FAT32 has no fixed root */
    if (sector >= g_root_dir_sectors) return FALSE;

    if (!read_sector(g_root_start_lba + sector, g_sector)) return FALSE;
    sima_memcpy(out, &g_sector[off_in_sector], sizeof(FAT_DIR_RAW));
    return TRUE;
}

static BOOL fat_read_cluster_entry(UINT32 cluster, UINT32 offset, FAT_DIR_RAW *out) {
    UINT32 cluster_bytes = (UINT32)1 << (g_spc_shift + 9);
    UINT32 lba = fat_cluster_to_lba(cluster) + (offset >> 9);
    UINT16 off_in_sector = (UINT16)(offset & (SECTOR_SIZE - 1));

    if (!fat_cluster_is_valid(cluster)) return FALSE;
    if (offset + 32U > cluster_bytes) return FALSE;
    if (!read_sector(lba, g_sector)) return FALSE;
    sima_memcpy(out, &g_sector[off_in_sector], sizeof(FAT_DIR_RAW));
    return TRUE;
}

/* 
 * Directory Iterator 
 */
static BOOL fat_dir_read_raw(FS_DIR *dir, FAT_DIR_RAW *out) {
    UINT32 cluster_guard_limit = fat_max_cluster();
    if (cluster_guard_limit > 8192U) cluster_guard_limit = 8192U;

    if (!dir || !out) return FALSE;
    if (dir->error) return FALSE;

    while (TRUE) {
        /* Legacy Root Directory (FAT12/16) */
        if (dir->root && g_fat_type != FAT_TYPE_FAT32) {
            if (dir->root_index >= g_bpb.root_ents) return FALSE;
            if (!fat_read_root_entry(dir->root_index, out)) {
                dir->error = TRUE;
                return FALSE;
            }
            dir->root_index++;
        } 
        /* Cluster Chain Directory */
        else {
            UINT32 cluster_bytes;
            if (!fat_cluster_is_valid(dir->cur_cluster)) {
                /* Invalid starting cluster for a cluster-chain directory. */
                dir->error = TRUE;
                return FALSE;
            }
            
            cluster_bytes = (UINT32)1 << (g_spc_shift + 9);
            if (dir->offset >= cluster_bytes) {
                UINT32 next_cluster = fat_next_cluster(dir->cur_cluster);
                dir->offset = 0;
                if (next_cluster < 2) return FALSE;
                dir->cur_cluster = next_cluster;
                dir->guard_clusters++;
                if (dir->guard_clusters > cluster_guard_limit) {
                    dir->error = TRUE;
                    return FALSE;
                }
            }

            if (!fat_read_cluster_entry(dir->cur_cluster, dir->offset, out)) {
                dir->error = TRUE;
                return FALSE;
            }
            dir->offset += 32;
        }

        if (out->name[0] == 0x00) return FALSE; /* End of Dir */
        if (out->name[0] == 0xE5) continue;     /* Deleted */
        if (out->attr == FAT_ATTR_LFN) continue; /* LFN skipped */
        
        return TRUE;
    }
}

static BOOL fat_build_83(const char *input, UINT8 out[11]) {
    UINT16 i = 0;
    UINT16 base_len = 0;
    UINT16 ext_len = 0;
    BOOL in_ext = FALSE;

    if (!input || input[0] == '\0') return FALSE;
    for (i = 0; i < 11; ++i) out[i] = ' ';

    /* FAT special entries */
    if (input[0] == '.' && input[1] == '\0') {
        out[0] = '.';
        return TRUE;
    }
    if (input[0] == '.' && input[1] == '.' && input[2] == '\0') {
        out[0] = '.';
        out[1] = '.';
        return TRUE;
    }

    i = 0;
    while (input[i] != '\0') {
        char ch = input[i];
        if (ch == '/' || ch == '\\') return FALSE;
        if (ch == '.') {
            if (in_ext) return FALSE;
            in_ext = TRUE;
            ++i;
            continue;
        }
        ch = (char)to_upper((UINT8)ch);
        if (!in_ext) {
            if (base_len >= 8) return FALSE;
            out[base_len++] = (UINT8)ch;
        } else {
            if (ext_len >= 3) return FALSE;
            out[8 + ext_len++] = (UINT8)ch;
        }
        ++i;
    }
    return TRUE;
}

static BOOL fat_names_equal(const FAT_DIR_RAW *raw, const char *input) {
    UINT8 name83[11];
    UINT16 i;
    if (!fat_build_83(input, name83)) return FALSE;
    for (i = 0; i < 8; ++i) {
        if (to_upper(raw->name[i]) != name83[i]) return FALSE;
    }
    for (i = 0; i < 3; ++i) {
        if (to_upper(raw->ext[i]) != name83[8 + i]) return FALSE;
    }
    return TRUE;
}

static void fat_format_name(const FAT_DIR_RAW *raw, char *out, UINT16 out_cap) {
    char name[9];
    char ext[4];
    UINT16 i;

    for (i = 0; i < 8; ++i) name[i] = (char)raw->name[i];
    name[8] = '\0';
    for (i = 7; i > 0 && name[i] == ' '; --i) name[i] = '\0';
    if (name[0] == ' ') name[0] = '\0';
    
    for (i = 0; i < 3; ++i) ext[i] = (char)raw->ext[i];
    ext[3] = '\0';
    for (i = 2; i > 0 && ext[i] == ' '; --i) ext[i] = '\0';
    if (ext[0] == ' ') ext[0] = '\0';

    sima_memclr(out, out_cap);
    if (name[0] == '\0') return;
    sima_strcpy(out, out_cap, name);
    if (ext[0] != '\0') {
        sima_strcat(out, out_cap, ".");
        sima_strcat(out, out_cap, ext);
    }
}

/* Forward declarations (used by write/path helpers before definitions). */
static BOOL fat_find_in_dir(UINT32 dir_cluster, const char *name, FAT_DIR_RAW *out_raw);
static UINT32 fat_entry_cluster(const FAT_DIR_RAW *raw);
static BOOL fat_parent_cluster(UINT32 cluster, UINT32 *out_parent);
static const char *fat_skip_separators(const char *cursor);
static BOOL fat_next_segment(const char **path_cursor, char *segment, UINT16 segment_cap);

static UINT32 fat_eoc_marker(void) {
    if (g_fat_type == FAT_TYPE_FAT12) return 0x0FFF;
    if (g_fat_type == FAT_TYPE_FAT16) return 0xFFFF;
    return 0x0FFFFFFF;
}

static UINT32 fat_max_cluster(void) {
    return g_count_of_clusters + 1;
}

static BOOL fat_cluster_is_valid(UINT32 cluster) {
    UINT32 max = fat_max_cluster();
    if (max < 2) return FALSE;
    return (cluster >= 2 && cluster <= max);
}

static UINT32 g_alloc_hint = 2;

static BOOL fat_find_free_cluster(UINT32 *out_cluster) {
    UINT32 cluster;
    UINT32 start;
    UINT32 max;

    if (!out_cluster) return FALSE;
    max = fat_max_cluster();
    if (max < 2) return FALSE;

    start = g_alloc_hint;
    if (start < 2 || start > max) start = 2;

    for (cluster = start; cluster <= max; ++cluster) {
        if (fat_get_entry(cluster) == FAT_FREE_CLUSTER) {
            *out_cluster = cluster;
            g_alloc_hint = cluster + 1;
            if (g_alloc_hint > max) g_alloc_hint = 2;
            return TRUE;
        }
    }
    for (cluster = 2; cluster < start; ++cluster) {
        if (fat_get_entry(cluster) == FAT_FREE_CLUSTER) {
            *out_cluster = cluster;
            g_alloc_hint = cluster + 1;
            if (g_alloc_hint > max) g_alloc_hint = 2;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL fat_free_chain(UINT32 first_cluster) {
    UINT32 cluster = first_cluster;
    UINT32 iter = 0;
    UINT32 max_iter = fat_max_cluster();

    while (cluster >= 2 && iter <= max_iter) {
        UINT32 next = fat_next_cluster(cluster);
        if (!fat_set_entry(cluster, FAT_FREE_CLUSTER)) return FALSE;
        cluster = next;
        iter++;
    }
    return TRUE;
}

static BOOL fat_alloc_chain(UINT32 count, UINT32 *out_first_cluster) {
    UINT32 first = 0;
    UINT32 prev = 0;
    UINT32 i;

    if (!out_first_cluster) return FALSE;
    *out_first_cluster = 0;
    if (count == 0) return TRUE;
    if (g_fat_type == FAT_TYPE_FAT12) return FALSE;

    for (i = 0; i < count; ++i) {
        UINT32 cluster;
        if (!fat_find_free_cluster(&cluster)) {
            if (first >= 2) (void)fat_free_chain(first);
            return FALSE;
        }
        if (first == 0) first = cluster;
        if (!fat_set_entry(cluster, fat_eoc_marker())) {
            if (first >= 2) (void)fat_free_chain(first);
            return FALSE;
        }
        if (prev >= 2) {
            if (!fat_set_entry(prev, cluster)) {
                (void)fat_set_entry(cluster, FAT_FREE_CLUSTER);
                if (first >= 2) (void)fat_free_chain(first);
                return FALSE;
            }
        }
        prev = cluster;
    }

    if (prev >= 2) {
        if (!fat_set_entry(prev, fat_eoc_marker())) {
            if (first >= 2) (void)fat_free_chain(first);
            return FALSE;
        }
    }

    *out_first_cluster = first;
    return TRUE;
}

static BOOL fat_loc_to_lba(const FAT_DIR_LOC *loc, UINT32 *out_lba, UINT16 *out_off_in_sector) {
    if (!loc || !out_lba || !out_off_in_sector) return FALSE;

    if (loc->root && g_fat_type != FAT_TYPE_FAT32) {
        UINT16 sector = loc->root_index / (SECTOR_SIZE / 32);
        UINT16 off_in_sector = (loc->root_index % (SECTOR_SIZE / 32)) * 32;
        if (sector >= g_root_dir_sectors) return FALSE;
        *out_lba = g_root_start_lba + sector;
        *out_off_in_sector = off_in_sector;
        return TRUE;
    }

    if (!fat_cluster_is_valid(loc->cluster)) return FALSE;
    *out_lba = fat_cluster_to_lba(loc->cluster) + (loc->offset >> 9);
    *out_off_in_sector = (UINT16)(loc->offset & (SECTOR_SIZE - 1));
    return TRUE;
}

static BOOL fat_write_entry_at_loc(const FAT_DIR_LOC *loc, const FAT_DIR_RAW *raw) {
    UINT32 lba;
    UINT16 off;
    if (!loc || !raw) return FALSE;
    if (!fat_loc_to_lba(loc, &lba, &off)) return FALSE;
    if (!read_sector(lba, g_sector)) return FALSE;
    sima_memcpy(&g_sector[off], raw, sizeof(FAT_DIR_RAW));
    return write_sector(lba, g_sector);
}

static BOOL fat_read_entry_at_loc(const FAT_DIR_LOC *loc, FAT_DIR_RAW *out_raw) {
    UINT32 lba;
    UINT16 off;
    if (!loc || !out_raw) return FALSE;
    if (!fat_loc_to_lba(loc, &lba, &off)) return FALSE;
    if (!read_sector(lba, g_sector)) return FALSE;
    sima_memcpy(out_raw, &g_sector[off], sizeof(FAT_DIR_RAW));
    return TRUE;
}

static BOOL fat_dir_loc_next(const FAT_DIR_LOC *loc, FAT_DIR_LOC *out_next) {
    UINT32 cluster_bytes;
    if (!loc || !out_next) return FALSE;
    sima_memcpy(out_next, loc, (UINT16)sizeof(FAT_DIR_LOC));

    if (loc->root && g_fat_type != FAT_TYPE_FAT32) {
        if ((UINT16)(loc->root_index + 1) >= g_bpb.root_ents) return FALSE;
        out_next->root_index = (UINT16)(loc->root_index + 1);
        return TRUE;
    }

    cluster_bytes = (UINT32)1 << (g_spc_shift + 9);
    if (loc->offset + 32 < cluster_bytes) {
        out_next->offset = loc->offset + 32;
        return TRUE;
    }

    {
        UINT32 next_cluster = fat_next_cluster(loc->cluster);
        if (next_cluster < 2) return FALSE;
        out_next->cluster = next_cluster;
        out_next->offset = 0;
        return TRUE;
    }
}

static BOOL fat_find_in_dir_loc(UINT32 dir_cluster, const char *name, FAT_DIR_RAW *out_raw, FAT_DIR_LOC *out_loc) {
    FAT_DIR_RAW raw;
    UINT32 cluster;
    UINT32 cluster_bytes;
    UINT32 offset;
    UINT32 iter = 0;
    UINT32 max_iter = fat_max_cluster();

    if (!name || name[0] == '\0' || !out_raw || !out_loc) return FALSE;

    if (g_fat_type != FAT_TYPE_FAT32 && dir_cluster == 0) {
        UINT16 index;
        for (index = 0; index < g_bpb.root_ents; ++index) {
            if (!fat_read_root_entry(index, &raw)) return FALSE;
            if (raw.name[0] == 0x00) return FALSE;
            if (raw.name[0] == 0xE5) continue;
            if (raw.attr == FAT_ATTR_LFN) continue;
            if (fat_names_equal(&raw, name)) {
                sima_memcpy(out_raw, &raw, sizeof(FAT_DIR_RAW));
                sima_memclr((char*)out_loc, (UINT16)sizeof(FAT_DIR_LOC));
                out_loc->root = TRUE;
                out_loc->root_index = index;
                return TRUE;
            }
        }
        return FALSE;
    }

    if (!fat_cluster_is_valid(dir_cluster)) return FALSE;

    cluster = dir_cluster;
    cluster_bytes = (UINT32)1 << (g_spc_shift + 9);
    if (max_iter > 8192U) max_iter = 8192U;
    while (fat_cluster_is_valid(cluster) && iter <= max_iter) {
        for (offset = 0; offset < cluster_bytes; offset += 32) {
            if (!fat_read_cluster_entry(cluster, offset, &raw)) return FALSE;
            if (raw.name[0] == 0x00) return FALSE;
            if (raw.name[0] == 0xE5) continue;
            if (raw.attr == FAT_ATTR_LFN) continue;
            if (fat_names_equal(&raw, name)) {
                sima_memcpy(out_raw, &raw, sizeof(FAT_DIR_RAW));
                sima_memclr((char*)out_loc, (UINT16)sizeof(FAT_DIR_LOC));
                out_loc->root = FALSE;
                out_loc->cluster = cluster;
                out_loc->offset = offset;
                return TRUE;
            }
        }
        cluster = fat_next_cluster(cluster);
        ++iter;
    }
    return FALSE;
}

static BOOL fat_find_free_in_dir(UINT32 dir_cluster, FAT_DIR_LOC *out_loc, BOOL *out_is_end_marker) {
    FAT_DIR_LOC first_deleted;
    BOOL have_deleted = FALSE;
    UINT32 cluster;
    UINT32 iter = 0;
    UINT32 max_iter = fat_max_cluster();

    if (!out_loc) return FALSE;
    if (out_is_end_marker) *out_is_end_marker = FALSE;
    sima_memclr((char*)&first_deleted, (UINT16)sizeof(FAT_DIR_LOC));

    if (g_fat_type != FAT_TYPE_FAT32 && dir_cluster == 0) {
        /* Scan root directory sector-by-sector to avoid thousands of redundant reads. */
        UINT16 max_entries = g_bpb.root_ents;
        UINT16 sectors = (UINT16)g_root_dir_sectors;
        UINT16 sector_index;
        UINT16 entry_index;

        for (sector_index = 0; sector_index < sectors; ++sector_index) {
            if (!read_sector(g_root_start_lba + sector_index, g_sector)) return FALSE;
            for (entry_index = 0; entry_index < (SECTOR_SIZE / 32); ++entry_index) {
                UINT16 index = (UINT16)(sector_index * (SECTOR_SIZE / 32) + entry_index);
                FAT_DIR_RAW *raw = (FAT_DIR_RAW*)&g_sector[entry_index * 32];
                if (index >= max_entries) break;
                if (raw->name[0] == 0xE5) {
                    if (!have_deleted) {
                        have_deleted = TRUE;
                        first_deleted.root = TRUE;
                        first_deleted.root_index = index;
                    }
                    continue;
                }
                if (raw->name[0] == 0x00) {
                    if (have_deleted) {
                        sima_memcpy(out_loc, &first_deleted, (UINT16)sizeof(FAT_DIR_LOC));
                        return TRUE;
                    }
                    sima_memclr((char*)out_loc, (UINT16)sizeof(FAT_DIR_LOC));
                    out_loc->root = TRUE;
                    out_loc->root_index = index;
                    if (out_is_end_marker) *out_is_end_marker = TRUE;
                    return TRUE;
                }
            }
        }
        if (have_deleted) {
            sima_memcpy(out_loc, &first_deleted, (UINT16)sizeof(FAT_DIR_LOC));
            return TRUE;
        }
        return FALSE;
    }

    if (!fat_cluster_is_valid(dir_cluster)) return FALSE;

    cluster = dir_cluster;
    if (max_iter > 8192U) max_iter = 8192U;
    while (fat_cluster_is_valid(cluster) && iter <= max_iter) {
        UINT8 sec_index;
        UINT32 lba = fat_cluster_to_lba(cluster);
        for (sec_index = 0; sec_index < g_bpb.sec_per_clus; ++sec_index) {
            UINT16 entry_index;
            if (!read_sector(lba + sec_index, g_sector)) return FALSE;
            for (entry_index = 0; entry_index < (SECTOR_SIZE / 32); ++entry_index) {
                FAT_DIR_RAW *raw = (FAT_DIR_RAW*)&g_sector[entry_index * 32];
                UINT32 offset = ((UINT32)sec_index * (UINT32)SECTOR_SIZE) + ((UINT32)entry_index * 32U);

                if (raw->name[0] == 0xE5) {
                    if (!have_deleted) {
                        have_deleted = TRUE;
                        first_deleted.root = FALSE;
                        first_deleted.cluster = cluster;
                        first_deleted.offset = offset;
                    }
                    continue;
                }
                if (raw->name[0] == 0x00) {
                    if (have_deleted) {
                        sima_memcpy(out_loc, &first_deleted, (UINT16)sizeof(FAT_DIR_LOC));
                        return TRUE;
                    }
                    sima_memclr((char*)out_loc, (UINT16)sizeof(FAT_DIR_LOC));
                    out_loc->root = FALSE;
                    out_loc->cluster = cluster;
                    out_loc->offset = offset;
                    if (out_is_end_marker) *out_is_end_marker = TRUE;
                    return TRUE;
                }
            }
        }

        {
            UINT32 next = fat_next_cluster(cluster);
            if (next < 2) break;
            cluster = next;
        }
        ++iter;
    }

    if (have_deleted) {
        sima_memcpy(out_loc, &first_deleted, (UINT16)sizeof(FAT_DIR_LOC));
        return TRUE;
    }
    return FALSE;
}

static BOOL fat_extend_dir_chain(UINT32 dir_cluster, FAT_DIR_LOC *out_new_loc) {
    UINT32 last_cluster;
    UINT32 next_cluster;
    UINT32 new_cluster;
    UINT32 cluster_bytes = (UINT32)1 << (g_spc_shift + 9);
    UINT32 lba;
    UINT8 sec_index;
    UINT32 iter = 0;
    UINT32 max_iter = fat_max_cluster();

    if (!out_new_loc) return FALSE;
    if (g_fat_type == FAT_TYPE_FAT12) return FALSE;
    if (!fat_cluster_is_valid(dir_cluster)) return FALSE;

    last_cluster = dir_cluster;
    if (max_iter > 8192U) max_iter = 8192U;
    for (;;) {
        next_cluster = fat_next_cluster(last_cluster);
        if (next_cluster < 2) break;
        last_cluster = next_cluster;
        if (++iter > max_iter) return FALSE;
    }

    if (!fat_alloc_chain(1, &new_cluster)) return FALSE;
    if (!fat_cluster_is_valid(new_cluster)) return FALSE;

    if (!fat_set_entry(last_cluster, new_cluster)) {
        (void)fat_free_chain(new_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }
    if (!fat_set_entry(new_cluster, fat_eoc_marker())) {
        (void)fat_set_entry(last_cluster, fat_eoc_marker());
        (void)fat_free_chain(new_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }

    /* Zero out new directory cluster. */
    lba = fat_cluster_to_lba(new_cluster);
    for (sec_index = 0; sec_index < g_bpb.sec_per_clus; ++sec_index) {
        sima_memclr((char*)g_sector, SECTOR_SIZE);
        if (!write_sector(lba + sec_index, g_sector)) {
            (void)fat_set_entry(last_cluster, fat_eoc_marker());
            (void)fat_free_chain(new_cluster);
            (void)fat_flush_fat_sector();
            return FALSE;
        }
    }

    if (!fat_flush_fat_sector()) return FALSE;

    sima_memclr((char*)out_new_loc, (UINT16)sizeof(FAT_DIR_LOC));
    out_new_loc->root = FALSE;
    out_new_loc->cluster = new_cluster;
    out_new_loc->offset = 0;
    (void)cluster_bytes;
    return TRUE;
}

static BOOL fat_resolve_parent_dir(const char *path, UINT32 *out_dir_cluster, char *out_name, UINT16 out_name_cap) {
    const char *cursor;
    UINT32 current_cluster;
    FAT_DIR_RAW raw;
    char segment[FS_NAME_MAX];

    if (!out_dir_cluster || !out_name || out_name_cap == 0) return FALSE;
    if (!path || path[0] == '\0') return FALSE;

    cursor = path;
    if (path[0] == '/' || path[0] == '\\') {
        current_cluster = (g_fat_type == FAT_TYPE_FAT32) ? g_bpb.root_cluster : 0;
    } else {
        current_cluster = g_cwd_cluster;
    }

    cursor = fat_skip_separators(cursor);
    if (*cursor == '\0') return FALSE;

    while (fat_next_segment(&cursor, segment, (UINT16)sizeof(segment))) {
        cursor = fat_skip_separators(cursor);
        if (*cursor == '\0') {
            /* Last segment */
            if (segment[0] == '.' && segment[1] == '\0') return FALSE;
            if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') return FALSE;
            sima_memclr(out_name, out_name_cap);
            if (!sima_strcpy(out_name, out_name_cap, segment)) return FALSE;
            *out_dir_cluster = current_cluster;
            return TRUE;
        }

        /* Intermediate segment must resolve to directory. */
        if (segment[0] == '.' && segment[1] == '\0') continue;
        if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
            if (!fat_parent_cluster(current_cluster, &current_cluster)) return FALSE;
            continue;
        }

        if (!fat_find_in_dir(current_cluster, segment, &raw)) return FALSE;
        if (!(raw.attr & FAT_ATTR_DIRECTORY)) return FALSE;
        current_cluster = fat_entry_cluster(&raw);
        if (current_cluster == 0 && g_fat_type == FAT_TYPE_FAT32) return FALSE;
    }

    return FALSE;
}

static BOOL fat_write_cluster_chain(UINT32 first_cluster, const UINT8 *data, UINT32 size) {
    UINT32 cluster = first_cluster;
    UINT32 remaining = size;
    UINT32 cluster_bytes = (UINT32)1 << (g_spc_shift + 9);

    if (size == 0) return TRUE;
    if (!data) return FALSE;
    if (!fat_cluster_is_valid(cluster)) return FALSE;

    while (fat_cluster_is_valid(cluster)) {
        UINT32 sector_lba = fat_cluster_to_lba(cluster);
        UINT8 sector_index;
        for (sector_index = 0; sector_index < g_bpb.sec_per_clus; ++sector_index) {
            UINT32 copy_bytes = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            sima_memclr((char*)g_sector, SECTOR_SIZE);
            if (copy_bytes > 0) {
                sima_memcpy(g_sector, data, (UINT16)copy_bytes);
                data += copy_bytes;
                remaining -= copy_bytes;
            }
            if (!write_sector(sector_lba + sector_index, g_sector)) return FALSE;
            if (remaining == 0) break;
        }

        if (remaining == 0) break;
        (void)cluster_bytes;
        cluster = fat_next_cluster(cluster);
    }

    return (remaining == 0);
}

static BOOL fat_is_dir_empty(UINT32 dir_cluster) {
    UINT32 cluster;
    UINT32 iter = 0;
    UINT32 max_iter = fat_max_cluster();

    /* Never treat errors as \"empty\"; rmdir must be conservative. */
    if (!fat_cluster_is_valid(dir_cluster)) return FALSE;

    if (max_iter > 8192U) max_iter = 8192U;

    cluster = dir_cluster;
    while (fat_cluster_is_valid(cluster) && iter <= max_iter) {
        UINT32 lba = fat_cluster_to_lba(cluster);
        UINT8 sec_index;
        for (sec_index = 0; sec_index < g_bpb.sec_per_clus; ++sec_index) {
            UINT16 entry_index;
            if (!read_sector(lba + sec_index, g_sector)) return FALSE;
            for (entry_index = 0; entry_index < (SECTOR_SIZE / 32); ++entry_index) {
                FAT_DIR_RAW *raw = (FAT_DIR_RAW*)&g_sector[entry_index * 32];

                if (raw->name[0] == 0x00) return TRUE;  /* end marker */
                if (raw->name[0] == 0xE5) continue;     /* deleted */
                if (raw->attr == FAT_ATTR_LFN) return FALSE; /* conservative */

                /* Skip dot / dotdot */
                if (raw->name[0] == '.' && (raw->name[1] == ' ' || (raw->name[1] == '.' && raw->name[2] == ' '))) {
                    continue;
                }

                return FALSE; /* any other entry => not empty */
            }
        }

        cluster = fat_next_cluster(cluster);
        ++iter;
    }

    /* Corrupted chain (cycle) or unexpected termination: do not delete. */
    return FALSE;
}

static BOOL fat_find_in_dir(UINT32 dir_cluster, const char *name, FAT_DIR_RAW *out_raw) {
    FS_DIR dir;
    FAT_DIR_RAW raw;

    sima_memclr((void*)&dir, (UINT16)sizeof(FS_DIR));
    if (g_fat_type != FAT_TYPE_FAT32 && dir_cluster == 0) {
        dir.root = TRUE;
    } else {
        dir.root = FALSE;
        dir.cluster = dir_cluster;
        dir.cur_cluster = dir_cluster;
    }

    while (fat_dir_read_raw(&dir, &raw)) {
        if (fat_names_equal(&raw, name)) {
            sima_memcpy(out_raw, &raw, sizeof(FAT_DIR_RAW));
            return TRUE;
        }
    }
    return FALSE;
}

static UINT32 fat_entry_cluster(const FAT_DIR_RAW *raw) {
    return (UINT32)le16(raw->start_cluster_lo) | ((UINT32)le16(raw->start_cluster_hi) << 16);
}

static BOOL fat_is_root_cluster(UINT32 cluster) {
    if (g_fat_type == FAT_TYPE_FAT32) {
        return cluster == g_bpb.root_cluster;
    }
    return cluster == 0;
}

static BOOL fat_parent_cluster(UINT32 cluster, UINT32 *out_parent) {
    FAT_DIR_RAW raw;
    UINT32 parent;

    if (!out_parent) return FALSE;
    if (fat_is_root_cluster(cluster)) {
        *out_parent = (g_fat_type == FAT_TYPE_FAT32) ? g_bpb.root_cluster : 0;
        return TRUE;
    }

    if (!fat_find_in_dir(cluster, "..", &raw)) return FALSE;
    if (!(raw.attr & FAT_ATTR_DIRECTORY)) return FALSE;
    parent = fat_entry_cluster(&raw);
    if (parent == 0 && g_fat_type == FAT_TYPE_FAT32) {
        parent = g_bpb.root_cluster;
    }
    *out_parent = parent;
    return TRUE;
}

static const char *fat_skip_separators(const char *cursor) {
    if (!cursor) return cursor;
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    return cursor;
}

static BOOL fat_next_segment(const char **path_cursor, char *segment, UINT16 segment_cap) {
    UINT16 segment_len = 0;
    const char *cursor;

    if (!path_cursor || !segment || segment_cap == 0) return FALSE;
    cursor = *path_cursor;
    if (!cursor) return FALSE;

    cursor = fat_skip_separators(cursor);
    if (*cursor == '\0') {
        *path_cursor = cursor;
        return FALSE;
    }

    while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
        if (segment_len + 1 >= segment_cap) return FALSE;
        segment[segment_len++] = *cursor;
        cursor++;
    }

    segment[segment_len] = '\0';
    *path_cursor = cursor;
    return TRUE;
}

static BOOL fat_resolve_path(const char *path, FAT_DIR_RAW *out_raw) {
    const char *cursor;
    UINT32 current_cluster;
    FAT_DIR_RAW raw;
    char segment[FS_NAME_MAX];

    if (!path || path[0] == '\0') return FALSE;

    cursor = path;
    if (path[0] == '/' || path[0] == '\\') {
        current_cluster = (g_fat_type == FAT_TYPE_FAT32) ? g_bpb.root_cluster : 0;
    } else {
        current_cluster = g_cwd_cluster;
    }

    while (fat_next_segment(&cursor, segment, (UINT16)sizeof(segment))) {
        if (segment[0] == '.' && segment[1] == '\0') {
            cursor = fat_skip_separators(cursor);
            continue;
        }
        if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
            if (!fat_parent_cluster(current_cluster, &current_cluster)) return FALSE;
            cursor = fat_skip_separators(cursor);
            continue;
        }
        if (!fat_find_in_dir(current_cluster, segment, &raw)) return FALSE;
        cursor = fat_skip_separators(cursor);
        if (*cursor == '\0') {
            if (out_raw) sima_memcpy(out_raw, &raw, sizeof(FAT_DIR_RAW));
            return TRUE;
        }
        if (!(raw.attr & FAT_ATTR_DIRECTORY)) return FALSE;
        current_cluster = fat_entry_cluster(&raw);
        if (current_cluster == 0 && g_fat_type == FAT_TYPE_FAT32) return FALSE;
    }

    return FALSE;
}

static BOOL fat_resolve_dir(const char *path, UINT32 *out_cluster) {
    const char *cursor;
    UINT32 current_cluster;
    FAT_DIR_RAW raw;
    UINT32 target_cluster;
    char segment[FS_NAME_MAX];

    if (!out_cluster) return FALSE;
    if (!path || path[0] == '\0') {
        *out_cluster = g_cwd_cluster;
        return TRUE;
    }

    cursor = path;
    if (path[0] == '/' || path[0] == '\\') {
        current_cluster = (g_fat_type == FAT_TYPE_FAT32) ? g_bpb.root_cluster : 0;
    } else {
        current_cluster = g_cwd_cluster;
    }

    cursor = fat_skip_separators(cursor);
    if (*cursor == '\0') {
        *out_cluster = current_cluster;
        return TRUE;
    }

    while (fat_next_segment(&cursor, segment, (UINT16)sizeof(segment))) {
        if (segment[0] == '.' && segment[1] == '\0') {
            cursor = fat_skip_separators(cursor);
            continue;
        }
        if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
            if (!fat_parent_cluster(current_cluster, &current_cluster)) return FALSE;
            cursor = fat_skip_separators(cursor);
            continue;
        }

        if (!fat_find_in_dir(current_cluster, segment, &raw)) return FALSE;
        if (!(raw.attr & FAT_ATTR_DIRECTORY)) return FALSE;

        target_cluster = fat_entry_cluster(&raw);
        if (g_fat_type == FAT_TYPE_FAT32) {
            if (!fat_cluster_is_valid(target_cluster)) return FALSE;
        } else {
            if (target_cluster < 2) return FALSE;
        }
        current_cluster = target_cluster;
        cursor = fat_skip_separators(cursor);
    }

    *out_cluster = current_cluster;
    return TRUE;
}

static BOOL fat_is_exec_entry(const FAT_DIR_RAW *raw) {
    if (!raw) return FALSE;
    if (to_upper(raw->ext[0]) != 'P') return FALSE;
    if (to_upper(raw->ext[1]) != 'R') return FALSE;
    if (to_upper(raw->ext[2]) != 'G') return FALSE;
    return TRUE;
}

static BOOL fat_read_file_entry(const FAT_DIR_RAW *raw, UINT8 *out, UINT32 out_cap, UINT32 *out_size) {
    UINT32 cluster;
    UINT32 size;
    UINT32 file_size;
    UINT32 read_bytes = 0;
    UINT32 cluster_bytes = (UINT32)1 << (g_spc_shift + 9);

    if (!raw || !out) return FALSE;
    cluster = fat_entry_cluster(raw);
    size = le32(raw->size);
    file_size = size;
    if (out_size) *out_size = 0;
    if (size > 0 && !fat_cluster_is_valid(cluster)) return FALSE;

    while (size > 0 && fat_cluster_is_valid(cluster)) {
        UINT32 chunk = (size > cluster_bytes) ? cluster_bytes : size;
        if (read_bytes >= out_cap) break;
        if (chunk > (out_cap - read_bytes)) chunk = out_cap - read_bytes;
        if (chunk == 0) break;

        {
            UINT32 lba = fat_cluster_to_lba(cluster);
            UINT32 sector_count = (chunk + SECTOR_SIZE - 1) / SECTOR_SIZE;
            UINT32 sector_index;
            for (sector_index = 0; sector_index < sector_count; ++sector_index) {
                UINT32 copy_bytes = SECTOR_SIZE;
                if (!read_sector(lba + sector_index, g_sector)) return FALSE;
                if (sector_index * SECTOR_SIZE + copy_bytes > chunk) {
                    copy_bytes = chunk - sector_index * SECTOR_SIZE;
                }
                sima_memcpy(out + read_bytes + sector_index * SECTOR_SIZE, g_sector, (UINT16)copy_bytes);
            }
        }

        read_bytes += chunk;
        size -= chunk;
        if (size > 0 && chunk < cluster_bytes) break;
        cluster = fat_next_cluster(cluster);
    }

    if (out_size) *out_size = read_bytes;
    if (out_cap >= file_size && read_bytes < file_size) return FALSE;
    return TRUE;
}

/* Public API */

BOOL fs_init(void) {
    if (!fat_load_bpb()) return FALSE;
    return TRUE;
}

BOOL fs_cd(const char *path) {
    UINT32 target_cluster;
    char new_path[FS_PATH_MAX];
    const char *base_for_rel;
    
    if (!path || path[0] == '\0' || (path[0] == '.' && path[1] == '\0')) {
        /* No-op */
        return TRUE;
    }

    if (path[0] == '/' && path[1] == 0) {
        if (g_fat_type == FAT_TYPE_FAT32) {
            g_cwd_cluster = g_bpb.root_cluster;
        } else {
            g_cwd_cluster = 0;
        }
        sima_strcpy(g_cwd_path, FS_PATH_MAX, "/");
        return TRUE;
    }

    if (!fat_resolve_dir(path, &target_cluster)) return FALSE;
    base_for_rel = g_cwd_path;
    if (!fs_normalize_path(base_for_rel, path, new_path, (UINT16)sizeof(new_path))) {
        /* Keep cluster change, but fall back to root-style prompt if path is too deep/long. */
        sima_strcpy(new_path, (UINT16)sizeof(new_path), "/");
    }
    g_cwd_cluster = target_cluster;
    sima_strcpy(g_cwd_path, FS_PATH_MAX, new_path);
    return TRUE;
}

BOOL fs_get_cwd(char *out, UINT16 out_cap) {
    if (!g_cwd_path[0]) {
        sima_strcpy(out, out_cap, "/");
    } else {
        sima_strcpy(out, out_cap, g_cwd_path);
    }
    return TRUE;
}

BOOL fs_dir_open(const char *path, FS_DIR *out_dir) {
    UINT32 target_cluster;
    if (!out_dir) return FALSE;
    sima_memclr((void*)out_dir, (UINT16)sizeof(FS_DIR));

    if (path == NULL || (path[0] == '/' && path[1] == 0) || (path[0] == '.' && path[1] == 0)) {
        /* Open Current / Root */
        if (path && path[0] == '/') {
             if (g_fat_type == FAT_TYPE_FAT32) {
                 out_dir->root = FALSE;
                 out_dir->cluster = g_bpb.root_cluster;
                 out_dir->cur_cluster = g_bpb.root_cluster;
             } else {
                 out_dir->root = TRUE;
             }
        } else {
             if (g_fat_type != FAT_TYPE_FAT32 && g_cwd_cluster == 0) {
                 out_dir->root = TRUE;
             } else {
                 out_dir->root = FALSE;
                 out_dir->cluster = g_cwd_cluster;
                 out_dir->cur_cluster = g_cwd_cluster;
             }
        }
        return TRUE;
    }
    
    if (!fat_resolve_dir(path, &target_cluster)) return FALSE;
    if (g_fat_type != FAT_TYPE_FAT32 && target_cluster == 0) {
        out_dir->root = TRUE;
        out_dir->root_index = 0;
        return TRUE;
    }
    out_dir->root = FALSE;
    out_dir->cluster = target_cluster;
    out_dir->cur_cluster = target_cluster;
    out_dir->offset = 0;
    return TRUE;
}

BOOL fs_dir_read(FS_DIR *dir, FS_DIRENT *out) {
    FAT_DIR_RAW raw;
    if (!fat_dir_read_raw(dir, &raw)) return FALSE;

    fat_format_name(&raw, out->name, FS_NAME_MAX);
    out->size = le32(raw.size);
    out->first_cluster = fat_entry_cluster(&raw);
    out->is_dir = (raw.attr & FAT_ATTR_DIRECTORY) ? TRUE : FALSE;
    out->is_volume = (raw.attr & FAT_ATTR_VOLUME) ? TRUE : FALSE;
    out->is_hidden = (raw.attr & FAT_ATTR_HIDDEN) ? TRUE : FALSE;
    out->is_system = (raw.attr & FAT_ATTR_SYSTEM) ? TRUE : FALSE;
    out->is_exec = fat_is_exec_entry(&raw);
    
    return TRUE;
}

BOOL fs_read(const char *path, UINT8 *out, UINT32 out_cap, UINT32 *out_size) {
    FAT_DIR_RAW raw;
    if (!fat_resolve_path(path, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;
    return fat_read_file_entry(&raw, out, out_cap, out_size);
}

/* Stubs for write operations */
BOOL fs_format(void) { return FALSE; }
static BOOL fat_make_dir_entry(const char *name, UINT8 attr, UINT32 first_cluster, UINT32 size, FAT_DIR_RAW *out_raw) {
    UINT8 name83[11];
    if (!out_raw) return FALSE;
    if (!fat_build_83(name, name83)) return FALSE;
    sima_memclr((char*)out_raw, (UINT16)sizeof(FAT_DIR_RAW));
    sima_memcpy(out_raw->name, name83, 8);
    sima_memcpy(out_raw->ext, &name83[8], 3);
    out_raw->attr = attr;
    wr16(out_raw->start_cluster_lo, (UINT16)(first_cluster & 0xFFFF));
    wr16(out_raw->start_cluster_hi, (UINT16)((first_cluster >> 16) & 0xFFFF));
    wr32(out_raw->size, size);
    return TRUE;
}

static BOOL fat_write_end_marker_after_loc(const FAT_DIR_LOC *loc) {
    FAT_DIR_LOC next;
    FAT_DIR_RAW zero;

    if (!loc) return FALSE;
    if (!fat_dir_loc_next(loc, &next)) return TRUE;
    sima_memclr((char*)&zero, (UINT16)sizeof(FAT_DIR_RAW));
    return fat_write_entry_at_loc(&next, &zero);
}

static BOOL fat_write_file_in_dir(UINT32 dir_cluster, const char *name, const UINT8 *data, UINT32 size) {
    FAT_DIR_RAW existing;
    FAT_DIR_RAW entry;
    FAT_DIR_LOC loc;
    BOOL exists;
    BOOL end_marker = FALSE;
    UINT32 old_cluster = 0;
    UINT32 new_cluster = 0;
    UINT32 cluster_bytes;
    UINT32 cluster_count;

    if (!name || name[0] == '\0') return FALSE;
    if (g_fat_type == FAT_TYPE_FAT12) return FALSE;

    exists = fat_find_in_dir_loc(dir_cluster, name, &existing, &loc);
    if (exists) {
        if (existing.attr & FAT_ATTR_DIRECTORY) return FALSE;
        old_cluster = fat_entry_cluster(&existing);
    } else {
        if (!fat_find_free_in_dir(dir_cluster, &loc, &end_marker)) {
            if (g_fat_type != FAT_TYPE_FAT32 && dir_cluster == 0) return FALSE;
            if (!fat_extend_dir_chain(dir_cluster, &loc)) return FALSE;
            end_marker = TRUE;
        }
    }

    if (size == 0) {
        new_cluster = 0;
    } else {
        UINT8 shift;
        UINT32 mask;
        if (!data) return FALSE;
        shift = (UINT8)(g_spc_shift + 9);
        mask = ((UINT32)1 << shift) - 1;
        cluster_bytes = (UINT32)1 << shift;
        cluster_count = (size + mask) >> shift;
        if (!fat_alloc_chain(cluster_count, &new_cluster)) return FALSE;
        if (!fat_write_cluster_chain(new_cluster, data, size)) {
            (void)fat_free_chain(new_cluster);
            (void)fat_flush_fat_sector();
            return FALSE;
        }
    }

    if (!fat_make_dir_entry(name, FAT_ATTR_ARCHIVE, new_cluster, size, &entry)) {
        if (new_cluster >= 2) (void)fat_free_chain(new_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }
    if (!fat_write_entry_at_loc(&loc, &entry)) {
        if (new_cluster >= 2) (void)fat_free_chain(new_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }
    if (end_marker) {
        if (!fat_write_end_marker_after_loc(&loc)) return FALSE;
    }
    if (!fat_flush_fat_sector()) return FALSE;

    if (exists && old_cluster >= 2) {
        if (!fat_free_chain(old_cluster)) return FALSE;
        if (!fat_flush_fat_sector()) return FALSE;
    }

    return TRUE;
}

static BOOL fat_init_directory_cluster(UINT32 dir_cluster, UINT32 parent_cluster) {
    UINT32 lba;
    UINT8 sec_index;
    FAT_DIR_RAW dot;
    FAT_DIR_RAW dotdot;
    UINT32 dotdot_cluster;

    if (dir_cluster < 2) return FALSE;
    dotdot_cluster = parent_cluster;
    if (fat_is_root_cluster(parent_cluster)) {
        dotdot_cluster = 0;
    }

    if (!fat_make_dir_entry(".", FAT_ATTR_DIRECTORY, dir_cluster, 0, &dot)) return FALSE;
    if (!fat_make_dir_entry("..", FAT_ATTR_DIRECTORY, dotdot_cluster, 0, &dotdot)) return FALSE;

    lba = fat_cluster_to_lba(dir_cluster);
    for (sec_index = 0; sec_index < g_bpb.sec_per_clus; ++sec_index) {
        sima_memclr((char*)g_sector, SECTOR_SIZE);
        if (sec_index == 0) {
            sima_memcpy(&g_sector[0], &dot, sizeof(FAT_DIR_RAW));
            sima_memcpy(&g_sector[32], &dotdot, sizeof(FAT_DIR_RAW));
        }
        if (!write_sector(lba + sec_index, g_sector)) return FALSE;
    }

    return TRUE;
}

static BOOL fat_create_dir_in_dir(UINT32 parent_cluster, const char *name) {
    FAT_DIR_RAW entry;
    FAT_DIR_RAW existing;
    FAT_DIR_LOC loc;
    BOOL end_marker = FALSE;
    UINT32 dir_cluster;

    if (!name || name[0] == '\0') return FALSE;
    if (g_fat_type == FAT_TYPE_FAT12) return FALSE;

    if (fat_find_in_dir_loc(parent_cluster, name, &existing, &loc)) return FALSE;
    if (!fat_find_free_in_dir(parent_cluster, &loc, &end_marker)) {
        if (g_fat_type != FAT_TYPE_FAT32 && parent_cluster == 0) return FALSE;
        if (!fat_extend_dir_chain(parent_cluster, &loc)) return FALSE;
        end_marker = TRUE;
    }

    if (!fat_alloc_chain(1, &dir_cluster)) return FALSE;
    if (dir_cluster < 2) return FALSE;
    if (!fat_init_directory_cluster(dir_cluster, parent_cluster)) {
        (void)fat_free_chain(dir_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }

    if (!fat_make_dir_entry(name, FAT_ATTR_DIRECTORY, dir_cluster, 0, &entry)) {
        (void)fat_free_chain(dir_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }
    if (!fat_write_entry_at_loc(&loc, &entry)) {
        (void)fat_free_chain(dir_cluster);
        (void)fat_flush_fat_sector();
        return FALSE;
    }
    if (end_marker) {
        if (!fat_write_end_marker_after_loc(&loc)) return FALSE;
    }
    return fat_flush_fat_sector();
}

BOOL fs_create_dir(const char *name) {
    UINT32 dir_cluster;
    char leaf[FS_NAME_MAX];

    if (!fat_resolve_parent_dir(name, &dir_cluster, leaf, (UINT16)sizeof(leaf))) return FALSE;
    return fat_create_dir_in_dir(dir_cluster, leaf);
}

BOOL fs_write(const char *name, const UINT8 *data, UINT32 size) {
    UINT32 dir_cluster;
    char leaf[FS_NAME_MAX];

    if (!fat_resolve_parent_dir(name, &dir_cluster, leaf, (UINT16)sizeof(leaf))) return FALSE;
    return fat_write_file_in_dir(dir_cluster, leaf, data, size);
}

BOOL fs_write_in_dir(const char *dir_name, const char *name, const UINT8 *data, UINT32 size) {
    UINT32 dir_cluster;
    if (!dir_name || !name) return FALSE;
    if (!fat_resolve_dir(dir_name, &dir_cluster)) return FALSE;
    return fat_write_file_in_dir(dir_cluster, name, data, size);
}

BOOL fs_delete(const char *name) {
    UINT32 dir_cluster;
    char leaf[FS_NAME_MAX];
    FAT_DIR_RAW raw;
    FAT_DIR_LOC loc;
    UINT32 cluster;

    if (!fat_resolve_parent_dir(name, &dir_cluster, leaf, (UINT16)sizeof(leaf))) return FALSE;
    if (!fat_find_in_dir_loc(dir_cluster, leaf, &raw, &loc)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;

    cluster = fat_entry_cluster(&raw);
    if (!fat_read_entry_at_loc(&loc, &raw)) return FALSE;
    raw.name[0] = 0xE5;
    if (!fat_write_entry_at_loc(&loc, &raw)) return FALSE;
    if (!fat_flush_fat_sector()) return FALSE;
    if (cluster >= 2) {
        if (!fat_free_chain(cluster)) return FALSE;
        if (!fat_flush_fat_sector()) return FALSE;
    }
    return TRUE;
}

BOOL fs_delete_dir(const char *name) {
    UINT32 parent_cluster;
    char leaf[FS_NAME_MAX];
    FAT_DIR_RAW raw;
    FAT_DIR_LOC loc;
    UINT32 dir_cluster;

    if (!fat_resolve_parent_dir(name, &parent_cluster, leaf, (UINT16)sizeof(leaf))) return FALSE;
    if (!fat_find_in_dir_loc(parent_cluster, leaf, &raw, &loc)) return FALSE;
    if (!(raw.attr & FAT_ATTR_DIRECTORY)) return FALSE;

    dir_cluster = fat_entry_cluster(&raw);
    if (dir_cluster < 2) return FALSE;
    if (fat_is_root_cluster(dir_cluster)) return FALSE;
    if (dir_cluster == g_cwd_cluster) return FALSE;

    if (!fat_is_dir_empty(dir_cluster)) return FALSE;

    if (!fat_read_entry_at_loc(&loc, &raw)) return FALSE;
    raw.name[0] = 0xE5;
    if (!fat_write_entry_at_loc(&loc, &raw)) return FALSE;
    if (!fat_flush_fat_sector()) return FALSE;

    if (!fat_free_chain(dir_cluster)) return FALSE;
    if (!fat_flush_fat_sector()) return FALSE;
    return TRUE;
}

BOOL fs_sync(void) { return fat_flush_fat_sector(); }

BOOL fs_get_volume_info(UINT32 *out_total_sectors, UINT8 *out_sec_per_clus) {
    if (out_total_sectors) *out_total_sectors = g_total_sectors;
    if (out_sec_per_clus) *out_sec_per_clus = g_bpb.sec_per_clus;
    return TRUE;
}

BOOL fs_get_volume_info32(UINT32 *out_total_sectors, UINT8 *out_sec_per_clus, UINT32 *out_base_lba) {
    if (out_total_sectors) *out_total_sectors = g_total_sectors;
    if (out_sec_per_clus) *out_sec_per_clus = g_bpb.sec_per_clus;
    if (out_base_lba) *out_base_lba = g_base_lba;
    return TRUE;
}

/* Helper stubs */
BOOL fs_read_in_dir(const char *dir_name, const char *name, UINT8 *out, UINT32 out_cap, UINT32 *out_size) {
    UINT32 dir_cluster;
    FAT_DIR_RAW raw;

    if (!dir_name || !name) return FALSE;
    if (!fat_resolve_dir(dir_name, &dir_cluster)) return FALSE;
    if (!fat_find_in_dir(dir_cluster, name, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;
    return fat_read_file_entry(&raw, out, out_cap, out_size);
}

BOOL fs_is_executable(const char *path) {
    FAT_DIR_RAW raw;
    if (!fat_resolve_path(path, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;
    return fat_is_exec_entry(&raw);
}

BOOL fs_is_executable_in_dir(const char *dir_name, const char *name) {
    UINT32 dir_cluster;
    FAT_DIR_RAW raw;

    if (!dir_name || !name) return FALSE;
    if (!fat_resolve_dir(dir_name, &dir_cluster)) return FALSE;
    if (!fat_find_in_dir(dir_cluster, name, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;
    return fat_is_exec_entry(&raw);
}

BOOL fs_is_dir(const char *path) { 
    UINT32 cluster;
    return fat_resolve_dir(path, &cluster);
}
