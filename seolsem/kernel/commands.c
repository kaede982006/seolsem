#include "commands.h"
#include "sima_conv.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_io.h"
#include "sima_program.h"
#include "sima_env.h"
#include "sima_ide.h"
#include "sima_user.h"
#include "sima_perm.h"

static char wrong_command_message[128];
static char message_buffer[160];
static char argv_buffer[64];
static PROGRAM_HANDLE g_loaded_handle = PROGRAM_HANDLE_INVALID;
static void print_simple(const char *text);
static void print_program_result(PROGRAM_RESULT rc);
static BOOL run_exec(const char *name);

static const char* skip_tokens(const char *buffer, UINT16 count) {
    UINT16 i = 0;
    while (buffer[i] == ' ') ++i;
    while (count > 0 && buffer[i] != '\0') {
        while (buffer[i] != '\0' && buffer[i] != ' ') ++i;
        while (buffer[i] == ' ') ++i;
        --count;
    }
    return &buffer[i];
}

static BOOL is_space_char(char ch) {
    return (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
}

/* Minimal Linux-like argv parser:
 * - Splits on spaces/tabs
 * - Supports single/double quotes to include spaces
 * - Treats backslash as an escape ONLY for whitespace and quotes (so paths like "U\\A" keep the '\\')
 * - Edits the input buffer in-place and writes tokens back into it (dst <= src).
 */
static UINT16 parse_argv(char *line, char **argv, UINT16 argv_cap) {
    UINT16 argc = 0;
    char *src;
    char *dst;

    BOOL in_single = FALSE;
    BOOL in_double = FALSE;

    if (!line || !argv || argv_cap == 0) return 0;
    src = line;
    dst = line;

    while (*src != '\0') {
        while (is_space_char(*src)) ++src;
        if (*src == '\0') break;
        if (argc >= argv_cap) break;

        argv[argc++] = dst;
        in_single = FALSE;
        in_double = FALSE;

        while (*src != '\0') {
            char ch = *src;
            if (!in_single && !in_double && is_space_char(ch)) {
                /* Consume the delimiter so dst==src in-place parsing keeps progressing. */
                ++src;
                break;
            }

            ++src;
            if (!in_double && ch == '\'') {
                in_single = (BOOL)!in_single;
                continue;
            }
            if (!in_single && ch == '"') {
                in_double = (BOOL)!in_double;
                continue;
            }

            if (!in_single && ch == '\\') {
                char next = *src;
                if (next == ' ' || next == '\t' || next == '\\' || next == '"' || next == '\'') {
                    if (next != '\0') {
                        ch = next;
                        ++src;
                    }
                }
            }

            *dst++ = ch;
        }

        *dst++ = '\0';
        while (is_space_char(*src)) ++src;
    }

    return argc;
}

/* Moved to sima_perm.c as perm_is_root() */

/* Permission helper (now uses sima_perm.c) */
static BOOL deny_if_forbidden(const char *path, BOOL allow_bin) {
    if (perm_allow_path(path, allow_bin)) return FALSE;
    print_simple("Permission denied.");
    return TRUE;
}

static BOOL has_path_separator(const char *path) {
    if (!path) return FALSE;
    while (*path) {
        if (*path == '/' || *path == '\\') return TRUE;
        ++path;
    }
    return FALSE;
}

static void print_simple(const char *text) {
    sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
    sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), text);
    print_message(message_buffer);
}

