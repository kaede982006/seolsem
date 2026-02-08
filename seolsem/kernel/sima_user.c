#include "sima_user.h"
#include "sima_mem.h"
#include "sima_conv.h"
#include "sima_env.h"
#include "sima_fs.h"

#define PASSWD_PATH "/ETC/PASSWD"
#define HOME_BASE   "/HOME"

static SIMA_USER g_users[USER_MAX];
static SIMA_USER g_current;

static UINT8 g_passwd_buf[512 + 1];

static char user_to_upper(char ch) {
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - 'a' + 'A');
    }
    return ch;
}

static BOOL user_is_name_char(char ch) {
    if (ch >= 'a' && ch <= 'z') return TRUE;
    if (ch >= 'A' && ch <= 'Z') return TRUE;
    if (ch >= '0' && ch <= '9') return TRUE;
    return (ch == '_') ? TRUE : FALSE;
}

static BOOL user_copy_password(const char *in, char *out, UINT16 out_cap, BOOL allow_empty) {
    UINT16 len = 0;
    if (!out || out_cap == 0) return FALSE;
    if (!in) in = "";
    sima_memclr(out, out_cap);
    while (*in != '\0') {
        char ch = *in++;
        if (ch < 32 || ch == 127 || ch == ':' || ch == '\r' || ch == '\n') return FALSE;
        if (len + 1 >= out_cap) return FALSE;
        out[len++] = ch;
    }
    if (len == 0 && !allow_empty) return FALSE;
    out[len] = '\0';
    return TRUE;
}

static BOOL user_normalize_name(const char *in, char *out, UINT16 out_cap) {
    UINT16 len = 0;
    if (!in || !out || out_cap == 0) return FALSE;
    sima_memclr(out, out_cap);
    while (*in != '\0') {
        char ch = *in++;
        if (!user_is_name_char(ch)) return FALSE;
        if (len + 1 >= out_cap) return FALSE;
        out[len++] = user_to_upper(ch);
        if (len >= 8) break; /* FAT 8.3 base limit */
    }
    if (len == 0) return FALSE;
    /* Reject names longer than 8. */
    if (*in != '\0') return FALSE;
    out[len] = '\0';
    return TRUE;
}

static const SIMA_USER *user_find(const char *name_norm) {
    UINT16 i;
    if (!name_norm || name_norm[0] == '\0') return (const SIMA_USER*)0;
    for (i = 0; i < USER_MAX; ++i) {
        if (g_users[i].used &&
            sima_strcmp(g_users[i].name, name_norm) == STRC_SAME) {
            return &g_users[i];
        }
    }
    return (const SIMA_USER*)0;
}

static BOOL ensure_dir(const char *path) {
    if (!path) return FALSE;
    if (fs_is_dir(path)) return TRUE;
    return fs_create_dir(path);
}

static BOOL user_set_current(const SIMA_USER *u) {
    char cwd[FS_PATH_MAX];

    if (!u || !u->used) return FALSE;
    sima_memcpy(&g_current, u, (UINT16)sizeof(SIMA_USER));
    env_set_public("USER", g_current.name);
    env_set_public("HOME", g_current.home);
    if (!fs_cd(g_current.home)) {
        (void)fs_cd("/");
    }
    sima_memclr(cwd, (UINT16)sizeof(cwd));
    if (!fs_get_cwd(cwd, (UINT16)sizeof(cwd))) {
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "/");
    }
    env_set_public("PWD", cwd);
    env_set_public("OLDPWD", cwd);
    return TRUE;
}

static void user_add_entry(const char *name, UINT16 uid, const char *home, const char *pass) {
    UINT16 i;
    for (i = 0; i < USER_MAX; ++i) {
        if (!g_users[i].used) {
            g_users[i].used = TRUE;
            sima_strcpy(g_users[i].name, (UINT16)sizeof(g_users[i].name), name);
            g_users[i].uid = uid;
            sima_strcpy(g_users[i].home, (UINT16)sizeof(g_users[i].home), home);
            if (pass) {
                sima_strcpy(g_users[i].pass, (UINT16)sizeof(g_users[i].pass), pass);
            } else {
                g_users[i].pass[0] = '\0';
            }
            return;
        }
    }
}

