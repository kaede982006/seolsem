#include "xef_loader.h"
#include "sima_mem.h"
#include "sima_conv.h"
#include "sima_syscall.h"
#include "sima_io.h"
#include "sima_fs.h"
#include "sima_perm.h"

typedef struct {
    UINT16 entry;
    UINT16 code_size;
    UINT16 data_size;
    UINT16 bss_size;
    UINT16 import_off;
    UINT16 import_count;
    UINT16 reloc_off;
    UINT16 reloc_count;
    UINT16 str_off;
    UINT16 str_size;
} XEF_NATIVE_META;

typedef struct {
    UINT16 patch_off;
    UINT8 type;
    UINT8 arg;
} XEF_RELOC_REC;

extern UINT16 __cdecl xef_call_entry(UINT16 entry_off, const char *arg, UINT16 stack_top);

static UINT16 xef_le16(const UINT8 *p) {
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static void xef_set_le16(UINT8 *p, UINT16 value) {
    p[0] = (UINT8)(value & 0xFF);
    p[1] = (UINT8)((value >> 8) & 0xFF);
}

static BOOL xef_str_eq(const char *a, const char *b) {
    UINT16 i = 0;
    if (!a || !b) return FALSE;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return FALSE;
        ++i;
    }
    return (a[i] == '\0' && b[i] == '\0') ? TRUE : FALSE;
}

/* Import wrappers (cdecl). */
static UINT16 __cdecl xef_imp_print(const char *text) {
    syscall_print(text);
    return 0;
}

static UINT16 __cdecl xef_imp_print_num(UINT16 value) {
    char text[8];
    sima_memclr(text, (UINT16)sizeof(text));
    sima_utoa(value, text, (UINT16)sizeof(text), 10);
    syscall_print(text);
    return 0;
}

static UINT16 __cdecl xef_imp_cls(void) {
    syscall_cls();
    return 0;
}

static UINT16 __cdecl xef_imp_read_key(void) {
    UINT16 k = read_key();
    return k;
}

static UINT16 __cdecl xef_imp_write_char(UINT16 row, UINT16 col, UINT16 ch, UINT16 attr) {
    write_char((UINT8)row, (UINT8)col, (char)ch, (UINT8)attr);
    return 0;
}

static UINT16 __cdecl xef_imp_cursor_set(UINT16 row, UINT16 col) {
    set_cursor((UINT8)row, (UINT8)col);
    return 0;
}

static UINT16 __cdecl xef_imp_cursor_show(void) {
    cursor_show();
    return 0;
}

static UINT16 __cdecl xef_imp_cursor_hide(void) {
    cursor_hide();
    return 0;
}

/* File system (minimal): write file.
 * Return codes:
 *   0 = ok
 *   1 = permission denied
 *   2 = target is a directory
 *   3 = invalid argument
 *   4 = write failed
 */
static UINT16 __cdecl xef_imp_fs_write(const char *path, const UINT8 *data, UINT16 size) {
    FS_DIRENT ent;
    if (!path || path[0] == '\0') return 3;
    if (!perm_check(path, PERM_OP_WRITE)) return 1;
    if (fs_stat(path, &ent) && ent.is_dir) return 2;
    if (!fs_write(path, data, (UINT32)size)) return 4;
    return 0;
}

/* File system (minimal): read file.
 * Return codes:
 *   0 = ok
 *   1 = not found
 *   2 = permission denied
 *   3 = target is a directory
 *   4 = invalid argument
 *   5 = too large
 *   6 = read failed
 */
static UINT16 __cdecl xef_imp_fs_read(const char *path, UINT8 *out, UINT16 out_cap, UINT16 *out_size) {
    FS_DIRENT ent;
    UINT32 size = 0;

    if (out_size) *out_size = 0;
    if (!path || path[0] == '\0') return 4;
    if (!out || out_cap == 0) return 4;
    if (!perm_check(path, PERM_OP_READ)) return 2;
    if (!fs_stat(path, &ent)) return 1;
    if (ent.is_dir) return 3;
    if (ent.size > (UINT32)out_cap) return 5;
    if (!fs_read(path, out, (UINT32)out_cap, &size)) return 6;
    if (out_size) *out_size = (UINT16)size;
    return 0;
}