static BOOL run_diskinfo(void) {
    UINT32 phys_sectors = 0;
    UINT32 vol_sectors = 0;
    UINT32 vol_base_lba = 0;
    UINT8 vol_spc = 0;
    char line[128];

    print_message("Disk information:");

    if (ide_identify_total_sectors(&phys_sectors)) {
        UINT16 hi = (UINT16)(phys_sectors >> 16);
        UINT16 lo = (UINT16)(phys_sectors & 0xFFFF);
        char hi_s[8];
        char lo_s[8];

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Physical sectors: ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2); /* skip "0x" */
        print_message(line);
    } else {
        print_message("  Physical sectors: (ATA IDENTIFY failed)");
    }

    if (fs_get_volume_info32(&vol_sectors, &vol_spc, &vol_base_lba)) {
        UINT16 base_hi = (UINT16)(vol_base_lba >> 16);
        UINT16 base_lo = (UINT16)(vol_base_lba & 0xFFFF);
        UINT16 vol_hi = (UINT16)(vol_sectors >> 16);
        UINT16 vol_lo = (UINT16)(vol_sectors & 0xFFFF);
        UINT16 approx_mib = (UINT16)(vol_sectors >> 11); /* sectors/2048 */
        char hi_s[8];
        char lo_s[8];
        char n1[8];
        char n2[8];
        UINT16 cluster_bytes = (UINT16)((UINT16)vol_spc << 9);

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(base_hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(base_lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Partition base LBA: ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2);
        print_message(line);

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(vol_hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(vol_lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(n1, (UINT16)sizeof(n1));
        sima_memclr(n2, (UINT16)sizeof(n2));
        sima_utoa(approx_mib, n1, (UINT16)sizeof(n1), 10);
        sima_utoa(cluster_bytes, n2, (UINT16)sizeof(n2), 10);

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Volume sectors (BPB): ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2);
        sima_strcat(line, (UINT16)sizeof(line), " (~");
        sima_strcat(line, (UINT16)sizeof(line), n1);
        sima_strcat(line, (UINT16)sizeof(line), " MiB)");
        print_message(line);

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Sectors/cluster: ");
        sima_memclr(n1, (UINT16)sizeof(n1));
        sima_utoa(vol_spc, n1, (UINT16)sizeof(n1), 10);
        sima_strcat(line, (UINT16)sizeof(line), n1);
        sima_strcat(line, (UINT16)sizeof(line), " (");
        sima_strcat(line, (UINT16)sizeof(line), n2);
        sima_strcat(line, (UINT16)sizeof(line), " bytes)");
        print_message(line);
    } else {
        print_message("  Volume info (BPB): unavailable");
    }

    return TRUE;
}

BOOL wrong_command_usage(char* buffer) {
	
	sima_memclr(wrong_command_message, (UINT16)sizeof(wrong_command_message));
	sima_strcpy(wrong_command_message,sizeof(wrong_command_message),"Wrong command usage: ");
	sima_strcat(wrong_command_message, sizeof(wrong_command_message), buffer);
	print_message(wrong_command_message);
	return TRUE;
}

static BOOL run_cls(void) {
    clear_screen();
    return TRUE;
}

static BOOL has_xef_extension(const char *name) {
    UINT16 len;
    UINT8 c0;
    UINT8 c1;
    UINT8 c2;
    UINT8 c3;
    if (!name) return FALSE;
    len = sima_strlen(name);
    if (len < 4) return FALSE;
    c0 = (UINT8)name[len - 4];
    c1 = (UINT8)name[len - 3];
    c2 = (UINT8)name[len - 2];
    c3 = (UINT8)name[len - 1];
    if (c1 >= 'a' && c1 <= 'z') c1 = (UINT8)(c1 - 'a' + 'A');
    if (c2 >= 'a' && c2 <= 'z') c2 = (UINT8)(c2 - 'a' + 'A');
    if (c3 >= 'a' && c3 <= 'z') c3 = (UINT8)(c3 - 'a' + 'A');
    return (c0 == '.' && c1 == 'X' && c2 == 'E' && c3 == 'F') ? TRUE : FALSE;
}

static void print_program_result(PROGRAM_RESULT rc) {
    char msg[96];
    if (rc == PROGRAM_OK) return;
    sima_memclr(msg, (UINT16)sizeof(msg));
    (void)program_loader_result_message(rc, msg, (UINT16)sizeof(msg));
    print_message(msg);
}

static PROGRAM_RESULT load_program_candidate(const char *dir, const char *name, PROGRAM_HANDLE *out_handle) {
    if (!name || name[0] == '\0' || !out_handle) return PROGRAM_ERR_INVALID_ARG;
    if (dir && dir[0] != '\0') {
        return program_loader_open_in_dir(dir, name, out_handle);
    }
    return program_loader_open(name, out_handle);
}

static PROGRAM_RESULT load_program_with_optional_ext(const char *dir, const char *name, PROGRAM_HANDLE *out_handle) {
    PROGRAM_RESULT rc;
    PROGRAM_RESULT best;
    char with_ext[FS_PATH_MAX];

    if (!name || name[0] == '\0' || !out_handle) return PROGRAM_ERR_INVALID_ARG;

    rc = load_program_candidate(dir, name, out_handle);
    if (rc == PROGRAM_OK) return PROGRAM_OK;
    if (rc != PROGRAM_ERR_NOT_FOUND && rc != PROGRAM_ERR_NOT_EXECUTABLE) return rc;
    best = rc;

    if (!has_xef_extension(name)) {
        sima_memclr(with_ext, (UINT16)sizeof(with_ext));
        if (!sima_strcpy(with_ext, (UINT16)sizeof(with_ext), name)) return PROGRAM_ERR_INVALID_ARG;
        if (!sima_strcat(with_ext, (UINT16)sizeof(with_ext), ".XEF")) return PROGRAM_ERR_INVALID_ARG;
        rc = load_program_candidate(dir, with_ext, out_handle);
        if (rc == PROGRAM_OK) return PROGRAM_OK;
        if (rc != PROGRAM_ERR_NOT_FOUND && rc != PROGRAM_ERR_NOT_EXECUTABLE) return rc;
        if (best == PROGRAM_ERR_NOT_FOUND) best = rc;
    }
    return best;
}

static PROGRAM_RESULT load_program_with_path(const char *name, PROGRAM_HANDLE *out_handle) {
    const char *path;
    char path_buffer[64];
    char abs_dir[64];
    char *dirs[8];
    UINT16 count;
    UINT16 i;
    PROGRAM_RESULT rc;
    PROGRAM_RESULT best = PROGRAM_ERR_NOT_FOUND;

    if (!name || !out_handle) return PROGRAM_ERR_INVALID_ARG;

    rc = load_program_with_optional_ext((const char*)0, name, out_handle);
    if (rc == PROGRAM_OK) return PROGRAM_OK;
    if (rc != PROGRAM_ERR_NOT_FOUND && rc != PROGRAM_ERR_NOT_EXECUTABLE) return rc;
    best = rc;

    path = env_get("PATH");
    if (!path) return best;
    if (!sima_strcpy(path_buffer, (UINT16)sizeof(path_buffer), path)) {
        return PROGRAM_ERR_INVALID_ARG;
    }
    count = sima_strcspl(path_buffer, ';', dirs, (UINT16)8);
    for (i = 0; i < count; ++i) {
        rc = load_program_with_optional_ext(dirs[i], name, out_handle);
        if (rc == PROGRAM_OK) return PROGRAM_OK;
        if (rc != PROGRAM_ERR_NOT_FOUND && rc != PROGRAM_ERR_NOT_EXECUTABLE) return rc;
        if (best == PROGRAM_ERR_NOT_FOUND) best = rc;
        if (dirs[i][0] != '/' && dirs[i][0] != '\\') {
            sima_memclr(abs_dir, (UINT16)sizeof(abs_dir));
            if (!sima_strcpy(abs_dir, (UINT16)sizeof(abs_dir), "/")) continue;
            if (!sima_strcat(abs_dir, (UINT16)sizeof(abs_dir), dirs[i])) continue;
            rc = load_program_with_optional_ext(abs_dir, name, out_handle);
            if (rc == PROGRAM_OK) return PROGRAM_OK;
            if (rc != PROGRAM_ERR_NOT_FOUND && rc != PROGRAM_ERR_NOT_EXECUTABLE) return rc;
            if (best == PROGRAM_ERR_NOT_FOUND) best = rc;
        }
    }
    return best;
}

static UINT8 man_upper(UINT8 ch) {
    if (ch >= 'a' && ch <= 'z') return (UINT8)(ch - 'a' + 'A');
    return ch;
}

static BOOL man_topic_eq(const char *lhs, const char *rhs) {
    UINT16 i = 0;
    if (!lhs || !rhs) return FALSE;
    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (man_upper((UINT8)lhs[i]) != man_upper((UINT8)rhs[i])) return FALSE;
        ++i;
    }
    return (lhs[i] == '\0' && rhs[i] == '\0') ? TRUE : FALSE;
}