static void user_parse_line(char *line) {
    char *c1;
    char *c2;
    char *name;
    char *uid_s;
    char *home;
    char *pass;
    char *c3;
    char name_norm[USER_NAME_MAX];
    char pass_norm[USER_PASS_MAX];
    UINT16 uid = 0;

    if (!line) return;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0' || *line == '#') return;

    c1 = (char*)sima_strchr(line, ':');
    if (!c1) return;
    *c1 = '\0';
    name = line;
    uid_s = c1 + 1;

    c2 = (char*)sima_strchr(uid_s, ':');
    if (!c2) return;
    *c2 = '\0';
    home = c2 + 1;
    pass = "";

    c3 = (char*)sima_strchr(home, ':');
    if (c3) {
        *c3 = '\0';
        pass = c3 + 1;
    }

    if (!user_normalize_name(name, name_norm, (UINT16)sizeof(name_norm))) return;
    if (!sima_atoi(uid_s, &uid)) return;
    if (!user_copy_password(pass, pass_norm, (UINT16)sizeof(pass_norm), TRUE)) return;
    if (!home || home[0] == '\0') home = "/";

    if (user_find(name_norm)) return;
    user_add_entry(name_norm, uid, home, pass_norm);
}

static BOOL user_load_passwd(void) {
    UINT32 size = 0;
    UINT32 i = 0;
    UINT32 start = 0;

    sima_memclr((char*)g_users, (UINT16)sizeof(g_users));
    if (!fs_read(PASSWD_PATH, g_passwd_buf, (UINT32)sizeof(g_passwd_buf) - 1U, &size)) {
        /* No passwd file yet (or FS not ready) -> keep a minimal in-memory root entry. */
        user_add_entry("ROOT", 0, "/", "");
        return FALSE;
    }

    g_passwd_buf[size] = '\0';
    for (i = 0; i <= size; ++i) {
        UINT8 ch = g_passwd_buf[i];
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            g_passwd_buf[i] = '\0';
            if (i > start) {
                user_parse_line((char*)&g_passwd_buf[start]);
            }
            start = i + 1;
        }
    }
    return TRUE;
}

static BOOL user_sync_passwd(void) {
    UINT16 i;
    char out[512];
    char uid_s[8];

    sima_memclr(out, (UINT16)sizeof(out));
    for (i = 0; i < USER_MAX; ++i) {
        if (!g_users[i].used) continue;
        sima_memclr(uid_s, (UINT16)sizeof(uid_s));
        if (!sima_utoa(g_users[i].uid, uid_s, (UINT16)sizeof(uid_s), 10)) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), g_users[i].name)) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), ":")) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), uid_s)) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), ":")) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), g_users[i].home)) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), ":")) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), g_users[i].pass)) return FALSE;
        if (!sima_strcat(out, (UINT16)sizeof(out), "\r\n")) return FALSE;
    }
    return fs_write(PASSWD_PATH, (const UINT8*)out, (UINT32)sima_strlen(out));
}

BOOL user_init(void) {
    BOOL loaded;

    sima_memclr((char*)&g_current, (UINT16)sizeof(g_current));
    loaded = user_load_passwd();

    /* Ensure there's at least a root user. */
    if (!user_find("ROOT")) {
        user_add_entry("ROOT", 0, "/", "");
        loaded = FALSE; /* force sync to include ROOT */
    }

    /* Best-effort: create base dirs and write PASSWD (may fail on FAT12). */
    (void)ensure_dir("/ETC");
    (void)ensure_dir(HOME_BASE);
    if (!loaded) {
        (void)user_sync_passwd();
    }

    user_logout();
    return TRUE;
}

