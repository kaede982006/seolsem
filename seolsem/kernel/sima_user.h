#ifndef __SIMA_USER__
#define __SIMA_USER__

#include "sima_type.h"

#define USER_MAX 8
#define USER_NAME_MAX 9   /* 8.3 base name length + NUL */
#define USER_HOME_MAX 128 /* keep in sync with FS_PATH_MAX */
#define USER_PASS_MAX 9   /* installer/useradd default: 1..8 chars + NUL */

typedef struct {
    BOOL used;
    char name[USER_NAME_MAX];
    UINT16 uid;
    char home[USER_HOME_MAX];
    char pass[USER_PASS_MAX];
} SIMA_USER;

BOOL user_init(void);
BOOL user_add(const char *name, const char *password);
BOOL user_login(const char *name, const char *password);
BOOL user_change_password(const char *old_password, const char *new_password);

const SIMA_USER *user_current(void);
UINT16 user_count(void);
const SIMA_USER *user_at(UINT16 index);

#endif