static BOOL man_parse_section(const char *text, UINT8 *out_section) {
    if (!text || text[0] == '\0' || text[1] != '\0') return FALSE;
    if (text[0] == '1') {
        if (out_section) *out_section = 1;
        return TRUE;
    }
    if (text[0] == '7') {
        if (out_section) *out_section = 7;
        return TRUE;
    }
    return FALSE;
}

static BOOL man_wait_next(void) {
    for (;;) {
        UINT16 key = read_key();
        UINT8 ascii = (UINT8)(key & 0xFF);
        if (ascii == 'q' || ascii == 'Q' || ascii == 27) return FALSE;
        if (ascii == ' ' || ascii == 13) return TRUE;
    }
}

static BOOL man_show_page(const char *title, const char *const *lines, UINT16 line_count) {
    UINT16 index = 0;
    const UINT16 body_lines = 18;

    while (index < line_count) {
        UINT16 shown = 0;
        clear_screen();
        print_message("Seolsem Manual");
        print_message(title);
        print_message("");
        while (index < line_count && shown < body_lines) {
            print_message(lines[index]);
            ++index;
            ++shown;
        }
        if (index >= line_count) break;
        print_message("");
        print_message("[SPACE/ENTER] next  [Q/ESC] quit");
        if (!man_wait_next()) return TRUE;
    }
    return TRUE;
}

static const char *const man_command_names[] = {
    "ver", "help", "man", "cls",
    "ls", "cd", "pwd", "cat", "write", "edit", "rm", "rmdir", "mkdir",
    "load", "run", "exec", "sync", "diskinfo", "calc", "hello",
    "whoami", "id", "users", "useradd", "passwd", "logout"
};

static const char *const man_topic_names[] = {
    "system", "fs", "exec", "user", "display", "tools"
};

static BOOL man_in_table(const char *topic, const char *const *table, UINT16 table_count) {
    UINT16 i;
    if (!topic || !table) return FALSE;
    for (i = 0; i < table_count; ++i) {
        if (man_topic_eq(topic, table[i])) return TRUE;
    }
    return FALSE;
}

static void man_print_whatis_entry(const char *name, UINT8 section) {
    char sec_text[2];
    sec_text[0] = (section == 7) ? '7' : '1';
    sec_text[1] = '\0';

    sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
    sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), name);
    sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " (");
    sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), sec_text);
    sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), ") - ");
    if (section == 7) {
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), "topic");
    } else {
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), "command");
    }
    print_message(message_buffer);
}

