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

#define FAT12_EOC          0x0FF8
#define FAT12_EOC_VALUE    0x0FFF

#define FAT_CACHE_MAX_BYTES 8192
#define SECTOR_SIZE         512

typedef struct {
    UINT16 bytes_per_sec;
    UINT8  sec_per_clus;
    UINT16 res_sectors;
    UINT8  fats;
    UINT16 root_ents;
    UINT16 total_sectors16;
    UINT8  media;
    UINT16 fat_secs;
    UINT16 sec_per_track;
    UINT16 heads;
    UINT32 hidden_sectors;
    UINT32 total_sectors32;
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
    UINT16 root_index; /* valid if root == TRUE */
    UINT16 cluster;    /* valid if root == FALSE */
    UINT16 offset;     /* byte offset within cluster if root == FALSE */
} FAT_DIR_LOC;

static FAT_BPB g_bpb;
static UINT16 g_root_dir_sectors;
static UINT16 g_fat_start_lba;
static UINT16 g_root_start_lba;
static UINT16 g_data_start_lba;
static UINT16 g_total_sectors;
static UINT16 g_fat_bytes;
static UINT8  g_fat_cache[FAT_CACHE_MAX_BYTES];
static UINT8  g_sector[SECTOR_SIZE];
static UINT16 g_max_cluster;
static BOOL   g_fat_dirty = FALSE;

static UINT16 g_cwd_cluster = 0; /* 0 = root */
static char   g_cwd_path[FS_PATH_MAX] = "/";

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

static BOOL fat12_load_bpb(void) {
    if (!read_sector(0, g_sector)) return FALSE;

    g_bpb.bytes_per_sec = le16(&g_sector[0x0B]);
    g_bpb.sec_per_clus = g_sector[0x0D];
    g_bpb.res_sectors = le16(&g_sector[0x0E]);
    g_bpb.fats = g_sector[0x10];
    g_bpb.root_ents = le16(&g_sector[0x11]);
    g_bpb.total_sectors16 = le16(&g_sector[0x13]);
    g_bpb.media = g_sector[0x15];
    g_bpb.fat_secs = le16(&g_sector[0x16]);
    g_bpb.sec_per_track = le16(&g_sector[0x18]);
    g_bpb.heads = le16(&g_sector[0x1A]);
    g_bpb.hidden_sectors = le32(&g_sector[0x1C]);
    g_bpb.total_sectors32 = le32(&g_sector[0x20]);

    if (g_bpb.bytes_per_sec != SECTOR_SIZE) return FALSE;
    if (g_bpb.sec_per_clus == 0 || g_bpb.fat_secs == 0) return FALSE;

    g_total_sectors = g_bpb.total_sectors16 ? g_bpb.total_sectors16 : (UINT16)g_bpb.total_sectors32;

    /* root_dir_sectors = ceil(root_ents * 32 / 512) = ceil(root_ents / 16) */
    g_root_dir_sectors = (UINT16)((g_bpb.root_ents + 15U) / 16U);
    g_fat_start_lba = g_bpb.res_sectors;
    g_root_start_lba = (UINT16)(g_fat_start_lba + (UINT16)g_bpb.fats * g_bpb.fat_secs);
    g_data_start_lba = (UINT16)(g_root_start_lba + g_root_dir_sectors);

    g_fat_bytes = (UINT16)((UINT16)g_bpb.fat_secs << 9);
    if (g_fat_bytes > FAT_CACHE_MAX_BYTES) return FALSE;

    {
        UINT16 data_sectors;
        UINT16 cluster_count;

        if (g_total_sectors < g_data_start_lba) return FALSE;
        data_sectors = (UINT16)(g_total_sectors - g_data_start_lba);
        if (data_sectors == 0) return FALSE;

        cluster_count = (UINT16)(data_sectors / (UINT16)g_bpb.sec_per_clus);
        if (cluster_count == 0) return FALSE;

        g_max_cluster = (UINT16)(cluster_count + 1U);

        /* Ensure cluster byte size fits in UINT16 (we use 16-bit offsets). */
        if (((UINT16)g_bpb.sec_per_clus << 9) == 0) return FALSE;
    }

    return TRUE;
}

static BOOL fat12_load_fat(void) {
    UINT16 i;
    for (i = 0; i < g_bpb.fat_secs; ++i) {
        if (!read_sector((UINT32)(g_fat_start_lba + i), &g_fat_cache[i * SECTOR_SIZE])) return FALSE;
    }
    g_fat_dirty = FALSE;
    return TRUE;
}

static UINT16 fat12_next_cluster(UINT16 cluster) {
    UINT16 offset = (UINT16)(cluster + (cluster / 2));
    UINT16 value;
    if (offset + 1 >= g_fat_bytes) return 0xFFFF;

    if (cluster & 1) {
        value = (UINT16)((g_fat_cache[offset] >> 4) | (g_fat_cache[offset + 1] << 4));
    } else {
        value = (UINT16)(g_fat_cache[offset] | ((g_fat_cache[offset + 1] & 0x0F) << 8));
    }
    return (UINT16)(value & 0x0FFF);
}

