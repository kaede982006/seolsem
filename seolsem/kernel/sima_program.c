#include "sima_program.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_heap.h"
#include "sima_perm.h"
#include "xef_loader.h"

typedef struct {
    UINT8 *base;
    UINT16 heap_mark;
    UINT16 capacity;
    UINT16 image_size;
    UINT16 last_exec_size;
    UINT16 generation;
    BOOL allocated;
    BOOL loaded;
} PROGRAM_SLOT;

static PROGRAM_SLOT g_program_slot;

#ifndef PROGRAM_LOADER_STACK_RESERVE
#define PROGRAM_LOADER_STACK_RESERVE 0U
#endif


static UINT16 program_le16(const UINT8 *p) {
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static UINT16 program_required_capacity(const UINT8 *header, UINT16 header_size, UINT16 file_size) {
    UINT32 exec_size;
    if (!header || header_size < XEFN_HEADER_SIZE) return file_size;
    if (header[0] != 'X' || header[1] != 'E' || header[2] != 'F' || header[3] != 'N') return file_size;
    if (program_le16(&header[4]) != XEFN_VERSION) return file_size;

    exec_size = (UINT32)program_le16(&header[10]) +
                (UINT32)program_le16(&header[12]) +
                (UINT32)program_le16(&header[14]);
    if (exec_size == 0 || exec_size > 65535UL) return file_size;
    if (exec_size <= (UINT32)file_size) return file_size;
    return (UINT16)exec_size;
}

static UINT16 program_next_generation(void) {
    if (g_program_slot.generation == PROGRAM_HANDLE_INVALID) {
        g_program_slot.generation = 1U;
    } else {
        ++g_program_slot.generation;
        if (g_program_slot.generation == PROGRAM_HANDLE_INVALID) {
            g_program_slot.generation = 1U;
        }
    }
    return g_program_slot.generation;
}

static BOOL program_handle_valid(PROGRAM_HANDLE handle) {
    if (handle == PROGRAM_HANDLE_INVALID) return FALSE;
    if (!g_program_slot.allocated || !g_program_slot.base) return FALSE;
    if (!g_program_slot.loaded) return FALSE;
    return (handle == g_program_slot.generation) ? TRUE : FALSE;
}

static PROGRAM_RESULT program_from_xef_result(XEF_RESULT rc) {
    if (rc == XEF_OK) return PROGRAM_OK;
    if (rc == XEF_ERR_BAD_HEADER) return PROGRAM_ERR_BAD_FORMAT;
    if (rc == XEF_ERR_IMAGE_TOO_LARGE) return PROGRAM_ERR_FILE_TOO_LARGE;
    if (rc == XEF_ERR_NO_MEMORY) return PROGRAM_ERR_NO_MEMORY;
    if (rc == XEF_ERR_IMPORT) return PROGRAM_ERR_IMPORT;
    if (rc == XEF_ERR_RELOC) return PROGRAM_ERR_RELOC;
    return PROGRAM_ERR_EXEC;
}

static PROGRAM_RESULT program_loader_ensure_slot(void) {
    UINT16 remain;
    UINT16 slot_size;
    UINT16 mark;

    if (g_program_slot.allocated && g_program_slot.base && g_program_slot.capacity > 0) {
        return PROGRAM_OK;
    }

    remain = sima_heap_remaining();
    if (remain <= (UINT16)(PROGRAM_LOADER_STACK_RESERVE + XEFN_HEADER_SIZE)) {
        return PROGRAM_ERR_NO_MEMORY;
    }
    slot_size = (UINT16)((remain - PROGRAM_LOADER_STACK_RESERVE) & 0xFFFE);
    if (slot_size <= XEFN_HEADER_SIZE) return PROGRAM_ERR_NO_MEMORY;

    mark = sima_heap_mark();
    g_program_slot.heap_mark = mark;
    g_program_slot.base = (UINT8*)sima_malloc(slot_size);
    if (!g_program_slot.base) {
        sima_heap_restore(mark);
        g_program_slot.capacity = 0;
        g_program_slot.image_size = 0;
        g_program_slot.last_exec_size = 0;
        g_program_slot.loaded = FALSE;
        g_program_slot.allocated = FALSE;
        return PROGRAM_ERR_NO_MEMORY;
    }

    g_program_slot.capacity = slot_size;
    g_program_slot.image_size = 0;
    g_program_slot.last_exec_size = 0;
    g_program_slot.loaded = FALSE;
    g_program_slot.allocated = TRUE;
    return PROGRAM_OK;
}

static BOOL program_build_dir_path(const char *dir_name, const char *name,
                                   char *out_path, UINT16 out_cap) {
    UINT16 len;
    if (!dir_name || !name || !out_path || out_cap == 0) return FALSE;
    sima_memclr(out_path, out_cap);
    if (!sima_strcpy(out_path, out_cap, dir_name)) return FALSE;
    len = sima_strlen(out_path);
    if (len == 0) return FALSE;
    if (out_path[len - 1] != '/' && out_path[len - 1] != '\\') {
        if (!sima_strcat(out_path, out_cap, "/")) return FALSE;
    }
    if (!sima_strcat(out_path, out_cap, name)) return FALSE;
    return TRUE;
}

static PROGRAM_RESULT program_loader_commit(PROGRAM_HANDLE *out_handle,
                                            UINT32 read_size, UINT16 need_size) {
    if (!out_handle) return PROGRAM_ERR_INVALID_ARG;
    if (read_size == 0 || read_size > (UINT32)g_program_slot.capacity) return PROGRAM_ERR_IO;

    g_program_slot.image_size = (UINT16)read_size;
    g_program_slot.last_exec_size = need_size;
    g_program_slot.loaded = TRUE;
    *out_handle = program_next_generation();
    return PROGRAM_OK;
}

static PROGRAM_RESULT program_loader_open_common(const char *path,
                                                 const char *dir_name,
                                                 const char *name,
                                                 PROGRAM_HANDLE *out_handle) {
    FS_DIRENT ent;
    UINT8 header[XEFN_HEADER_SIZE];
    UINT32 header_size = 0;
    UINT32 read_size = 0;
    UINT16 need_size = 0;
    PROGRAM_RESULT rc;
    BOOL in_dir;

    if (out_handle) *out_handle = PROGRAM_HANDLE_INVALID;
    if (!path || path[0] == '\0' || !out_handle) return PROGRAM_ERR_INVALID_ARG;
    in_dir = (dir_name && name) ? TRUE : FALSE;

    if (!perm_check(path, PERM_OP_READ | PERM_OP_EXEC)) return PROGRAM_ERR_PERMISSION;

    if (in_dir) {
        if (!fs_stat_in_dir(dir_name, name, &ent)) return PROGRAM_ERR_NOT_FOUND;
        if (ent.is_dir) return PROGRAM_ERR_NOT_EXECUTABLE;
        if (!fs_is_executable_in_dir(dir_name, name)) return PROGRAM_ERR_NOT_EXECUTABLE;
    } else {
        if (!fs_stat(path, &ent)) return PROGRAM_ERR_NOT_FOUND;
        if (ent.is_dir) return PROGRAM_ERR_NOT_EXECUTABLE;
        if (!fs_is_executable(path)) return PROGRAM_ERR_NOT_EXECUTABLE;
    }
    if (ent.size == 0 || ent.size > 65535UL) return PROGRAM_ERR_FILE_TOO_LARGE;

    sima_memclr((char*)header, (UINT16)sizeof(header));
    if (in_dir) {
        if (!fs_read_in_dir(dir_name, name, header, (UINT32)sizeof(header), &header_size) ||
            header_size < XEFN_HEADER_SIZE) {
            return PROGRAM_ERR_BAD_FORMAT;
        }
    } else {
        if (!fs_read(path, header, (UINT32)sizeof(header), &header_size) ||
            header_size < XEFN_HEADER_SIZE) {
            return PROGRAM_ERR_BAD_FORMAT;
        }
    }

    need_size = program_required_capacity(header, (UINT16)header_size, (UINT16)ent.size);

    rc = program_loader_ensure_slot();
    if (rc != PROGRAM_OK) return rc;
    if (need_size > g_program_slot.capacity) return PROGRAM_ERR_FILE_TOO_LARGE;

    if (in_dir) {
        if (!fs_read_in_dir(dir_name, name, g_program_slot.base, (UINT32)ent.size, &read_size) ||
            read_size != ent.size) {
            return PROGRAM_ERR_IO;
        }
    } else {
        if (!fs_read(path, g_program_slot.base, (UINT32)ent.size, &read_size) ||
            read_size != ent.size) {
            return PROGRAM_ERR_IO;
        }
    }

    return program_loader_commit(out_handle, read_size, need_size);
}

PROGRAM_RESULT program_loader_open(const char *path, PROGRAM_HANDLE *out_handle) {
    return program_loader_open_common(path, (const char*)0, (const char*)0, out_handle);
}

PROGRAM_RESULT program_loader_open_in_dir(const char *dir_name, const char *name, PROGRAM_HANDLE *out_handle) {
    char full_path[FS_PATH_MAX];

    if (out_handle) *out_handle = PROGRAM_HANDLE_INVALID;
    if (!dir_name || !name || name[0] == '\0' || !out_handle) return PROGRAM_ERR_INVALID_ARG;
    if (!program_build_dir_path(dir_name, name, full_path, (UINT16)sizeof(full_path))) {
        return PROGRAM_ERR_INVALID_ARG;
    }
    return program_loader_open_common(full_path, dir_name, name, out_handle);
}

PROGRAM_RESULT program_loader_exec(PROGRAM_HANDLE handle, const char *arg, UINT16 *out_exit_code) {
    XEF_EXEC_CONTEXT exec_ctx;
    XEF_RESULT xef_rc;
    UINT16 exit_code = 1;

    if (out_exit_code) *out_exit_code = 1;
    if (!program_handle_valid(handle)) return PROGRAM_ERR_BAD_HANDLE;

    exec_ctx.image = g_program_slot.base;
    exec_ctx.image_size = g_program_slot.image_size;
    exec_ctx.image_capacity = g_program_slot.capacity;
    exec_ctx.arg = arg;
    exec_ctx.stack_top = 0;

    xef_rc = xef_native_exec_ctx(&exec_ctx, &exit_code);
    if (xef_rc != XEF_OK) return program_from_xef_result(xef_rc);
    g_program_slot.last_exec_size = g_program_slot.image_size;

    if (out_exit_code) *out_exit_code = exit_code;
    return PROGRAM_OK;
}

PROGRAM_RESULT program_loader_close(PROGRAM_HANDLE handle) {
    if (!program_handle_valid(handle)) return PROGRAM_ERR_BAD_HANDLE;

    /* The slot is heap-backed. Closing releases it so other heap users can run. */
    sima_heap_restore(g_program_slot.heap_mark);
    g_program_slot.base = (UINT8*)0;
    g_program_slot.heap_mark = 0;
    g_program_slot.capacity = 0;
    g_program_slot.loaded = FALSE;
    g_program_slot.allocated = FALSE;
    g_program_slot.image_size = 0;
    g_program_slot.last_exec_size = 0;
    return PROGRAM_OK;
}

PROGRAM_RESULT program_loader_result_message(PROGRAM_RESULT rc, char *out, UINT16 out_cap) {
    const char *text = "prog:E?";

    if (rc == PROGRAM_OK) text = "prog:OK";
    else if (rc == PROGRAM_ERR_INVALID_ARG) text = "prog:EARG";
    else if (rc == PROGRAM_ERR_PERMISSION) text = "prog:EPERM";
    else if (rc == PROGRAM_ERR_NOT_FOUND) text = "prog:ENOENT";
    else if (rc == PROGRAM_ERR_NOT_EXECUTABLE) text = "prog:ENOEXEC";
    else if (rc == PROGRAM_ERR_FILE_TOO_LARGE) text = "prog:ETOOBIG";
    else if (rc == PROGRAM_ERR_NO_MEMORY) text = "prog:ENOMEM";
    else if (rc == PROGRAM_ERR_IO) text = "prog:EIO";
    else if (rc == PROGRAM_ERR_BAD_FORMAT) text = "prog:EBADFMT";
    else if (rc == PROGRAM_ERR_IMPORT) text = "prog:EIMPORT";
    else if (rc == PROGRAM_ERR_RELOC) text = "prog:ERELOC";
    else if (rc == PROGRAM_ERR_EXEC) text = "prog:EEXEC";
    else if (rc == PROGRAM_ERR_BAD_HANDLE) text = "prog:EBADH";

    if (out && out_cap > 0) {
        sima_memclr(out, out_cap);
        sima_strcpy(out, out_cap, text);
    }
    return rc;
}

PROGRAM_RESULT program_loader_get_info(PROGRAM_LOADER_INFO *out) {
    UINT16 remain;
    UINT16 cap;

    if (!out) return PROGRAM_ERR_INVALID_ARG;
    out->image_capacity = 0;
    out->stack_reserve = PROGRAM_LOADER_STACK_RESERVE;

    if (g_program_slot.allocated && g_program_slot.base && g_program_slot.capacity > 0) {
        out->image_capacity = g_program_slot.capacity;
        return PROGRAM_OK;
    }

    remain = sima_heap_remaining();
    if (remain <= (UINT16)(PROGRAM_LOADER_STACK_RESERVE + XEFN_HEADER_SIZE)) {
        return PROGRAM_ERR_NO_MEMORY;
    }
    cap = (UINT16)((remain - PROGRAM_LOADER_STACK_RESERVE) & 0xFFFE);
    out->image_capacity = cap;
    return PROGRAM_OK;
}