static BOOL man_lookup(const char *topic, const char **name_out, UINT8 *section_out) {
    UINT16 i;
    if (!topic || topic[0] == '\0') return FALSE;

    for (i = 0; i < (UINT16)(sizeof(man_command_names) / sizeof(man_command_names[0])); ++i) {
        if (man_topic_eq(topic, man_command_names[i])) {
            if (name_out) *name_out = man_command_names[i];
            if (section_out) *section_out = 1;
            return TRUE;
        }
    }
    for (i = 0; i < (UINT16)(sizeof(man_topic_names) / sizeof(man_topic_names[0])); ++i) {
        if (man_topic_eq(topic, man_topic_names[i])) {
            if (name_out) *name_out = man_topic_names[i];
            if (section_out) *section_out = 7;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL run_man_topic(const char *topic) {
    static const char *const man_index[] = {
        "man [section] <topic>",
        "man -k <key> | man -f <name>",
        "sections: 1=cmd, 7=topic",
        "ex: man 1 ls"
    };
    static const char *const man_system[] = {
        "system: ver help man diskinfo"
    };
    static const char *const man_fs[] = {
        "fs: ls cd pwd cat write edit rm rmdir mkdir sync"
    };
    static const char *const man_exec[] = {
        "exec: load <f> | run | exec <f>",
        "single active process only"
    };
    static const char *const man_user[] = {
        "user: whoami id users useradd passwd logout",
        "useradd: root only"
    };
    static const char *const man_display[] = {
        "display: cls"
    };
    static const char *const man_calc[] = {
        "calc: a+ s- d* f/ g% h& j| k^ l<< ;>>",
        "q/ESC quit"
    };
    static const char *const man_hello[] = {
        "hello: run HELLO.XEF from PATH"
    };
    static const char *const man_cd[] = {
        "cd: [path] | / | - | (HOME)"
    };

    if (!topic || topic[0] == '\0') {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }

    if (man_topic_eq(topic, "INDEX")) {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }
    if (man_topic_eq(topic, "SYSTEM") || man_topic_eq(topic, "VER") || man_topic_eq(topic, "HELP") || man_topic_eq(topic, "MAN") || man_topic_eq(topic, "DISKINFO")) {
        return man_show_page("System", man_system, (UINT16)(sizeof(man_system) / sizeof(man_system[0])));
    }
    if (man_topic_eq(topic, "FS") || man_topic_eq(topic, "LS") || man_topic_eq(topic, "PWD") ||
        man_topic_eq(topic, "CAT") || man_topic_eq(topic, "WRITE") || man_topic_eq(topic, "EDIT") ||
        man_topic_eq(topic, "RM") || man_topic_eq(topic, "RMDIR") || man_topic_eq(topic, "MKDIR") ||
        man_topic_eq(topic, "SYNC")) {
        return man_show_page("Filesystem", man_fs, (UINT16)(sizeof(man_fs) / sizeof(man_fs[0])));
    }
    if (man_topic_eq(topic, "CD")) {
        return man_show_page("cd", man_cd, (UINT16)(sizeof(man_cd) / sizeof(man_cd[0])));
    }
    if (man_topic_eq(topic, "EXEC") || man_topic_eq(topic, "LOAD") || man_topic_eq(topic, "RUN")) {
        return man_show_page("Program Execution", man_exec, (UINT16)(sizeof(man_exec) / sizeof(man_exec[0])));
    }
    if (man_topic_eq(topic, "USER") || man_topic_eq(topic, "WHOAMI") || man_topic_eq(topic, "ID") ||
        man_topic_eq(topic, "USERS") || man_topic_eq(topic, "USERADD") ||
        man_topic_eq(topic, "PASSWD") || man_topic_eq(topic, "LOGOUT")) {
        return man_show_page("User", man_user, (UINT16)(sizeof(man_user) / sizeof(man_user[0])));
    }
    if (man_topic_eq(topic, "DISPLAY") || man_topic_eq(topic, "CLS")) {
        return man_show_page("Display", man_display, (UINT16)(sizeof(man_display) / sizeof(man_display[0])));
    }
    if (man_topic_eq(topic, "TOOLS") || man_topic_eq(topic, "CALC")) {
        return man_show_page("Calculator", man_calc, (UINT16)(sizeof(man_calc) / sizeof(man_calc[0])));
    }
    if (man_topic_eq(topic, "HELLO")) {
        return man_show_page("Hello", man_hello, (UINT16)(sizeof(man_hello) / sizeof(man_hello[0])));
    }

    print_simple("No manual entry.");
    return TRUE;
}

static BOOL run_man(const char *topic) {
    return run_man_topic(topic);
}

static BOOL run_man_with_section(const char *section, const char *topic) {
    UINT8 sec = 0;
    if (!section || !topic) return FALSE;
    if (!man_parse_section(section, &sec)) {
        print_simple("Supported sections: 1, 7");
        return TRUE;
    }
    if (sec == 1) {
        if (man_in_table(topic, man_topic_names, (UINT16)(sizeof(man_topic_names) / sizeof(man_topic_names[0])))) {
            print_simple("No manual entry.");
            return TRUE;
        }
        return run_man_topic(topic);
    }

    if (man_in_table(topic, man_topic_names, (UINT16)(sizeof(man_topic_names) / sizeof(man_topic_names[0])))) {
        return run_man_topic(topic);
    }

    print_simple("No manual entry.");
    return TRUE;
}

static BOOL run_man_whatis(const char *topic) {
    const char *name = (const char*)0;
    UINT8 section = 0;
    if (!topic || topic[0] == '\0') return FALSE;
    if (!man_lookup(topic, &name, &section)) {
        print_simple("No manual entry.");
        return TRUE;
    }
    man_print_whatis_entry(name, section);
    return TRUE;
}

static BOOL run_man_apropos(const char *keyword) {
    UINT16 i;
    BOOL found = FALSE;
    if (!keyword || keyword[0] == '\0') return FALSE;

    for (i = 0; i < (UINT16)(sizeof(man_command_names) / sizeof(man_command_names[0])); ++i) {
        if (sima_strstr(man_command_names[i], keyword) != (const char*)0) {
            man_print_whatis_entry(man_command_names[i], 1);
            found = TRUE;
        }
    }
    for (i = 0; i < (UINT16)(sizeof(man_topic_names) / sizeof(man_topic_names[0])); ++i) {
        if (sima_strstr(man_topic_names[i], keyword) != (const char*)0) {
            man_print_whatis_entry(man_topic_names[i], 7);
            found = TRUE;
        }
    }
    if (!found) {
        print_simple("Nothing appropriate.");
    }
    return TRUE;
}

void run_help() {
    print_message("commands:");
    print_message("  ver help man cls ls cd pwd cat write edit rm rmdir mkdir");
    print_message("  load run exec sync diskinfo calc hello whoami id users useradd");
    print_message("  passwd logout");
}

static BOOL cd_expand_user(const char *path, char *out, UINT16 out_cap) {
    const char *home;
    const char *rest;

    if (!out || out_cap == 0) return FALSE;
    out[0] = '\0';

    if (!path) return FALSE;
    if (path[0] != '~') {
        return sima_strcpy(out, out_cap, path);
    }

    home = env_get("HOME");
    if (!home || home[0] == '\0') {
        /* No HOME set; keep the original path (e.g., "~" literal). */
        return sima_strcpy(out, out_cap, path);
    }

    rest = path + 1; /* skip '~' */
    /* Support "~" and "~/" only (no ~user). */
    if (rest[0] != '\0' && rest[0] != '/' && rest[0] != '\\') {
        return sima_strcpy(out, out_cap, path);
    }

    if (!sima_strcpy(out, out_cap, home)) return FALSE;
    if (rest[0] == '\0') return TRUE;

    /* Join carefully to avoid '//' when HOME is '/'. */
    if (out[0] == '/' && out[1] == '\0') {
        return sima_strcat(out, out_cap, rest + 1);
    }
    return sima_strcat(out, out_cap, rest);
}

static BOOL run_ls(const char *path) {
    FS_DIR dir;
    FS_DIRENT entry;
    char size_buffer[16];
    UINT16 guard = 0;

    if (deny_if_forbidden(path && path[0] ? path : ".", FALSE)) return TRUE;

    if (!fs_dir_open(path && path[0] ? path : ".", &dir)) {
        print_simple("Path not found.");
        return TRUE;
    }

    while (fs_dir_read(&dir, &entry)) {
        if (++guard > 256U) {
            print_simple("Directory listing aborted.");
            break;
        }
        sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
        sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), entry.name);
        if (entry.is_dir) {
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " [DIR]");
        } else {
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " ");
            sima_utoa((UINT16)entry.size, size_buffer, (UINT16)sizeof(size_buffer), 10);
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), size_buffer);
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " bytes");
            if (entry.is_exec) {
                sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " [EXEC]");
            }
        }
        print_message(message_buffer);
    }
    return TRUE;
}