typedef struct {
    const char *name;
    UINT8 id;
} XEF_IMPORT_NAME;

enum {
    XEF_IMPORT_PRINT = 0,
    XEF_IMPORT_PRINT_NUM = 1,
    XEF_IMPORT_CLS = 2,
    XEF_IMPORT_READ_KEY = 3,
    XEF_IMPORT_WRITE_CHAR = 4,
    XEF_IMPORT_CURSOR_SET = 5,
    XEF_IMPORT_CURSOR_SHOW = 6,
    XEF_IMPORT_CURSOR_HIDE = 7,
    XEF_IMPORT_FS_WRITE = 8,
    XEF_IMPORT_FS_READ = 9
};

static const XEF_IMPORT_NAME g_xef_import_names[] = {
    { "print", XEF_IMPORT_PRINT },
    { "print_num", XEF_IMPORT_PRINT_NUM },
    { "cls", XEF_IMPORT_CLS },
    { "read_key", XEF_IMPORT_READ_KEY },
    { "write_char", XEF_IMPORT_WRITE_CHAR },
    { "cursor_set", XEF_IMPORT_CURSOR_SET },
    { "cursor_show", XEF_IMPORT_CURSOR_SHOW },
    { "cursor_hide", XEF_IMPORT_CURSOR_HIDE },
    { "fs_write", XEF_IMPORT_FS_WRITE },
    { "fs_read", XEF_IMPORT_FS_READ }
};

