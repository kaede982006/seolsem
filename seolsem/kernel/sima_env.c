#include "sima_env.h"
#include "sima_fs.h"
#include "sima_mem.h"

#define ENV_MAX_VARS 8
#define ENV_NAME_MAX 12
#define ENV_VALUE_MAX 64

typedef struct {
    BOOL used;
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
} ENV_ENTRY;

static ENV_ENTRY env_table[ENV_MAX_VARS];

static void env_clear(void) {
    sima_memclr((char*)env_table, (UINT16)sizeof(env_table));
}

static void env_trim_right(char *text) {
    UINT16 len;
    if (!text) return;
    len = sima_strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[len - 1] = '\0';
        len--;
    }
}

static char *env_trim_left(char *text) {
    if (!text) return text;
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void env_set(const char *name, const char *value) {
    UINT16 i;
    UINT16 slot = ENV_MAX_VARS;
    BOOL append_path = FALSE;

    if (!name || !value || name[0] == '\0') return;

    for (i = 0; i < ENV_MAX_VARS; ++i) {
        if (env_table[i].used &&
            sima_strcmp(env_table[i].name, name) == STRC_SAME) {
            slot = i;
            if (sima_strcmp(name, "PATH") == STRC_SAME) {
                append_path = TRUE;
            }
            break;
        }
        if (!env_table[i].used && slot == ENV_MAX_VARS) {
            slot = i;
        }
    }

    if (slot == ENV_MAX_VARS) return;
    if (append_path) {
        char merged[ENV_VALUE_MAX];
        BOOL ok = TRUE;

        ok = ok && sima_strcpy(merged, (UINT16)sizeof(merged), env_table[slot].value);
        if (ok && merged[0] != '\0') {
            ok = sima_strcat(merged, (UINT16)sizeof(merged), ";");
        }
        ok = ok && sima_strcat(merged, (UINT16)sizeof(merged), value);
        if (!ok) return;
        sima_strcpy(env_table[slot].value, (UINT16)sizeof(env_table[slot].value), merged);
        return;
    }
    env_table[slot].used = TRUE;
    sima_strcpy(env_table[slot].name, (UINT16)sizeof(env_table[slot].name), name);
    sima_strcpy(env_table[slot].value, (UINT16)sizeof(env_table[slot].value), value);
}

static void env_parse_line(char *line) {
    char *eq;
    char *name;
    char *value;

    if (!line) return;
    name = env_trim_left(line);
    if (name[0] == '\0' || name[0] == '#') return;

    eq = (char*)sima_strchr(name, '=');
    if (!eq) return;
    *eq = '\0';
    value = env_trim_left(eq + 1);
    env_trim_right(name);
    env_trim_right(value);
    if (name[0] == '\0') return;
    env_set(name, value);
}

BOOL env_init(void) {
    UINT8 buffer[FS_BLOCK_SIZE + 1];
    UINT32 size = 0;
    UINT32 i = 0;
    UINT32 start = 0;

    env_clear();
    if (!fs_read("/XENV.ENV", buffer, FS_BLOCK_SIZE, &size)) {
        return FALSE;
    }

    buffer[size] = '\0';
    for (i = 0; i <= size; ++i) {
        if (buffer[i] == '\r') buffer[i] = '\0';
        if (buffer[i] == '\n' || buffer[i] == '\0') {
            env_parse_line((char*)&buffer[start]);
            start = i + 1;
        }
    }
    return TRUE;
}

const char *env_get(const char *name) {
    UINT16 i;
    if (!name) return (const char*)0;
    for (i = 0; i < ENV_MAX_VARS; ++i) {
        if (env_table[i].used &&
            sima_strcmp(env_table[i].name, name) == STRC_SAME) {
            return env_table[i].value;
        }
    }
    return (const char*)0;
}