static BOOL run_pwd(void) {
    char cwd[FS_PATH_MAX];
    sima_memclr(cwd, (UINT16)sizeof(cwd));
    if (!fs_get_cwd(cwd, (UINT16)sizeof(cwd))) {
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "/");
    }
    print_message(cwd);
    return TRUE;
}

static BOOL run_cd(const char *path) {
    char old_path[FS_PATH_MAX];
    char target[FS_PATH_MAX];
    const char *home;
    char new_path[FS_PATH_MAX];
    static char prev_path[FS_PATH_MAX] = "";
    static BOOL has_prev = FALSE;
    BOOL swap_prev = FALSE;

    sima_memclr(old_path, (UINT16)sizeof(old_path));
    fs_get_cwd(old_path, (UINT16)sizeof(old_path));

    if (!path || path[0] == '\0') {
        home = env_get("HOME");
        path = (home && home[0] != '\0') ? home : "/";
    } else if (sima_strcmp(path, "-") == STRC_SAME) {
        if (!has_prev || prev_path[0] == '\0') {
            print_simple("No previous directory.");
            return TRUE;
        }
        path = prev_path;
        swap_prev = TRUE;
    }

    sima_memclr(target, (UINT16)sizeof(target));
    if (!cd_expand_user(path, target, (UINT16)sizeof(target))) {
        print_simple("Unable to change directory.");
        return TRUE;
    }
    if (deny_if_forbidden(target, FALSE)) return TRUE;

    if (!fs_cd(target)) {
        print_simple("Directory not found.");
        return TRUE;
    }

    /* Keep PWD/OLDPWD in sync for Linux-like behavior. */
    sima_memclr(new_path, (UINT16)sizeof(new_path));
    if (!fs_get_cwd(new_path, (UINT16)sizeof(new_path))) {
        sima_strcpy(new_path, (UINT16)sizeof(new_path), "/");
    }
    env_set_public("OLDPWD", old_path);
    env_set_public("PWD", new_path);

    /* Linux-like "cd -" swaps current and previous directories. */
    if (swap_prev) {
        sima_strcpy(prev_path, (UINT16)sizeof(prev_path), old_path);
        has_prev = TRUE;
        return run_pwd();
    }

    sima_strcpy(prev_path, (UINT16)sizeof(prev_path), old_path);
    has_prev = TRUE;
    return TRUE;
}