static BOOL xef_import_id_to_offset(UINT8 id, UINT16 *out_off) {
    if (!out_off) return FALSE;
    switch (id) {
    case XEF_IMPORT_PRINT:
        *out_off = (UINT16)(xef_imp_print);
        return TRUE;
    case XEF_IMPORT_PRINT_NUM:
        *out_off = (UINT16)(xef_imp_print_num);
        return TRUE;
    case XEF_IMPORT_CLS:
        *out_off = (UINT16)(xef_imp_cls);
        return TRUE;
    case XEF_IMPORT_READ_KEY:
        *out_off = (UINT16)(xef_imp_read_key);
        return TRUE;
    case XEF_IMPORT_WRITE_CHAR:
        *out_off = (UINT16)(xef_imp_write_char);
        return TRUE;
    case XEF_IMPORT_CURSOR_SET:
        *out_off = (UINT16)(xef_imp_cursor_set);
        return TRUE;
    case XEF_IMPORT_CURSOR_SHOW:
        *out_off = (UINT16)(xef_imp_cursor_show);
        return TRUE;
    case XEF_IMPORT_CURSOR_HIDE:
        *out_off = (UINT16)(xef_imp_cursor_hide);
        return TRUE;
    case XEF_IMPORT_FS_WRITE:
        *out_off = (UINT16)(xef_imp_fs_write);
        return TRUE;
    case XEF_IMPORT_FS_READ:
        *out_off = (UINT16)(xef_imp_fs_read);
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL xef_resolve_import(const char *name, UINT16 *out_off) {
    UINT16 i;
    if (!name || !out_off) return FALSE;
    for (i = 0; i < (UINT16)(sizeof(g_xef_import_names) / sizeof(g_xef_import_names[0])); ++i) {
        if (xef_str_eq(name, g_xef_import_names[i].name)) {
            return xef_import_id_to_offset(g_xef_import_names[i].id, out_off);
        }
    }
    return FALSE;
}

static BOOL xef_read_import_name(const UINT8 *image, const XEF_NATIVE_META *meta,
                                 UINT16 import_idx, char *out_name, UINT16 out_cap) {
    const UINT8 *import_base;
    const UINT8 *str_base;
    UINT16 off;
    UINT16 j;

    if (!image || !meta || !out_name || out_cap == 0) return FALSE;
    if (import_idx >= meta->import_count) return FALSE;

    import_base = image + meta->import_off;
    str_base = image + meta->str_off;
    off = xef_le16(import_base + (UINT16)(import_idx * 2U));
    if (off >= meta->str_size) return FALSE;

    j = 0;
    while ((UINT16)(off + j) < meta->str_size) {
        UINT8 ch = str_base[off + j];
        if (ch == 0) {
            out_name[j] = '\0';
            return TRUE;
        }
        if ((UINT16)(j + 1U) >= out_cap) return FALSE;
        out_name[j++] = (char)ch;
    }

    return FALSE;
}

static BOOL xef_validate_header(const UINT8 *image, UINT16 image_size, XEF_NATIVE_META *meta) {
    UINT32 load_blob_end;

    if (!image || !meta) return FALSE;
    if (image_size < XEFN_HEADER_SIZE) return FALSE;

    if (image[0] != 'X' || image[1] != 'E' || image[2] != 'F' || image[3] != 'N') return FALSE;
    if (xef_le16(&image[4]) != XEFN_VERSION) return FALSE;

    meta->entry = xef_le16(&image[8]);
    meta->code_size = xef_le16(&image[10]);
    meta->data_size = xef_le16(&image[12]);
    meta->bss_size = xef_le16(&image[14]);
    meta->import_off = xef_le16(&image[16]);
    meta->import_count = xef_le16(&image[18]);
    meta->reloc_off = xef_le16(&image[20]);
    meta->reloc_count = xef_le16(&image[22]);
    meta->str_off = xef_le16(&image[24]);
    meta->str_size = xef_le16(&image[26]);

    if (meta->code_size == 0) return FALSE;
    if (meta->entry >= meta->code_size) return FALSE;

    load_blob_end = (UINT32)XEFN_HEADER_SIZE + (UINT32)meta->code_size + (UINT32)meta->data_size;
    if (load_blob_end > (UINT32)image_size) return FALSE;

    if (meta->import_count > 0) {
        if (meta->str_size == 0) return FALSE;
        if (meta->import_off < (UINT16)load_blob_end) return FALSE;
        if (meta->str_off < (UINT16)load_blob_end) return FALSE;
        if ((UINT32)meta->import_off + (UINT32)(meta->import_count * 2U) > (UINT32)image_size) return FALSE;
        if ((UINT32)meta->str_off + (UINT32)meta->str_size > (UINT32)image_size) return FALSE;
    }

    if (meta->reloc_count > 0) {
        if (meta->reloc_off < (UINT16)load_blob_end) return FALSE;
        if ((UINT32)meta->reloc_off + (UINT32)(meta->reloc_count * 4U) > (UINT32)image_size) return FALSE;
    }

    return TRUE;
}

static BOOL xef_build_import_addr_table(const UINT8 *image, const XEF_NATIVE_META *meta,
                                        UINT16 *import_addr, UINT16 import_addr_cap) {
    UINT16 i;

    if (!image || !meta) return FALSE;
    if (meta->import_count == 0) return TRUE;
    if (!import_addr) return FALSE;
    if (meta->import_count > import_addr_cap) return FALSE;

    for (i = 0; i < meta->import_count; ++i) {
        char name[64];
        if (!xef_read_import_name(image, meta, i, name, (UINT16)sizeof(name))) {
            return FALSE;
        }
        if (!xef_resolve_import(name, &import_addr[i])) {
            return FALSE;
        }
    }

    return TRUE;
}

static BOOL xef_apply_relocs(UINT8 *load_base, UINT16 load_size, UINT16 load_base_off,
                             const UINT8 *image, const XEF_NATIVE_META *meta,
                             const UINT16 *import_addr) {
    UINT16 i;
    const UINT8 *reloc_base;

    if (!load_base || !image || !meta) return FALSE;
    if (meta->reloc_count == 0) return TRUE;

    reloc_base = image + meta->reloc_off;

    for (i = 0; i < meta->reloc_count; ++i) {
        const UINT8 *rec = reloc_base + (UINT16)(i * 4U);
        XEF_RELOC_REC r;
        UINT16 cur;

        r.patch_off = xef_le16(rec);
        r.type = rec[2];
        r.arg = rec[3];

        if ((UINT32)r.patch_off + 2U > (UINT32)load_size) return FALSE;
        cur = xef_le16(load_base + r.patch_off);

        if (r.type == XEFN_RELOC_BASE16) {
            /* Idempotent patching: add base only while value looks like an unrelocated offset. */
            if (cur < load_size) {
                cur = (UINT16)(cur + load_base_off);
                xef_set_le16(load_base + r.patch_off, cur);
            }
            continue;
        }

        if (r.type == XEFN_RELOC_IMPORT16) {
            if (!import_addr) return FALSE;
            if (r.arg >= meta->import_count) return FALSE;
            xef_set_le16(load_base + r.patch_off, import_addr[r.arg]);
            continue;
        }

        return FALSE;
    }

    return TRUE;
}

static UINT16 xef_pick_stack_top(const XEF_EXEC_CONTEXT *ctx) {
    UINT16 stack_top;

    if (!ctx || !ctx->image) return 0xFFFE;
    if (ctx->stack_top != 0) {
        stack_top = (UINT16)(ctx->stack_top & 0xFFFE);
        if (stack_top == 0) return 0xFFFE;
        return stack_top;
    }

    {
        UINT32 top32 = (UINT32)(UINT16)ctx->image + (UINT32)ctx->image_capacity;
        if (top32 > 0xFFFEUL) top32 = 0xFFFEUL;
        stack_top = (UINT16)top32;
        stack_top &= 0xFFFE;
        if (stack_top == 0) stack_top = 0xFFFE;
    }

    return stack_top;
}

XEF_RESULT xef_native_exec_ctx(XEF_EXEC_CONTEXT *ctx, UINT16 *exit_code) {
    XEF_NATIVE_META meta;
    UINT16 import_addr[32];
    UINT8 *load_base;
    UINT16 patch_size;
    const UINT8 *file_code_data;
    UINT16 file_code_data_size;
    UINT32 exec_size32;
    UINT16 exec_size;
    UINT16 base_off;
    UINT16 entry_off;
    UINT16 stack_top;
    UINT16 status;

    if (exit_code) *exit_code = 1;
    if (!ctx || !ctx->image) return XEF_ERR_BAD_HEADER;

    sync_ds();

    if (!xef_validate_header(ctx->image, ctx->image_size, &meta)) {
        return XEF_ERR_BAD_HEADER;
    }

    exec_size32 = (UINT32)meta.code_size + (UINT32)meta.data_size + (UINT32)meta.bss_size;
    if (exec_size32 == 0 || exec_size32 > 65535UL) {
        return XEF_ERR_IMAGE_TOO_LARGE;
    }
    exec_size = (UINT16)exec_size32;
    if (ctx->image_capacity < exec_size) {
        return XEF_ERR_NO_MEMORY;
    }

    if (meta.import_count > (UINT16)(sizeof(import_addr) / sizeof(import_addr[0]))) {
        return XEF_ERR_IMPORT;
    }

    sima_memclr((char*)import_addr, (UINT16)sizeof(import_addr));
    if (!xef_build_import_addr_table(ctx->image, &meta, import_addr,
                                     (UINT16)(sizeof(import_addr) / sizeof(import_addr[0])))) {
        return XEF_ERR_IMPORT;
    }

    load_base = ctx->image;
    file_code_data = ctx->image + XEFN_HEADER_SIZE;
    file_code_data_size = (UINT16)(meta.code_size + meta.data_size);
    patch_size = exec_size;

    if (meta.bss_size == 0) {
        load_base = (UINT8*)file_code_data;
    } else {
        if (ctx->image_capacity >= (UINT16)(XEFN_HEADER_SIZE + exec_size)) {
            load_base = (UINT8*)file_code_data;
        } else if (ctx->image_capacity >= exec_size) {
            if (!sima_memmove(load_base, file_code_data, file_code_data_size)) {
                return XEF_ERR_EXEC;
            }
        } else {
            return XEF_ERR_NO_MEMORY;
        }
    }

    base_off = (UINT16)load_base;
    if (!xef_apply_relocs(load_base, patch_size, base_off, ctx->image, &meta, import_addr)) {
        return XEF_ERR_RELOC;
    }

    if (meta.bss_size > 0) {
        /* BSS may overlap the loader metadata area (imports/relocs/strings),
         * so clear it only after relocation/import processing is complete.
         */
        if (!sima_memset(load_base + meta.code_size + meta.data_size, 0, meta.bss_size)) {
            return XEF_ERR_EXEC;
        }
    }

    entry_off = (UINT16)(base_off + meta.entry);
    stack_top = xef_pick_stack_top(ctx);
    status = xef_call_entry(entry_off, ctx->arg, stack_top);
    sync_ds();

    if (exit_code) *exit_code = status;
    return XEF_OK;
}