static BOOL fat12_set_entry(UINT16 cluster, UINT16 value) {
    UINT16 offset;
    UINT16 v;

    if (cluster < 2 || cluster > g_max_cluster) return FALSE;
    offset = (UINT16)(cluster + (cluster / 2));
    if (offset + 1 >= g_fat_bytes) return FALSE;

    v = (UINT16)(value & 0x0FFF);

    if (cluster & 1) {
        g_fat_cache[offset] = (UINT8)((g_fat_cache[offset] & 0x0F) | ((v << 4) & 0xF0));
        g_fat_cache[offset + 1] = (UINT8)((v >> 4) & 0xFF);
    } else {
        g_fat_cache[offset] = (UINT8)(v & 0xFF);
        g_fat_cache[offset + 1] = (UINT8)((g_fat_cache[offset + 1] & 0xF0) | ((v >> 8) & 0x0F));
    }

    g_fat_dirty = TRUE;
    return TRUE;
}

static BOOL fat12_flush_fat(void) {
    UINT16 fat_index;
    UINT16 sec;
    UINT16 lba_base;

    if (!g_fat_dirty) return TRUE;

    for (fat_index = 0; fat_index < (UINT16)g_bpb.fats; ++fat_index) {
        lba_base = (UINT16)(g_fat_start_lba + (UINT16)(fat_index * g_bpb.fat_secs));
        for (sec = 0; sec < g_bpb.fat_secs; ++sec) {
            if (!write_sector((UINT32)(lba_base + sec), &g_fat_cache[sec * SECTOR_SIZE])) return FALSE;
        }
    }

    g_fat_dirty = FALSE;
    return TRUE;
}

static UINT16 fat12_cluster_to_lba(UINT16 cluster);
static BOOL fat12_free_chain(UINT16 first);

static BOOL fat12_clear_cluster(UINT16 cluster) {
    UINT16 i;
    UINT32 lba;

    if (cluster < 2 || cluster > g_max_cluster) return FALSE;
    lba = (UINT32)fat12_cluster_to_lba(cluster);

    sima_memset(g_sector, 0, SECTOR_SIZE);
    for (i = 0; i < g_bpb.sec_per_clus; ++i) {
        if (!write_sector(lba + i, g_sector)) return FALSE;
    }
    return TRUE;
}

static UINT16 fat12_find_free_cluster(void) {
    UINT16 c;
    for (c = 2; c <= g_max_cluster; ++c) {
        UINT16 v = fat12_next_cluster(c);
        if (v == 0x000) return c;
    }
    return 0;
}

static BOOL fat12_alloc_chain(UINT16 count, UINT16 *out_first) {
    UINT16 first = 0;
    UINT16 prev = 0;
    UINT16 i;

    if (!out_first) return FALSE;
    *out_first = 0;
    if (count == 0) return TRUE;

    for (i = 0; i < count; ++i) {
        UINT16 c = fat12_find_free_cluster();
        if (c == 0) {
            if (first) {
                /* Best-effort rollback; if this fails we still report failure. */
                (void)fat12_free_chain(first);
            }
            return FALSE;
        }

        if (!fat12_set_entry(c, FAT12_EOC_VALUE)) {
            if (first) (void)fat12_free_chain(first);
            return FALSE;
        }
        if (prev) {
            if (!fat12_set_entry(prev, c)) {
                if (first) (void)fat12_free_chain(first);
                return FALSE;
            }
        } else {
            first = c;
        }
        prev = c;
    }

    if (!fat12_flush_fat()) {
        if (first) (void)fat12_free_chain(first);
        return FALSE;
    }

    *out_first = first;
    return TRUE;
}

static BOOL fat12_free_chain(UINT16 first) {
    UINT16 cluster = first;

    if (cluster < 2) return TRUE;
    while (cluster >= 2 && cluster < FAT12_EOC) {
        UINT16 next = fat12_next_cluster(cluster);
        if (!fat12_set_entry(cluster, 0x000)) return FALSE;
        if (next == 0x000 || next == 0xFFFF || next == 0xFF7) break;
        cluster = next;
    }
    return fat12_flush_fat();
}

static UINT16 fat12_cluster_to_lba(UINT16 cluster) {
    return (UINT16)(g_data_start_lba + (UINT16)(cluster - 2) * g_bpb.sec_per_clus);
}