static BOOL run_cat(const char *name) {
    UINT8 data[FS_BLOCK_SIZE + 1];
    UINT32 size;

    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_read(name, data, FS_BLOCK_SIZE, &size)) {
        print_simple("Unable to read file.");
        return TRUE;
    }
    data[size] = '\0';
    print_message((const char*)data);
    return TRUE;
}

static BOOL run_write(const char *name, const char *data) {
    UINT16 size;
    if (!name || !data) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    size = sima_strlen(data);
    if (!fs_write(name, (const UINT8*)data, (UINT32)size)) {
        print_simple("Unable to write file.");
        return TRUE;
    }
    print_simple("Write complete.");
    return TRUE;
}

static BOOL run_builtin_program(const char *primary, const char *fallback, const char *arg) {
    PROGRAM_RESULT rc;
    PROGRAM_HANDLE handle = PROGRAM_HANDLE_INVALID;
    UINT16 status = 1;

    rc = load_program_with_path(primary, &handle);
    if (rc != PROGRAM_OK && fallback && fallback[0] != '\0') {
        rc = load_program_with_path(fallback, &handle);
    }
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }

    g_loaded_handle = handle;

    rc = program_loader_exec(handle, arg, &status);
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }
    return TRUE;
}

static BOOL run_edit(const char *name) {
    const char *arg = (const char*)0;

    if (name && name[0] != '\0') {
        if (deny_if_forbidden(name, FALSE)) return TRUE;
        arg = name;
    }
    return run_builtin_program("BIN/EDIT.XEF", "EDIT.XEF", arg);
}

static BOOL run_rm(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_delete(name)) {
        print_simple("Unable to delete file.");
        return TRUE;
    }
    print_simple("File deleted.");
    return TRUE;
}

static BOOL run_rmdir(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_delete_dir(name)) {
        print_simple("Unable to remove directory.");
        return TRUE;
    }
    print_simple("Directory removed.");
    return TRUE;
}

static BOOL run_mkdir(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_create_dir(name)) {
        print_simple("Unable to create directory.");
        return TRUE;
    }
    print_simple("Directory created.");
    return TRUE;
}

static PROGRAM_RESULT prepare_loaded_program(const char *name, PROGRAM_HANDLE *out_handle) {
    PROGRAM_RESULT rc;
    PROGRAM_HANDLE handle = PROGRAM_HANDLE_INVALID;

    if (!name || name[0] == '\0') return PROGRAM_ERR_INVALID_ARG;
    rc = load_program_with_path(name, &handle);
    if (rc != PROGRAM_OK) return rc;
    g_loaded_handle = handle;
    if (out_handle) *out_handle = handle;
    return PROGRAM_OK;
}

static BOOL run_load(const char *name) {
    PROGRAM_RESULT rc;

    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;

    rc = prepare_loaded_program(name, (PROGRAM_HANDLE*)0);
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }
    print_simple("Program loaded.");
    return TRUE;
}

static BOOL run_program(void) {
    PROGRAM_RESULT rc;
    UINT16 status = 1;

    if (g_loaded_handle == PROGRAM_HANDLE_INVALID) {
        print_simple("No program loaded.");
        return TRUE;
    }

    rc = program_loader_exec(g_loaded_handle,
                             (const char*)0,
                             &status);
    if (rc == PROGRAM_ERR_BAD_HANDLE) {
        g_loaded_handle = PROGRAM_HANDLE_INVALID;
        print_simple("No program loaded.");
        return TRUE;
    }
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }

    return TRUE;
}

static BOOL run_exec(const char *name) {
    PROGRAM_RESULT rc;
    PROGRAM_HANDLE handle = PROGRAM_HANDLE_INVALID;
    UINT16 status = 1;

    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;

    rc = prepare_loaded_program(name, &handle);
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }
    rc = program_loader_exec(handle, (const char*)0, &status);
    if (rc != PROGRAM_OK) {
        print_program_result(rc);
        return TRUE;
    }
    return TRUE;
}

static BOOL run_sync(void) {
    if (!fs_sync()) {
        print_simple("Disk sync failed.");
        return TRUE;
    }
    print_simple("Filesystem synced to disk.");
    return TRUE;
}

static BOOL run_calc(void) {
    return run_builtin_program("BIN/CALC.XEF", "CALC.XEF", (const char*)0);
}

static BOOL run_hello(void) {
    return run_builtin_program("BIN/HELLO.XEF", "HELLO.XEF", (const char*)0);
}

static BOOL run_whoami(void) {
    const SIMA_USER *u = user_current();
    if (!u) {
        print_simple("ROOT");
        return TRUE;
    }
    print_message(u->name);
    return TRUE;
}

