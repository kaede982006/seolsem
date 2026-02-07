#include "sima_perm.h"
#include "sima_user.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_conv.h"
#include "sima_env.h"

/**
 * Linux-style permission system for Seolsem OS
 * Simplified for FAT32 (no actual UID/GID stored on disk)
 * Uses in-memory permission model based on path patterns
 */

/* System paths owned by root */
static const char* g_root_paths[] = {
    "/BIN",
    "/SBIN",
    "/ETC",
    "/BOOT",
    "/SYS",
    NULL
};

/* Check if path starts with given prefix */
static BOOL path_has_prefix(const char *path, const char *prefix) {
    UINT16 i = 0;
    if (!path || !prefix) return FALSE;
    while (prefix[i] != '\0') {
        if (path[i] != prefix[i]) return FALSE;
        ++i;
    }
    /* Exact match or followed by separator */
    return (path[i] == '\0' || path[i] == '/' || path[i] == '\\');
}

BOOL perm_is_root(void) {
    const SIMA_USER *u = user_current();
    return (u && u->used && u->uid == 0) ? TRUE : FALSE;
}

/* Check if path is under user's HOME */
static BOOL is_home_path(const char *abs_path) {
    const char *home = env_get("HOME");
    char home_abs[FS_PATH_MAX];

    if (!home || home[0] == '\0') home = "/";
    sima_memclr(home_abs, (UINT16)sizeof(home_abs));

    /* Make absolute home path */
    if (!fs_make_abs_path(home, home_abs, (UINT16)sizeof(home_abs))) {
        return FALSE;
    }

    return path_has_prefix(abs_path, home_abs);
}

/* Check if path is system path (root-owned) */
static BOOL is_system_path(const char *abs_path) {
    UINT16 i;
    for (i = 0; g_root_paths[i] != NULL; ++i) {
        if (path_has_prefix(abs_path, g_root_paths[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

BOOL perm_allow_path(const char *path, BOOL allow_bin_read) {
    char abs_path[FS_PATH_MAX];

    /* Root can access everything */
    if (perm_is_root()) return TRUE;

    /* Resolve to absolute path */
    sima_memclr(abs_path, (UINT16)sizeof(abs_path));
    if (!fs_make_abs_path(path, abs_path, (UINT16)sizeof(abs_path))) {
        return FALSE;
    }

    /* User can access own HOME */
    if (is_home_path(abs_path)) return TRUE;

    /* User can READ from /BIN if allowed */
    if (allow_bin_read && path_has_prefix(abs_path, "/BIN")) {
        return TRUE;
    }

    /* Deny system paths */
    if (is_system_path(abs_path)) return FALSE;

    /* Allow other paths (e.g., /TMP) */
    return TRUE;
}

BOOL perm_check(const char *path, UINT8 operation) {
    char abs_path[FS_PATH_MAX];

    /* Root bypasses all checks */
    if (perm_is_root()) return TRUE;

    if (!path) return FALSE;

    sima_memclr(abs_path, (UINT16)sizeof(abs_path));
    if (!fs_make_abs_path(path, abs_path, (UINT16)sizeof(abs_path))) {
        return FALSE;
    }

    /* System paths: read-only for non-root */
    if (is_system_path(abs_path)) {
        if (operation & PERM_OP_WRITE) return FALSE;
        if (operation & PERM_OP_EXEC) {
            /* Allow execution from /BIN, /SBIN */
            if (path_has_prefix(abs_path, "/BIN") ||
                path_has_prefix(abs_path, "/SBIN")) {
                return TRUE;
            }
            return FALSE;
        }
        return TRUE; /* READ allowed */
    }

    /* HOME: full access */
    if (is_home_path(abs_path)) return TRUE;

    /* Default: allow */
    return TRUE;
}

BOOL perm_parse_mode(const char *str, UINT16 *out) {
    UINT16 mode = 0;
    UINT16 val;

    if (!str || !out) return FALSE;

    /* Skip leading "0" if present */
    if (str[0] == '0') ++str;

    /* Parse octal digits (max 4 digits: special bits + owner + group + other) */
    while (*str >= '0' && *str <= '7') {
        val = (UINT16)(*str - '0');
        mode = (mode << 3) | val;
        ++str;
    }

    /* Must end with null */
    if (*str != '\0') return FALSE;

    *out = mode;
    return TRUE;
}

void perm_format_mode(UINT16 mode, char *out) {
    if (!out) return;

    /* Format as "rwxrwxrwx" */
    out[0] = (mode & PERM_OWNER_READ) ? 'r' : '-';
    out[1] = (mode & PERM_OWNER_WRITE) ? 'w' : '-';
    out[2] = (mode & PERM_OWNER_EXEC) ? 'x' : '-';
    out[3] = (mode & PERM_GROUP_READ) ? 'r' : '-';
    out[4] = (mode & PERM_GROUP_WRITE) ? 'w' : '-';
    out[5] = (mode & PERM_GROUP_EXEC) ? 'x' : '-';
    out[6] = (mode & PERM_OTHER_READ) ? 'r' : '-';
    out[7] = (mode & PERM_OTHER_WRITE) ? 'w' : '-';
    out[8] = (mode & PERM_OTHER_EXEC) ? 'x' : '-';
    out[9] = '\0';

    /* Handle special bits */
    if (mode & PERM_SUID) out[2] = (mode & PERM_OWNER_EXEC) ? 's' : 'S';
    if (mode & PERM_SGID) out[5] = (mode & PERM_GROUP_EXEC) ? 's' : 'S';
    if (mode & PERM_STICKY) out[8] = (mode & PERM_OTHER_EXEC) ? 't' : 'T';
}