static void fat12_format_name(const FAT_DIR_RAW *raw, char *out, UINT16 out_cap) {
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

static BOOL fat12_build_83(const char *input, UINT8 out[11]) {
    UINT16 i = 0;
    UINT16 base_len = 0;
    UINT16 ext_len = 0;
    BOOL in_ext = FALSE;

    if (!input || input[0] == '\0') return FALSE;
    for (i = 0; i < 11; ++i) out[i] = ' ';

    i = 0;
    while (input[i] != '\0') {
        char ch = input[i];
        if (ch == '/' || ch == '\\') break;
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

static BOOL fat12_names_equal(const FAT_DIR_RAW *raw, const char *input) {
    UINT8 name83[11];
    UINT16 i;
    if (!fat12_build_83(input, name83)) return FALSE;
    for (i = 0; i < 8; ++i) {
        if (to_upper(raw->name[i]) != name83[i]) return FALSE;
    }
    for (i = 0; i < 3; ++i) {
        if (to_upper(raw->ext[i]) != name83[8 + i]) return FALSE;
    }
    return TRUE;
}

static BOOL fat12_read_root_entry(UINT16 index, FAT_DIR_RAW *out) {
    UINT16 sector = (UINT16)(index >> 4);              /* 16 entries per sector */
    UINT16 off_in_sector = (UINT16)((index & 0x0F) << 5); /* 32 bytes per entry */

    if (index >= g_bpb.root_ents) return FALSE;
    if (!read_sector((UINT32)(g_root_start_lba + sector), g_sector)) return FALSE;
    sima_memcpy(out, &g_sector[off_in_sector], (UINT16)sizeof(FAT_DIR_RAW));
    return TRUE;
}

static BOOL fat12_read_cluster_entry(UINT16 cluster, UINT16 offset, FAT_DIR_RAW *out) {
    UINT16 lba = (UINT16)(fat12_cluster_to_lba(cluster) + (offset / SECTOR_SIZE));
    UINT16 off_in_sector = (UINT16)(offset % SECTOR_SIZE);

    if (!read_sector(lba, g_sector)) return FALSE;
    sima_memcpy(out, &g_sector[off_in_sector], (UINT16)sizeof(FAT_DIR_RAW));
    return TRUE;
}

static BOOL fat12_dir_read_raw(FS_DIR *dir, FAT_DIR_RAW *out) {
    while (TRUE) {
        if (dir->root) {
            if (dir->root_index >= g_bpb.root_ents) return FALSE;
            if (!fat12_read_root_entry(dir->root_index, out)) return FALSE;
            dir->root_index = (UINT16)(dir->root_index + 1);
        } else {
            if (dir->cur_cluster >= FAT12_EOC) return FALSE;
            if (dir->offset >= (UINT16)((UINT16)g_bpb.sec_per_clus << 9)) {
                dir->cur_cluster = fat12_next_cluster(dir->cur_cluster);
                dir->offset = 0;
                if (dir->cur_cluster >= FAT12_EOC) return FALSE;
            }
            if (!fat12_read_cluster_entry(dir->cur_cluster, dir->offset, out)) return FALSE;
            dir->offset = (UINT16)(dir->offset + 32);
        }

        if (out->name[0] == 0x00) return FALSE;
        if (out->name[0] == 0xE5) continue;
        if (out->attr == FAT_ATTR_LFN) continue;
        return TRUE;
    }
}

static BOOL fat12_find_in_dir(UINT16 cluster, const char *name, FAT_DIR_RAW *out_raw) {
    FS_DIR dir;
    FAT_DIR_RAW raw;

    dir.root = (cluster == 0) ? TRUE : FALSE;
    dir.cluster = cluster;
    dir.cur_cluster = cluster;
    dir.offset = 0;
    dir.root_index = 0;

    while (fat12_dir_read_raw(&dir, &raw)) {
        if (fat12_names_equal(&raw, name)) {
            sima_memcpy(out_raw, &raw, (UINT16)sizeof(FAT_DIR_RAW));
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL fat12_loc_to_lba_off(const FAT_DIR_LOC *loc, UINT32 *out_lba, UINT16 *out_off) {
    UINT16 sector;

    if (!loc || !out_lba || !out_off) return FALSE;

    if (loc->root) {
        if (loc->root_index >= g_bpb.root_ents) return FALSE;
        sector = (UINT16)(loc->root_index >> 4);
        *out_off = (UINT16)((loc->root_index & 0x0F) << 5);
        *out_lba = (UINT32)(g_root_start_lba + sector);
        return TRUE;
    }

    if (loc->cluster < 2 || loc->cluster >= FAT12_EOC) return FALSE;
    sector = (UINT16)(loc->offset >> 9);
    *out_off = (UINT16)(loc->offset & (SECTOR_SIZE - 1));
    *out_lba = (UINT32)(fat12_cluster_to_lba(loc->cluster) + sector);
    return TRUE;
}

static BOOL fat12_write_entry_at_loc(const FAT_DIR_LOC *loc, const FAT_DIR_RAW *raw) {
    UINT32 lba;
    UINT16 off_in_sector;

    if (!loc || !raw) return FALSE;
    if (!fat12_loc_to_lba_off(loc, &lba, &off_in_sector)) return FALSE;
    if (!read_sector(lba, g_sector)) return FALSE;
    sima_memcpy(&g_sector[off_in_sector], raw, (UINT16)sizeof(FAT_DIR_RAW));
    return write_sector(lba, g_sector);
}

static BOOL fat12_find_entry_loc(UINT16 dir_cluster, const char *name, FAT_DIR_RAW *out_raw, FAT_DIR_LOC *out_loc) {
    FAT_DIR_RAW raw;

    if (!name || !out_raw || !out_loc) return FALSE;

    if (dir_cluster == 0) {
        UINT16 index;
        for (index = 0; index < g_bpb.root_ents; ++index) {
            if (!fat12_read_root_entry(index, &raw)) return FALSE;
            if (raw.name[0] == 0x00) return FALSE;
            if (raw.name[0] == 0xE5) continue;
            if (raw.attr == FAT_ATTR_LFN) continue;
            if (fat12_names_equal(&raw, name)) {
                sima_memcpy(out_raw, &raw, (UINT16)sizeof(FAT_DIR_RAW));
                out_loc->root = TRUE;
                out_loc->root_index = index;
                out_loc->cluster = 0;
                out_loc->offset = 0;
                return TRUE;
            }
        }
        return FALSE;
    }

    {
        UINT16 cluster = dir_cluster;
        UINT16 cluster_bytes = (UINT16)((UINT16)g_bpb.sec_per_clus << 9);

        while (cluster >= 2 && cluster < FAT12_EOC) {
            UINT16 off;
            for (off = 0; off < cluster_bytes; off = (UINT16)(off + 32)) {
                if (!fat12_read_cluster_entry(cluster, off, &raw)) return FALSE;
                if (raw.name[0] == 0x00) return FALSE;
                if (raw.name[0] == 0xE5) continue;
                if (raw.attr == FAT_ATTR_LFN) continue;
                if (fat12_names_equal(&raw, name)) {
                    sima_memcpy(out_raw, &raw, (UINT16)sizeof(FAT_DIR_RAW));
                    out_loc->root = FALSE;
                    out_loc->root_index = 0;
                    out_loc->cluster = cluster;
                    out_loc->offset = off;
                    return TRUE;
                }
            }
            cluster = fat12_next_cluster(cluster);
        }
    }
    return FALSE;
}

static BOOL fat12_find_free_slot(UINT16 dir_cluster, FAT_DIR_LOC *out_loc) {
    FAT_DIR_RAW raw;

    if (!out_loc) return FALSE;

    if (dir_cluster == 0) {
        UINT16 index;
        for (index = 0; index < g_bpb.root_ents; ++index) {
            if (!fat12_read_root_entry(index, &raw)) return FALSE;
            if (raw.name[0] == 0x00 || raw.name[0] == 0xE5) {
                out_loc->root = TRUE;
                out_loc->root_index = index;
                out_loc->cluster = 0;
                out_loc->offset = 0;
                return TRUE;
            }
        }
        return FALSE;
    }

    {
        UINT16 cluster = dir_cluster;
        UINT16 last_cluster = 0;
        UINT16 cluster_bytes = (UINT16)((UINT16)g_bpb.sec_per_clus << 9);

        while (cluster >= 2 && cluster < FAT12_EOC) {
            UINT16 off;
            last_cluster = cluster;
            for (off = 0; off < cluster_bytes; off = (UINT16)(off + 32)) {
                if (!fat12_read_cluster_entry(cluster, off, &raw)) return FALSE;
                if (raw.name[0] == 0x00 || raw.name[0] == 0xE5) {
                    out_loc->root = FALSE;
                    out_loc->root_index = 0;
                    out_loc->cluster = cluster;
                    out_loc->offset = off;
                    return TRUE;
                }
            }
            cluster = fat12_next_cluster(cluster);
        }

        /* No free slot: extend directory by one cluster. */
        if (last_cluster == 0) return FALSE;

        {
            UINT16 new_cluster = fat12_find_free_cluster();
            if (new_cluster == 0) return FALSE;

            if (!fat12_set_entry(new_cluster, FAT12_EOC_VALUE)) return FALSE;
            if (!fat12_set_entry(last_cluster, new_cluster)) {
                (void)fat12_set_entry(new_cluster, 0x000);
                (void)fat12_flush_fat();
                return FALSE;
            }

            if (!fat12_flush_fat()) {
                (void)fat12_set_entry(last_cluster, FAT12_EOC_VALUE);
                (void)fat12_set_entry(new_cluster, 0x000);
                (void)fat12_flush_fat();
                return FALSE;
            }

            if (!fat12_clear_cluster(new_cluster)) {
                (void)fat12_set_entry(last_cluster, FAT12_EOC_VALUE);
                (void)fat12_set_entry(new_cluster, 0x000);
                (void)fat12_flush_fat();
                return FALSE;
            }

            out_loc->root = FALSE;
            out_loc->root_index = 0;
            out_loc->cluster = new_cluster;
            out_loc->offset = 0;
            return TRUE;
        }
    }

    return FALSE;
}

static void fat12_raw_set_name83(FAT_DIR_RAW *raw, const UINT8 name83[11]) {
    UINT16 i;
    if (!raw || !name83) return;
    for (i = 0; i < 8; ++i) raw->name[i] = name83[i];
    for (i = 0; i < 3; ++i) raw->ext[i] = name83[8 + i];
}

static BOOL fat12_make_raw_entry(FAT_DIR_RAW *raw, const char *name, UINT8 attr, UINT16 start_cluster, UINT32 size) {
    UINT8 name83[11];
    UINT16 i;

    if (!raw || !name) return FALSE;
    if (!fat12_build_83(name, name83)) return FALSE;

    sima_memset(raw, 0, (UINT16)sizeof(FAT_DIR_RAW));
    fat12_raw_set_name83(raw, name83);
    raw->attr = attr;
    raw->reserved = 0;
    raw->ctime_tenths = 0;
    for (i = 0; i < 2; ++i) raw->start_cluster_hi[i] = 0;
    wr16(raw->start_cluster_lo, start_cluster);
    wr32(raw->size, size);
    return TRUE;
}

static BOOL fat12_write_new_dir_cluster(UINT16 dir_cluster, UINT16 parent_cluster) {
    FAT_DIR_RAW dot;
    FAT_DIR_RAW dotdot;
    UINT8 name83[11];
    UINT16 i;
    UINT32 lba;

    if (dir_cluster < 2 || dir_cluster > g_max_cluster) return FALSE;

    lba = (UINT32)fat12_cluster_to_lba(dir_cluster);
    for (i = 0; i < g_bpb.sec_per_clus; ++i) {
        sima_memset(g_sector, 0, SECTOR_SIZE);

        if (i == 0) {
            /* '.' entry */
            sima_memset(name83, ' ', 11);
            name83[0] = '.';
            sima_memset(&dot, 0, (UINT16)sizeof(dot));
            fat12_raw_set_name83(&dot, name83);
            dot.attr = FAT_ATTR_DIRECTORY;
            wr16(dot.start_cluster_lo, dir_cluster);
            wr32(dot.size, 0);

            /* '..' entry */
            sima_memset(name83, ' ', 11);
            name83[0] = '.';
            name83[1] = '.';
            sima_memset(&dotdot, 0, (UINT16)sizeof(dotdot));
            fat12_raw_set_name83(&dotdot, name83);
            dotdot.attr = FAT_ATTR_DIRECTORY;
            wr16(dotdot.start_cluster_lo, parent_cluster);
            wr32(dotdot.size, 0);

            sima_memcpy(&g_sector[0], &dot, (UINT16)sizeof(FAT_DIR_RAW));
            sima_memcpy(&g_sector[32], &dotdot, (UINT16)sizeof(FAT_DIR_RAW));
        }

        if (!write_sector(lba + i, g_sector)) return FALSE;
    }
    return TRUE;
}

static BOOL fat12_dir_is_empty(UINT16 dir_cluster) {
    FAT_DIR_RAW raw;
    UINT16 cluster = dir_cluster;
    UINT16 cluster_bytes;

    if (cluster < 2 || cluster >= FAT12_EOC) return FALSE;
    cluster_bytes = (UINT16)((UINT16)g_bpb.sec_per_clus << 9);

    while (cluster >= 2 && cluster < FAT12_EOC) {
        UINT16 off;
        for (off = 0; off < cluster_bytes; off = (UINT16)(off + 32)) {
            if (!fat12_read_cluster_entry(cluster, off, &raw)) return FALSE;
            if (raw.name[0] == 0x00) return TRUE;
            if (raw.name[0] == 0xE5) continue;
            if (raw.attr == FAT_ATTR_LFN) continue;

            /* Skip '.' and '..' */
            if (raw.name[0] == '.' && (raw.name[1] == ' ' || raw.name[1] == '.')) continue;
            return FALSE;
        }
        cluster = fat12_next_cluster(cluster);
    }
    return TRUE;
}

static UINT16 fat12_parent_cluster(UINT16 cluster) {
    FAT_DIR_RAW raw;
    FS_DIR dir;

    if (cluster == 0) return 0;

    dir.root = FALSE;
    dir.cluster = cluster;
    dir.cur_cluster = cluster;
    dir.offset = 0;
    dir.root_index = 0;

    while (fat12_dir_read_raw(&dir, &raw)) {
        if (raw.name[0] == '.' && raw.name[1] == '.' ) {
            return le16(raw.start_cluster_lo);
        }
    }
    return 0;
}

static const char *fat12_next_token(const char *path, char *token, UINT16 cap) {
    UINT16 len = 0;
    if (!path) return NULL;
    while (*path == '/' || *path == '\\') ++path;
    if (*path == '\0') return NULL;

    while (*path && *path != '/' && *path != '\\') {
        if (len + 1 < cap) {
            token[len++] = *path;
        }
        ++path;
    }
    token[len] = '\0';
    return path;
}

static void fat12_path_reset(char *path, UINT16 cap) {
    sima_memclr(path, cap);
    sima_strcpy(path, cap, "/");
}

static void fat12_path_append(char *path, UINT16 cap, const char *name) {
    if (path[0] == '\0') {
        sima_strcpy(path, cap, "/");
    }
    if (sima_strcmp(path, "/") != STRC_SAME) {
        sima_strcat(path, cap, "/");
    }
    sima_strcat(path, cap, name);
}

static void fat12_path_pop(char *path) {
    UINT16 len = sima_strlen(path);
    if (len == 0 || sima_strcmp(path, "/") == STRC_SAME) return;
    while (len > 0 && path[len - 1] != '/') {
        path[len - 1] = '\0';
        --len;
    }
    if (len > 1) {
        path[len - 1] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
}

static BOOL fat12_resolve_dir(const char *path, UINT16 *out_cluster, char *out_path, UINT16 out_cap) {
    char token[FS_NAME_MAX];
    const char *p = path;
    UINT16 cluster = g_cwd_cluster;
    char temp_path[FS_PATH_MAX];
    FAT_DIR_RAW raw;
    BOOL last;
    const char *scan;

    if (!path || path[0] == '\0') {
        if (out_cluster) *out_cluster = g_cwd_cluster;
        if (out_path) sima_strcpy(out_path, out_cap, g_cwd_path);
        return TRUE;
    }

    if (path[0] == '/' || path[0] == '\\') {
        cluster = 0;
        fat12_path_reset(temp_path, (UINT16)sizeof(temp_path));
    } else {
        sima_strcpy(temp_path, (UINT16)sizeof(temp_path), g_cwd_path);
    }

    while ((p = fat12_next_token(p, token, (UINT16)sizeof(token)))) {
        last = TRUE;
        scan = p;
        while (*scan) {
            if (*scan != '/' && *scan != '\\') { last = FALSE; break; }
            ++scan;
        }

        if (sima_strcmp(token, ".") == STRC_SAME) {
            continue;
        }
        if (sima_strcmp(token, "..") == STRC_SAME) {
            cluster = fat12_parent_cluster(cluster);
            fat12_path_pop(temp_path);
            continue;
        }

        if (!fat12_find_in_dir(cluster, token, &raw)) return FALSE;
        if ((raw.attr & FAT_ATTR_DIRECTORY) == 0) return FALSE;

        cluster = le16(raw.start_cluster_lo);
        fat12_path_append(temp_path, (UINT16)sizeof(temp_path), token);

        if (last) break;
    }

    if (out_cluster) *out_cluster = cluster;
    if (out_path) sima_strcpy(out_path, out_cap, temp_path);
    return TRUE;
}

static BOOL fat12_resolve_entry(const char *path, FAT_DIR_RAW *out_raw) {
    char token[FS_NAME_MAX];
    const char *p = path;
    UINT16 cluster = g_cwd_cluster;
    FAT_DIR_RAW raw;
    BOOL last;
    const char *scan;

    if (!path || path[0] == '\0') return FALSE;
    if (path[0] == '/' || path[0] == '\\') {
        cluster = 0;
    }

    while ((p = fat12_next_token(p, token, (UINT16)sizeof(token)))) {
        last = TRUE;
        scan = p;
        while (*scan) {
            if (*scan != '/' && *scan != '\\') { last = FALSE; break; }
            ++scan;
        }

        if (sima_strcmp(token, ".") == STRC_SAME) {
            continue;
        }
        if (sima_strcmp(token, "..") == STRC_SAME) {
            cluster = fat12_parent_cluster(cluster);
            continue;
        }

        if (!fat12_find_in_dir(cluster, token, &raw)) return FALSE;
        if (last) {
            sima_memcpy(out_raw, &raw, (UINT16)sizeof(FAT_DIR_RAW));
            return TRUE;
        }
        if ((raw.attr & FAT_ATTR_DIRECTORY) == 0) return FALSE;
        cluster = le16(raw.start_cluster_lo);
    }

    return FALSE;
}

static BOOL fat12_resolve_parent(const char *path, UINT16 *out_parent, char *out_name, UINT16 out_cap) {
    char token[FS_NAME_MAX];
    const char *p = path;
    UINT16 cluster = g_cwd_cluster;
    FAT_DIR_RAW raw;
    BOOL last;
    const char *scan;

    if (!path || path[0] == '\0') return FALSE;
    if (!out_parent || !out_name || out_cap == 0) return FALSE;

    if (path[0] == '/' || path[0] == '\\') {
        cluster = 0;
    }

    sima_memclr(out_name, out_cap);

    while ((p = fat12_next_token(p, token, (UINT16)sizeof(token)))) {
        last = TRUE;
        scan = p;
        while (*scan) {
            if (*scan != '/' && *scan != '\\') { last = FALSE; break; }
            ++scan;
        }

        if (last) {
            sima_strcpy(out_name, out_cap, token);
            *out_parent = cluster;
            return TRUE;
        }

        if (sima_strcmp(token, ".") == STRC_SAME) {
            continue;
        }
        if (sima_strcmp(token, "..") == STRC_SAME) {
            cluster = fat12_parent_cluster(cluster);
            continue;
        }

        if (!fat12_find_in_dir(cluster, token, &raw)) return FALSE;
        if ((raw.attr & FAT_ATTR_DIRECTORY) == 0) return FALSE;
        cluster = le16(raw.start_cluster_lo);
    }
    return FALSE;
}

static BOOL fat12_read_file_cluster(UINT16 cluster, UINT8 *out, UINT16 out_cap, UINT16 *offset, UINT32 *remaining) {
    UINT16 i;
    UINT16 lba;
    UINT32 to_copy;

    if (cluster < 2 || cluster >= FAT12_EOC) return FALSE;

    lba = fat12_cluster_to_lba(cluster);
    for (i = 0; i < g_bpb.sec_per_clus; ++i) {
        if (*remaining == 0) return TRUE;
        if (!read_sector((UINT32)(lba + i), g_sector)) return FALSE;

        to_copy = *remaining;
        if (to_copy > SECTOR_SIZE) to_copy = SECTOR_SIZE;
        if (*offset + (UINT16)to_copy > out_cap) {
            to_copy = (UINT32)(out_cap - *offset);
        }
        sima_memcpy(&out[*offset], g_sector, (UINT16)to_copy);
        *offset = (UINT16)(*offset + (UINT16)to_copy);
        *remaining -= to_copy;
        if (*offset >= out_cap) return TRUE;
    }
    return TRUE;
}

BOOL fs_init(void) {
    if (!fat12_load_bpb()) return FALSE;
    if (!fat12_load_fat()) return FALSE;
    g_cwd_cluster = 0;
    fat12_path_reset(g_cwd_path, (UINT16)sizeof(g_cwd_path));
    return TRUE;
}

BOOL fs_cd(const char *path) {
    UINT16 cluster = 0;
    char new_path[FS_PATH_MAX];

    if (!fat12_resolve_dir(path, &cluster, new_path, (UINT16)sizeof(new_path))) return FALSE;
    g_cwd_cluster = cluster;
    sima_strcpy(g_cwd_path, (UINT16)sizeof(g_cwd_path), new_path);
    return TRUE;
}

BOOL fs_get_cwd(char *out, UINT16 out_cap) {
    if (!out || out_cap == 0) return FALSE;
    return sima_strcpy(out, out_cap, g_cwd_path);
}

BOOL fs_dir_open(const char *path, FS_DIR *out_dir) {
    UINT16 cluster = 0;

    if (!out_dir) return FALSE;
    if (!fat12_resolve_dir(path, &cluster, NULL, 0)) return FALSE;

    out_dir->root = (cluster == 0) ? TRUE : FALSE;
    out_dir->cluster = cluster;
    out_dir->cur_cluster = cluster;
    out_dir->offset = 0;
    out_dir->root_index = 0;
    return TRUE;
}

BOOL fs_dir_read(FS_DIR *dir, FS_DIRENT *out) {
    FAT_DIR_RAW raw;

    if (!dir || !out) return FALSE;

    while (fat12_dir_read_raw(dir, &raw)) {
        UINT16 cluster = le16(raw.start_cluster_lo);

        fat12_format_name(&raw, out->name, (UINT16)sizeof(out->name));
        if (out->name[0] == '\0') continue;
        if (sima_strcmp(out->name, ".") == STRC_SAME || sima_strcmp(out->name, "..") == STRC_SAME) {
            continue;
        }

        out->size = le32(raw.size);
        out->first_cluster = cluster;
        out->is_dir = (raw.attr & FAT_ATTR_DIRECTORY) ? TRUE : FALSE;
        out->is_volume = (raw.attr & FAT_ATTR_VOLUME) ? TRUE : FALSE;
        out->is_hidden = (raw.attr & FAT_ATTR_HIDDEN) ? TRUE : FALSE;
        out->is_system = (raw.attr & FAT_ATTR_SYSTEM) ? TRUE : FALSE;
        out->is_exec = FALSE;

        {
            const char *dot = sima_strchr(out->name, '.');
            if (dot && (sima_strcmp(dot, ".PRG") == STRC_SAME || sima_strcmp(dot, ".BIN") == STRC_SAME)) {
                out->is_exec = TRUE;
            }
        }
        return TRUE;
    }
    return FALSE;
}

BOOL fs_read(const char *path, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    FAT_DIR_RAW raw;
    UINT32 remaining;
    UINT16 offset = 0;
    UINT16 cluster;

    if (!path || !out || !out_size) return FALSE;
    if (!fat12_resolve_entry(path, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;

    remaining = le32(raw.size);
    if (remaining > out_cap) remaining = out_cap;
    cluster = le16(raw.start_cluster_lo);

    while (remaining > 0 && cluster >= 2 && cluster < FAT12_EOC) {
        if (!fat12_read_file_cluster(cluster, out, out_cap, &offset, &remaining)) return FALSE;
        cluster = fat12_next_cluster(cluster);
    }

    *out_size = offset;
    return TRUE;
}

static BOOL fs_build_path(char *out, UINT16 cap, const char *dir_name, const char *name) {
    sima_memclr(out, cap);
    if (!dir_name || dir_name[0] == '\0') {
        sima_strcpy(out, cap, name);
        return TRUE;
    }
    if (dir_name[0] == '/' || dir_name[0] == '\\') {
        sima_strcpy(out, cap, dir_name);
    } else {
        sima_strcpy(out, cap, "/");
        sima_strcat(out, cap, dir_name);
    }
    if (out[sima_strlen(out) - 1] != '/') {
        sima_strcat(out, cap, "/");
    }
    sima_strcat(out, cap, name);
    return TRUE;
}

BOOL fs_read_in_dir(const char *dir_name, const char *name, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    char path[FS_PATH_MAX];
    if (!dir_name || !name) return FALSE;
    if (!fs_build_path(path, (UINT16)sizeof(path), dir_name, name)) return FALSE;
    return fs_read(path, out, out_cap, out_size);
}

BOOL fs_is_executable(const char *path) {
    FAT_DIR_RAW raw;
    FS_DIRENT entry;
    if (!path) return FALSE;
    if (!fat12_resolve_entry(path, &raw)) return FALSE;
    if (raw.attr & FAT_ATTR_DIRECTORY) return FALSE;
    fat12_format_name(&raw, entry.name, (UINT16)sizeof(entry.name));
    {
        const char *dot = sima_strchr(entry.name, '.');
        if (!dot) return FALSE;
        if (sima_strcmp(dot, ".PRG") == STRC_SAME || sima_strcmp(dot, ".BIN") == STRC_SAME) return TRUE;
    }
    return FALSE;
}

BOOL fs_is_executable_in_dir(const char *dir_name, const char *name) {
    char path[FS_PATH_MAX];
    if (!dir_name || !name) return FALSE;
    if (!fs_build_path(path, (UINT16)sizeof(path), dir_name, name)) return FALSE;
    return fs_is_executable(path);
}

BOOL fs_is_dir(const char *path) {
    FAT_DIR_RAW raw;
    if (!path) return FALSE;
    if (!fat12_resolve_entry(path, &raw)) return FALSE;
    return (raw.attr & FAT_ATTR_DIRECTORY) ? TRUE : FALSE;
}

static BOOL fat12_write_chain(UINT16 first_cluster, const UINT8 *data, UINT16 size) {
    UINT16 cluster = first_cluster;
    UINT32 remaining = (UINT32)size;
    UINT32 offset = 0;

    if (size == 0) return TRUE;
    if (!data) return FALSE;
    if (cluster < 2 || cluster >= FAT12_EOC) return FALSE;

    while (cluster >= 2 && cluster < FAT12_EOC) {
        UINT32 lba = (UINT32)fat12_cluster_to_lba(cluster);
        UINT16 s;

        for (s = 0; s < g_bpb.sec_per_clus; ++s) {
            UINT32 to_copy = 0;
            sima_memset(g_sector, 0, SECTOR_SIZE);

            if (remaining > 0) {
                to_copy = remaining;
                if (to_copy > SECTOR_SIZE) to_copy = SECTOR_SIZE;
                sima_memcpy(g_sector, &data[offset], (UINT16)to_copy);
                offset += to_copy;
                remaining -= to_copy;
            }

            if (!write_sector(lba + s, g_sector)) return FALSE;
        }

        if (remaining == 0) return TRUE;
        cluster = fat12_next_cluster(cluster);
    }
    return FALSE;
}

BOOL fs_format(void) {
    return FALSE;
}

BOOL fs_create_dir(const char *name) {
    UINT16 parent_cluster;
    char entry_name[FS_NAME_MAX];
    FAT_DIR_RAW existing;
    UINT16 new_cluster;
    FAT_DIR_LOC loc;
    FAT_DIR_RAW raw;

    if (!name) return FALSE;
    if (!fat12_resolve_parent(name, &parent_cluster, entry_name, (UINT16)sizeof(entry_name))) return FALSE;
    if (entry_name[0] == '\0') return FALSE;
    if (sima_strcmp(entry_name, ".") == STRC_SAME) return FALSE;
    if (sima_strcmp(entry_name, "..") == STRC_SAME) return FALSE;

    if (fat12_find_in_dir(parent_cluster, entry_name, &existing)) {
        return FALSE;
    }

    if (!fat12_alloc_chain(1, &new_cluster)) return FALSE;
    if (new_cluster < 2) return FALSE;

    if (!fat12_write_new_dir_cluster(new_cluster, parent_cluster)) {
        (void)fat12_free_chain(new_cluster);
        return FALSE;
    }

    if (!fat12_find_free_slot(parent_cluster, &loc)) {
        (void)fat12_free_chain(new_cluster);
        return FALSE;
    }

    if (!fat12_make_raw_entry(&raw, entry_name, FAT_ATTR_DIRECTORY, new_cluster, 0)) {
        (void)fat12_free_chain(new_cluster);
        return FALSE;
    }

    if (!fat12_write_entry_at_loc(&loc, &raw)) {
        (void)fat12_free_chain(new_cluster);
        return FALSE;
    }

    return TRUE;
}

BOOL fs_write(const char *name, const UINT8 *data, UINT16 size) {
    UINT16 parent_cluster;
    char entry_name[FS_NAME_MAX];
    FAT_DIR_RAW existing;
    FAT_DIR_LOC loc;
    BOOL found_existing;

    UINT16 old_first_cluster = 0;
    UINT16 new_first_cluster = 0;
    UINT16 cluster_count = 0;
    UINT16 cluster_bytes;
    FAT_DIR_RAW raw;

    if (!name) return FALSE;
    if (size > 0 && !data) return FALSE;

    if (!fat12_resolve_parent(name, &parent_cluster, entry_name, (UINT16)sizeof(entry_name))) return FALSE;
    if (entry_name[0] == '\0') return FALSE;
    if (sima_strcmp(entry_name, ".") == STRC_SAME) return FALSE;
    if (sima_strcmp(entry_name, "..") == STRC_SAME) return FALSE;

    found_existing = fat12_find_entry_loc(parent_cluster, entry_name, &existing, &loc);
    if (found_existing) {
        if (existing.attr & FAT_ATTR_DIRECTORY) return FALSE;
        if (existing.attr & FAT_ATTR_READONLY) return FALSE;
        old_first_cluster = le16(existing.start_cluster_lo);
    } else {
        if (!fat12_find_free_slot(parent_cluster, &loc)) return FALSE;
    }

    if (size == 0) {
        new_first_cluster = 0;
    } else {
        cluster_bytes = (UINT16)((UINT16)g_bpb.sec_per_clus << 9);
        if (cluster_bytes == 0) return FALSE;

        cluster_count = (UINT16)(size / cluster_bytes);
        if ((UINT16)(size % cluster_bytes) != 0) cluster_count = (UINT16)(cluster_count + 1);
        if (cluster_count == 0) cluster_count = 1;

        if (!fat12_alloc_chain(cluster_count, &new_first_cluster)) return FALSE;
        if (!fat12_write_chain(new_first_cluster, data, size)) {
            (void)fat12_free_chain(new_first_cluster);
            return FALSE;
        }
    }

    if (!fat12_make_raw_entry(&raw, entry_name, FAT_ATTR_ARCHIVE, new_first_cluster, (UINT32)size)) {
        if (new_first_cluster >= 2) (void)fat12_free_chain(new_first_cluster);
        return FALSE;
    }

    if (!fat12_write_entry_at_loc(&loc, &raw)) {
        if (new_first_cluster >= 2) (void)fat12_free_chain(new_first_cluster);
        return FALSE;
    }

    /* Free the old chain after the entry points to the new one. */
    if (old_first_cluster >= 2) {
        if (!fat12_free_chain(old_first_cluster)) return FALSE;
    }

    return TRUE;
}

BOOL fs_write_in_dir(const char *dir_name, const char *name, const UINT8 *data, UINT16 size) {
    char path[FS_PATH_MAX];
    if (!dir_name || !name) return FALSE;
    if (!fs_build_path(path, (UINT16)sizeof(path), dir_name, name)) return FALSE;
    return fs_write(path, data, size);
}

BOOL fs_delete(const char *name) {
    UINT16 parent_cluster;
    char entry_name[FS_NAME_MAX];
    FAT_DIR_RAW raw;
    FAT_DIR_LOC loc;
    UINT16 first_cluster;

    if (!name) return FALSE;
    if (!fat12_resolve_parent(name, &parent_cluster, entry_name, (UINT16)sizeof(entry_name))) return FALSE;
    if (entry_name[0] == '\0') return FALSE;
    if (sima_strcmp(entry_name, ".") == STRC_SAME) return FALSE;
    if (sima_strcmp(entry_name, "..") == STRC_SAME) return FALSE;

    if (!fat12_find_entry_loc(parent_cluster, entry_name, &raw, &loc)) return FALSE;
    if (raw.attr & FAT_ATTR_READONLY) return FALSE;

    first_cluster = le16(raw.start_cluster_lo);

    if (raw.attr & FAT_ATTR_DIRECTORY) {
        if (first_cluster < 2) return FALSE;
        if (!fat12_dir_is_empty(first_cluster)) return FALSE;
    }

    raw.name[0] = 0xE5;
    if (!fat12_write_entry_at_loc(&loc, &raw)) return FALSE;

    if (first_cluster >= 2) {
        if (!fat12_free_chain(first_cluster)) return FALSE;
    }

    return TRUE;
}

BOOL fs_sync(void) {
    /* Writes are immediate, but keep this for a manual FAT flush. */
    return fat12_flush_fat();
}