static BOOL run_users(void) {
    UINT16 i;
    const SIMA_USER *u;
    char uid_s[8];
    char line[128];

    print_message("Users:");
    for (i = 0; ; ++i) {
        u = user_at(i);
        if (!u) break;
        sima_memclr(uid_s, (UINT16)sizeof(uid_s));
        sima_utoa(u->uid, uid_s, (UINT16)sizeof(uid_s), 10);
        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  ");
        sima_strcat(line, (UINT16)sizeof(line), u->name);
        sima_strcat(line, (UINT16)sizeof(line), " uid=");
        sima_strcat(line, (UINT16)sizeof(line), uid_s);
        sima_strcat(line, (UINT16)sizeof(line), " home=");
        sima_strcat(line, (UINT16)sizeof(line), u->home);
        print_message(line);
    }
    return TRUE;
}

static BOOL run_useradd(const char *name, const char *password) {
    if (!name || name[0] == '\0') return FALSE;
    if (!perm_is_root()) {
        print_simple("Permission denied. Only root can add users.");
        return TRUE;
    }
    if (!user_add(name, password)) {
        print_simple("Unable to add user.");
        return TRUE;
    }
    print_simple("User added.");
    return TRUE;
}

static BOOL run_passwd(void) {
    char old_buf[USER_PASS_MAX];
    char new_buf[USER_PASS_MAX];
    char confirm_buf[USER_PASS_MAX];

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));
    sima_memclr(confirm_buf, (UINT16)sizeof(confirm_buf));

    wait_prompt_masked("current password: ", old_buf);
    wait_prompt_masked("new password: ", new_buf);
    wait_prompt_masked("confirm new password: ", confirm_buf);

    if (new_buf[0] == '\0') {
        print_simple("New password cannot be empty.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        sima_memclr(confirm_buf, (UINT16)sizeof(confirm_buf));
        return TRUE;
    }

    if (sima_strcmp(new_buf, confirm_buf) != STRC_SAME) {
        print_simple("New password confirmation mismatch.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        sima_memclr(confirm_buf, (UINT16)sizeof(confirm_buf));
        return TRUE;
    }

    if (!user_change_password(old_buf, new_buf)) {
        print_simple("Unable to update password.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        sima_memclr(confirm_buf, (UINT16)sizeof(confirm_buf));
        return TRUE;
    }

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));
    sima_memclr(confirm_buf, (UINT16)sizeof(confirm_buf));
    print_simple("Password updated.");
    return TRUE;
}

static BOOL run_logout(void) {
    user_logout();
    print_simple("Logged out.");
    return TRUE;
}

static BOOL run_id(void) {
    const SIMA_USER *u = user_current();
    char uid_s[8];
    char line[64];
    UINT16 uid = u ? u->uid : 0;

    sima_memclr(uid_s, (UINT16)sizeof(uid_s));
    sima_utoa(uid, uid_s, (UINT16)sizeof(uid_s), 10);
    sima_memclr(line, (UINT16)sizeof(line));
    sima_strcpy(line, (UINT16)sizeof(line), "uid=");
    sima_strcat(line, (UINT16)sizeof(line), uid_s);
    print_message(line);
    return TRUE;
}

