#ifndef __SIMA_PERM__
#define __SIMA_PERM__

#include "sima_type.h"

/* Linux-like permission bits (simplified for FAT32) */
#define PERM_OWNER_READ    0x0100
#define PERM_OWNER_WRITE   0x0080
#define PERM_OWNER_EXEC    0x0040
#define PERM_GROUP_READ    0x0020
#define PERM_GROUP_WRITE   0x0010
#define PERM_GROUP_EXEC    0x0008
#define PERM_OTHER_READ    0x0004
#define PERM_OTHER_WRITE   0x0002
#define PERM_OTHER_EXEC    0x0001

#define PERM_SUID          0x0800  /* Set UID on execution */
#define PERM_SGID          0x0400  /* Set GID on execution */
#define PERM_STICKY        0x0200  /* Sticky bit (restricted delete) */

/* Common permission masks */
#define PERM_OWNER_RW      (PERM_OWNER_READ | PERM_OWNER_WRITE)
#define PERM_OWNER_RWX     (PERM_OWNER_READ | PERM_OWNER_WRITE | PERM_OWNER_EXEC)
#define PERM_GROUP_RW      (PERM_GROUP_READ | PERM_GROUP_WRITE)
#define PERM_GROUP_RWX     (PERM_GROUP_READ | PERM_GROUP_WRITE | PERM_GROUP_EXEC)
#define PERM_OTHER_RW      (PERM_OTHER_READ | PERM_OTHER_WRITE)
#define PERM_OTHER_RWX     (PERM_OTHER_READ | PERM_OTHER_WRITE | PERM_OTHER_EXEC)

/* Standard Linux permission modes */
#define PERM_0755          (PERM_OWNER_RWX | PERM_GROUP_READ | PERM_GROUP_EXEC | PERM_OTHER_READ | PERM_OTHER_EXEC)
#define PERM_0644          (PERM_OWNER_RW | PERM_GROUP_READ | PERM_OTHER_READ)
#define PERM_0600          (PERM_OWNER_RW)
#define PERM_0700          (PERM_OWNER_RWX)

/* Permission check operations */
#define PERM_OP_READ       0x01
#define PERM_OP_WRITE      0x02
#define PERM_OP_EXEC       0x04

/**
 * Check if current user has permission to access a path
 * @param path Absolute path to check
 * @param operation PERM_OP_READ, PERM_OP_WRITE, or PERM_OP_EXEC
 * @return TRUE if allowed, FALSE otherwise
 */
BOOL perm_check(const char *path, UINT8 operation);

/**
 * Check if current user can access a specific directory/file
 * Used by commands to enforce permission model
 */
BOOL perm_allow_path(const char *path, BOOL allow_bin_read);

/**
 * Return TRUE if current user is root (UID 0)
 */
BOOL perm_is_root(void);

/**
 * Parse permission string like "755" or "0644"
 * @param str Permission string (octal)
 * @param out Output permission bits
 * @return TRUE if valid
 */
BOOL perm_parse_mode(const char *str, UINT16 *out);

/**
 * Format permission bits to string like "rwxr-xr-x"
 * @param mode Permission bits
 * @param out Output buffer (at least 10 bytes)
 */
void perm_format_mode(UINT16 mode, char *out);

#endif