BOOL user_add(const char *name, const char *password) {
    char name_norm[USER_NAME_MAX];
    char pass_norm[USER_PASS_MAX];
    char home[USER_HOME_MAX];
    UINT16 i;
    UINT16 max_uid = 999;

    if (!user_normalize_name(name, name_norm, (UINT16)sizeof(name_norm))) return FALSE;
    if (!password || password[0] == '\0') {
        password = name_norm; /* deterministic default when useradd omits password */
    }
    if (!user_copy_password(password, pass_norm, (UINT16)sizeof(pass_norm), FALSE)) return FALSE;
    (void)user_load_passwd();

    if (user_find(name_norm)) return FALSE;

    for (i = 0; i < USER_MAX; ++i) {
        if (!g_users[i].used) continue;
        if (g_users[i].uid > max_uid) max_uid = g_users[i].uid;
    }

    sima_memclr(home, (UINT16)sizeof(home));
    sima_strcpy(home, (UINT16)sizeof(home), HOME_BASE);
    sima_strcat(home, (UINT16)sizeof(home), "/");
    sima_strcat(home, (UINT16)sizeof(home), name_norm);

    if (!ensure_dir("/ETC")) return FALSE;
    if (!ensure_dir(HOME_BASE)) return FALSE;
    if (!ensure_dir(home)) return FALSE;

    user_add_entry(name_norm, (UINT16)(max_uid + 1), home, pass_norm);
    return user_sync_passwd();
}

BOOL user_login(const char *name, const char *password) {
    char name_norm[USER_NAME_MAX];
    const SIMA_USER *u;

    if (!user_normalize_name(name, name_norm, (UINT16)sizeof(name_norm))) return FALSE;
    (void)user_load_passwd();
    u = user_find(name_norm);
    if (!u) return FALSE;
    if (!password) password = "";
    if (u->pass[0] != '\0' && sima_strcmp(u->pass, password) != STRC_SAME) return FALSE;
    if (u->pass[0] == '\0' && password[0] != '\0') return FALSE;
    return user_set_current(u);
}

void user_logout(void) {
    sima_memclr((char*)&g_current, (UINT16)sizeof(g_current));
    env_set_public("USER", "");
    env_set_public("HOME", "/");
    env_set_public("PWD", "/");
    env_set_public("OLDPWD", "/");
    (void)fs_cd("/");
}

BOOL user_change_password(const char *old_password, const char *new_password) {
    const SIMA_USER *current = user_current();
    char old_norm[USER_PASS_MAX];
    char new_norm[USER_PASS_MAX];
    SIMA_USER *target = (SIMA_USER*)0;
    UINT16 i;

    if (!current) return FALSE;
    if (!old_password) old_password = "";
    if (!user_copy_password(old_password, old_norm, (UINT16)sizeof(old_norm), TRUE)) return FALSE;
    if (!user_copy_password(new_password, new_norm, (UINT16)sizeof(new_norm), FALSE)) return FALSE;

    (void)user_load_passwd();
    for (i = 0; i < USER_MAX; ++i) {
        if (!g_users[i].used) continue;
        if (sima_strcmp(g_users[i].name, current->name) == STRC_SAME) {
            target = &g_users[i];
            break;
        }
    }
    if (!target) return FALSE;

    if (target->pass[0] != '\0' && sima_strcmp(target->pass, old_norm) != STRC_SAME) return FALSE;
    if (target->pass[0] == '\0' && old_norm[0] != '\0') return FALSE;

    if (!sima_strcpy(target->pass, (UINT16)sizeof(target->pass), new_norm)) return FALSE;
    if (!user_sync_passwd()) return FALSE;
    return user_set_current(target);
}

const SIMA_USER *user_current(void) {
    if (!g_current.used) return (const SIMA_USER*)0;
    return &g_current;
}

BOOL user_is_logged_in(void) {
    return g_current.used ? TRUE : FALSE;
}

UINT16 user_count(void) {
    UINT16 i;
    UINT16 n = 0;
    for (i = 0; i < USER_MAX; ++i) {
        if (g_users[i].used) ++n;
    }
    return n;
}

const SIMA_USER *user_at(UINT16 index) {
    UINT16 i;
    UINT16 n = 0;
    for (i = 0; i < USER_MAX; ++i) {
        if (!g_users[i].used) continue;
        if (n == index) return &g_users[i];
        ++n;
    }
    return (const SIMA_USER*)0;
}