BOOL run_buffer(char *buffer)
{
    UINT16 argc;
    char *argv[16];
    if (!buffer || buffer[0] == '\0') return FALSE;

    /* wait_prompt INPUT_MAX is 64, so argv_buffer is sufficient and avoids
     * a large temporary stack buffer.
     */
    sima_memclr(argv_buffer, (UINT16)sizeof(argv_buffer));
    (void)sima_strcpy(argv_buffer, (UINT16)sizeof(argv_buffer), buffer);
    argc = parse_argv(argv_buffer, argv, (UINT16)16);
    if (argc == 0 || !argv[0] || argv[0][0] == '\0') return FALSE;
	
    /* argv[0] 가 명령어 */
    if (sima_strcmp(argv[0], "ver") == STRC_SAME && argc==1) {
        run_ver();
        return TRUE;
    }
	else if(sima_strcmp(argv[0], "ver") == STRC_SAME) {
		return wrong_command_usage(buffer);
	}
    if (sima_strcmp(argv[0], "help") == STRC_SAME) {
        if (argc == 1) {
            run_help();
            return TRUE;
        }
        if (argc == 2) {
            sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
            sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), "Use: man ");
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), argv[1]);
            print_message(message_buffer);
            return TRUE;
        }
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "man") == STRC_SAME) {
        if (argc == 1) return run_man((const char*)0);
        if (argc == 2) {
            if (sima_strcmp(argv[1], "-h") == STRC_SAME || sima_strcmp(argv[1], "--help") == STRC_SAME) {
                return run_man((const char*)0);
            }
            if (sima_strcmp(argv[1], "-k") == STRC_SAME || sima_strcmp(argv[1], "-f") == STRC_SAME) {
                return wrong_command_usage(buffer);
            }
            return run_man(argv[1]);
        }
        if (argc == 3) {
            if (sima_strcmp(argv[1], "-k") == STRC_SAME) return run_man_apropos(argv[2]);
            if (sima_strcmp(argv[1], "-f") == STRC_SAME) return run_man_whatis(argv[2]);
            return run_man_with_section(argv[1], argv[2]);
        }
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "cls") == STRC_SAME && argc == 1) {
        return run_cls();
    }
    if (sima_strcmp(argv[0], "ls") == STRC_SAME) {
        if (argc == 1) return run_ls(NULL);
        if (argc == 2) return run_ls(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "cd") == STRC_SAME) {
        if (argc == 1) return run_cd(NULL);
        if (argc == 2) return run_cd(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "pwd") == STRC_SAME && argc == 1) {
        return run_pwd();
    }
    if (sima_strcmp(argv[0], "cat") == STRC_SAME && argc == 2) {
        return run_cat(argv[1]);
    }
    if (sima_strcmp(argv[0], "write") == STRC_SAME && argc >= 3) {
        BOOL ok = TRUE;
        UINT16 i;
        sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
        for (i = 2; i < argc; ++i) {
            if (i != 2) {
                ok = ok && sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " ");
            }
            ok = ok && sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), argv[i]);
        }
        if (!ok) {
            print_simple("Write data too long.");
            return TRUE;
        }
        return run_write(argv[1], message_buffer);
    }
    if (sima_strcmp(argv[0], "edit") == STRC_SAME) {
        if (argc == 1) return run_edit((const char*)0);
        if (argc == 2) return run_edit(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "rm") == STRC_SAME && argc == 2) {
        return run_rm(argv[1]);
    }
    if (sima_strcmp(argv[0], "rmdir") == STRC_SAME && argc == 2) {
        return run_rmdir(argv[1]);
    }
    if (sima_strcmp(argv[0], "mkdir") == STRC_SAME && argc == 2) {
        return run_mkdir(argv[1]);
    }
    if (sima_strcmp(argv[0], "load") == STRC_SAME && argc == 2) {
        return run_load(argv[1]);
    }
    if (sima_strcmp(argv[0], "run") == STRC_SAME && argc == 1) {
        return run_program();
    }
    if (sima_strcmp(argv[0], "exec") == STRC_SAME && argc == 2) {
        return run_exec(argv[1]);
    }
    if (sima_strcmp(argv[0], "sync") == STRC_SAME && argc == 1) {
        return run_sync();
    }
    if (sima_strcmp(argv[0], "diskinfo") == STRC_SAME && argc == 1) {
        return run_diskinfo();
    }
    if (sima_strcmp(argv[0], "calc") == STRC_SAME && argc == 1) {
        return run_calc();
    }
    if (sima_strcmp(argv[0], "hello") == STRC_SAME && argc == 1) {
        return run_hello();
    }
    if (sima_strcmp(argv[0], "whoami") == STRC_SAME && argc == 1) {
        return run_whoami();
    }
    if (sima_strcmp(argv[0], "users") == STRC_SAME && argc == 1) {
        return run_users();
    }
    if (sima_strcmp(argv[0], "useradd") == STRC_SAME) {
        if (argc == 2) return run_useradd(argv[1], argv[1]);
        if (argc == 3) return run_useradd(argv[1], argv[2]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "id") == STRC_SAME && argc == 1) {
        return run_id();
    }
    if (sima_strcmp(argv[0], "passwd") == STRC_SAME && argc == 1) {
        return run_passwd();
    }
    if (sima_strcmp(argv[0], "logout") == STRC_SAME && argc == 1) {
        return run_logout();
    }
    if (sima_strcmp(argv[0], "help") == STRC_SAME ||
        sima_strcmp(argv[0], "man") == STRC_SAME ||
        sima_strcmp(argv[0], "cls") == STRC_SAME ||
        sima_strcmp(argv[0], "ls") == STRC_SAME ||
        sima_strcmp(argv[0], "cd") == STRC_SAME ||
        sima_strcmp(argv[0], "pwd") == STRC_SAME ||
        sima_strcmp(argv[0], "cat") == STRC_SAME ||
        sima_strcmp(argv[0], "write") == STRC_SAME ||
        sima_strcmp(argv[0], "edit") == STRC_SAME ||
        sima_strcmp(argv[0], "rm") == STRC_SAME ||
        sima_strcmp(argv[0], "rmdir") == STRC_SAME ||
        sima_strcmp(argv[0], "mkdir") == STRC_SAME ||
        sima_strcmp(argv[0], "load") == STRC_SAME ||
        sima_strcmp(argv[0], "run") == STRC_SAME ||
        sima_strcmp(argv[0], "exec") == STRC_SAME ||
        sima_strcmp(argv[0], "sync") == STRC_SAME ||
        sima_strcmp(argv[0], "diskinfo") == STRC_SAME ||
        sima_strcmp(argv[0], "calc") == STRC_SAME ||
        sima_strcmp(argv[0], "hello") == STRC_SAME ||
        sima_strcmp(argv[0], "whoami") == STRC_SAME ||
        sima_strcmp(argv[0], "users") == STRC_SAME ||
        sima_strcmp(argv[0], "useradd") == STRC_SAME ||
        sima_strcmp(argv[0], "id") == STRC_SAME ||
        sima_strcmp(argv[0], "passwd") == STRC_SAME ||
        sima_strcmp(argv[0], "logout") == STRC_SAME) {
        return wrong_command_usage(buffer);
    }
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.1. All Rights Reserved.");
}
