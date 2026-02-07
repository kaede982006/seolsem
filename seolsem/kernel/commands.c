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

static char wrong_command_message[256];
static char message_buffer[256];
static void print_simple(const char *text);

#define EDIT_COLS 80
#define EDIT_ROWS 23
#define EDIT_HEADER_ROW 0
#define EDIT_CONTENT_ROW 1
#define EDIT_STATUS_ROW 24
#define EDIT_MAX_SIZE FS_BLOCK_SIZE

#define KEY_ESC 27
#define KEY_ENTER 13
#define KEY_BACKSPACE 8
#define KEY_CTRL_S 19
#define KEY_CTRL_Q 17

#define SCAN_LEFT 0x4B
#define SCAN_RIGHT 0x4D
#define SCAN_UP 0x48
#define SCAN_DOWN 0x50
#define SCAN_HOME 0x47
#define SCAN_END 0x4F
#define SCAN_DEL 0x53

static void editor_draw_row(UINT8 row, const char *text) {
    UINT8 col;
    for (col = 0; col < EDIT_COLS; ++col) {
        char ch = ' ';
        if (text && text[col] != '\0') ch = text[col];
        write_char(row, col, ch, 0x07);
    }
}

static void editor_draw_header(const char *name, BOOL modified) {
    char line[EDIT_COLS + 1];
    sima_memset(line, ' ', (UINT16)EDIT_COLS);
    line[EDIT_COLS] = '\0';
    sima_strcpy(line, (UINT16)sizeof(line), "EDIT - ");
    sima_strcat(line, (UINT16)sizeof(line), name);
    if (modified) {
        sima_strcat(line, (UINT16)sizeof(line), " *");
    }
    editor_draw_row(EDIT_HEADER_ROW, line);
}

static void editor_draw_status(const char *status) {
    char line[EDIT_COLS + 1];
    sima_memset(line, ' ', (UINT16)EDIT_COLS);
    line[EDIT_COLS] = '\0';
    if (status) {
        sima_strcpy(line, (UINT16)sizeof(line), status);
    }
    editor_draw_row(EDIT_STATUS_ROW, line);
}

static void editor_clear_content(void) {
    UINT8 row;
    for (row = 0; row < EDIT_ROWS; ++row) {
        editor_draw_row((UINT8)(EDIT_CONTENT_ROW + row), NULL);
    }
}

static void editor_index_to_pos(const char *buffer, UINT16 size, UINT16 index, UINT16 *out_row, UINT16 *out_col) {
    UINT16 row = 0;
    UINT16 col = 0;
    UINT16 i;
    if (index > size) index = size;
    for (i = 0; i < index; ++i) {
        char ch = buffer[i];
        if (ch == '\n') {
            row++;
            col = 0;
        } else {
            col++;
            if (col >= EDIT_COLS) {
                row++;
                col = 0;
            }
        }
    }
    *out_row = row;
    *out_col = col;
}

static UINT16 editor_index_for_row_col(const char *buffer, UINT16 size, UINT16 target_row, UINT16 target_col) {
    UINT16 row = 0;
    UINT16 col = 0;
    UINT16 i = 0;

    while (i < size) {
        if (row == target_row && col >= target_col) return i;
        if (buffer[i] == '\n') {
            if (row == target_row) return i;
            row++;
            col = 0;
            i++;
            continue;
        }
        col++;
        i++;
        if (col >= EDIT_COLS) {
            if (row == target_row) return i;
            row++;
            col = 0;
        }
    }
    return size;
}

static void editor_render(const char *name, const char *buffer, UINT16 size, UINT16 cursor,
                          UINT16 *scroll_row, BOOL modified, const char *status) {
    UINT16 row;
    UINT16 col;
    UINT16 doc_row;
    UINT16 doc_col;
    UINT16 i;
    UINT16 render_cursor = cursor;

    editor_index_to_pos(buffer, size, render_cursor, &doc_row, &doc_col);
    if (doc_row < *scroll_row) {
        *scroll_row = doc_row;
    } else if (doc_row >= (UINT16)(*scroll_row + EDIT_ROWS)) {
        *scroll_row = (UINT16)(doc_row - EDIT_ROWS + 1);
    }

    editor_draw_header(name, modified);
    editor_draw_status(status);
    editor_clear_content();

    row = 0;
    col = 0;
    for (i = 0; i < size; ++i) {
        char ch = buffer[i];
        if (ch == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (row >= *scroll_row && row < (UINT16)(*scroll_row + EDIT_ROWS)) {
            UINT8 screen_row = (UINT8)(EDIT_CONTENT_ROW + (row - *scroll_row));
            write_char(screen_row, (UINT8)col, ch, 0x07);
        }
        col++;
        if (col >= EDIT_COLS) {
            row++;
            col = 0;
        }
    }

    editor_index_to_pos(buffer, size, render_cursor, &doc_row, &doc_col);
    if (doc_row >= *scroll_row && doc_row < (UINT16)(*scroll_row + EDIT_ROWS)) {
        UINT8 screen_row = (UINT8)(EDIT_CONTENT_ROW + (doc_row - *scroll_row));
        set_cursor(screen_row, (UINT8)doc_col);
    } else {
        set_cursor(EDIT_CONTENT_ROW, 0);
    }
}

static BOOL editor_insert_char(char *buffer, UINT16 *size, UINT16 *cursor, char ch) {
    if (*size >= EDIT_MAX_SIZE) return FALSE;
    sima_memmove(buffer + *cursor + 1, buffer + *cursor, (UINT16)(*size - *cursor));
    buffer[*cursor] = ch;
    (*size)++;
    (*cursor)++;
    buffer[*size] = '\0';
    return TRUE;
}

static BOOL editor_delete_before(char *buffer, UINT16 *size, UINT16 *cursor) {
    if (*cursor == 0 || *size == 0) return FALSE;
    sima_memmove(buffer + *cursor - 1, buffer + *cursor, (UINT16)(*size - *cursor));
    (*cursor)--;
    (*size)--;
    buffer[*size] = '\0';
    return TRUE;
}

static BOOL editor_delete_at(char *buffer, UINT16 *size, UINT16 *cursor) {
    if (*cursor >= *size) return FALSE;
    sima_memmove(buffer + *cursor, buffer + *cursor + 1, (UINT16)(*size - *cursor - 1));
    (*size)--;
    buffer[*size] = '\0';
    return TRUE;
}

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

static BOOL load_program_with_path(const char *name) {
    const char *path;
    char path_buffer[64];
    char *dirs[8];
    UINT16 count;
    UINT16 i;

    if (program_load(name)) return TRUE;
    path = env_get("PATH");
    if (!path) return FALSE;
    if (!sima_strcpy(path_buffer, (UINT16)sizeof(path_buffer), path)) {
        return FALSE;
    }
    count = sima_strcspl(path_buffer, ';', dirs, (UINT16)8);
    for (i = 0; i < count; ++i) {
        if (program_load_in_dir(dirs[i], name)) return TRUE;
    }
    return FALSE;
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

static BOOL run_man(const char *topic) {
    static const char *const man_index[] = {
        "Usage:",
        "  man <topic>",
        "  help <topic>",
        "",
        "Main topics:",
        "  system  fs  exec  user  display",
        "",
        "Command topics:",
        "  ls cd pwd cat write edit rm rmdir mkdir",
        "  load run exec sync diskinfo",
        "  whoami id users useradd login su passwd",
        "",
        "Examples:",
        "  man fs",
        "  man exec",
        "  man cd"
    };
    static const char *const man_system[] = {
        "system:",
        "  ver             show OS version",
        "  help [topic]    show manual index/page",
        "  man [topic]     show manual page",
        "  diskinfo        show disk and volume info",
        "",
        "Notes:",
        "  - command args support quotes",
        "  - path and permission checks are Linux-like"
    };
    static const char *const man_fs[] = {
        "fs:",
        "  ls [path]       list directory entries",
        "  cd [path]       change directory",
        "  cd -            jump to previous directory",
        "  pwd             print working directory",
        "  cat <file>      print file content",
        "  write <f> <txt> create/overwrite file",
        "  edit <file>     open text editor",
        "  rm <file>       delete file",
        "  rmdir <dir>     remove empty directory",
        "  mkdir <dir>     create directory",
        "  sync            flush cached FAT sectors",
        "",
        "Path tips:",
        "  - absolute: /BIN/HELLO.PRG",
        "  - relative: ./memo.txt  ../",
        "  - home: ~/notes.txt",
        "  - non-root users are limited by policy"
    };
    static const char *const man_exec[] = {
        "exec:",
        "  load <file>     load .PRG script into memory",
        "  run             run currently loaded program",
        "  exec <file>     load and run immediately",
        "",
        "Program format:",
        "  PRINT <text>",
        "  CLS",
        "  EXIT",
        "",
        "Search path:",
        "  - current directory first",
        "  - then PATH entries (default BIN)"
    };
    static const char *const man_user[] = {
        "user:",
        "  whoami          print current user",
        "  id              print current uid",
        "  users           list users",
        "  useradd <n> [p] add user (root only)",
        "  login <n> [p]   switch user",
        "  su <n> [p]      alias of login",
        "  passwd          change your password",
        "",
        "Default rules:",
        "  - root uid is 0",
        "  - user homes are under /HOME/<NAME>",
        "  - /ETC/PASSWD is source of truth"
    };
    static const char *const man_display[] = {
        "display:",
        "  cls             clear text screen",
        "",
        "Editor keys:",
        "  Ctrl+S          save",
        "  Esc             save and exit",
        "  Ctrl+Q          quit without saving",
        "  Arrow/Home/End/Delete supported"
    };
    static const char *const man_cd[] = {
        "cd:",
        "  cd              move to HOME",
        "  cd /            move to root",
        "  cd -            move to previous directory",
        "  cd <path>       move to path",
        "",
        "Examples:",
        "  cd /BIN",
        "  cd ..",
        "  cd ~/DOCS"
    };

    if (!topic || topic[0] == '\0') {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }

    if (man_topic_eq(topic, "HELP") || man_topic_eq(topic, "MAN")) {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }
    if (man_topic_eq(topic, "SYSTEM") || man_topic_eq(topic, "VER") || man_topic_eq(topic, "DISKINFO")) {
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
        man_topic_eq(topic, "LOGIN") || man_topic_eq(topic, "SU") ||
        man_topic_eq(topic, "PASSWD")) {
        return man_show_page("User", man_user, (UINT16)(sizeof(man_user) / sizeof(man_user[0])));
    }
    if (man_topic_eq(topic, "DISPLAY") || man_topic_eq(topic, "CLS")) {
        return man_show_page("Display", man_display, (UINT16)(sizeof(man_display) / sizeof(man_display[0])));
    }

    print_simple("No manual entry. Try: man");
    return TRUE;
}

void run_help() {
    (void)run_man((const char*)0);
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
    print_message("");
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

static BOOL run_edit(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;

    {
        char buffer[EDIT_MAX_SIZE + 1];
        UINT16 size = 0;
        UINT16 cursor = 0;
        UINT16 scroll_row = 0;
        UINT16 desired_col = 0;
        BOOL modified = FALSE;
        char status[EDIT_COLS + 1];
        UINT32 read_size = 0;

        sima_memclr(buffer, (UINT16)sizeof(buffer));
        if (fs_read(name, (UINT8*)buffer, EDIT_MAX_SIZE, &read_size)) {
            size = (UINT16)read_size;
            buffer[size] = '\0';
        }

        clear_screen();
        sima_strcpy(status, (UINT16)sizeof(status),
                    "Ctrl+S Save  Esc Save&Exit  Ctrl+Q Quit  Arrows Move");

        for (;;) {
            UINT16 key;
            UINT8 ascii;
            UINT8 scan;

            editor_render(name, buffer, size, cursor, &scroll_row, modified, status);
            key = read_key();
            ascii = (UINT8)(key & 0xFF);
            scan = (UINT8)((key >> 8) & 0xFF);

            if (ascii == KEY_ESC) {
                if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                    print_simple("Unable to save memo.");
                    return TRUE;
                }
                clear_screen();
                print_simple("Memo saved.");
                break;
            }

            if (ascii == KEY_CTRL_Q) {
                clear_screen();
                print_simple("Edit cancelled.");
                break;
            }

            if (ascii == KEY_CTRL_S) {
                if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                    sima_strcpy(status, (UINT16)sizeof(status), "Save failed.");
                } else {
                    sima_strcpy(status, (UINT16)sizeof(status), "Saved.");
                    modified = FALSE;
                }
                continue;
            }

            if (ascii == KEY_BACKSPACE) {
                if (editor_delete_before(buffer, &size, &cursor)) {
                    modified = TRUE;
                }
                continue;
            }

            if (ascii == KEY_ENTER) {
                if (editor_insert_char(buffer, &size, &cursor, '\n')) {
                    modified = TRUE;
                }
                continue;
            }

            if (ascii == 0 || ascii == 0xE0) {
                UINT16 row;
                UINT16 col;
                editor_index_to_pos(buffer, size, cursor, &row, &col);
                desired_col = col;

                if (scan == SCAN_LEFT) {
                    if (cursor > 0) cursor--;
                    continue;
                }
                if (scan == SCAN_RIGHT) {
                    if (cursor < size) cursor++;
                    continue;
                }
                if (scan == SCAN_UP) {
                    if (row > 0) {
                        cursor = editor_index_for_row_col(buffer, size, (UINT16)(row - 1), desired_col);
                    }
                    continue;
                }
                if (scan == SCAN_DOWN) {
                    cursor = editor_index_for_row_col(buffer, size, (UINT16)(row + 1), desired_col);
                    continue;
                }
                if (scan == SCAN_HOME) {
                    cursor = editor_index_for_row_col(buffer, size, row, 0);
                    continue;
                }
                if (scan == SCAN_END) {
                    cursor = editor_index_for_row_col(buffer, size, row, (UINT16)(EDIT_COLS - 1));
                    continue;
                }
                if (scan == SCAN_DEL) {
                    if (editor_delete_at(buffer, &size, &cursor)) {
                        modified = TRUE;
                    }
                    continue;
                }
            }

            if (ascii >= 32 && ascii != 127) {
                if (editor_insert_char(buffer, &size, &cursor, (char)ascii)) {
                    modified = TRUE;
                }
                continue;
            }
        }
    }
    return TRUE;
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

static BOOL run_load(const char *name) {
    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;
    if (!perm_check(name, PERM_OP_READ | PERM_OP_EXEC)) {
        print_simple("Permission denied.");
        return TRUE;
    }
    if (!load_program_with_path(name)) {
        print_simple("Unable to load program.");
        return TRUE;
    }
    print_simple("Program loaded.");
    return TRUE;
}

static BOOL run_program(void) {
    if (!program_run()) {
        print_simple("No program loaded.");
        return TRUE;
    }
    return TRUE;
}

static BOOL run_exec(const char *name) {
    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;
    if (!perm_check(name, PERM_OP_READ | PERM_OP_EXEC)) {
        print_simple("Permission denied.");
        return TRUE;
    }
    if (!load_program_with_path(name)) {
        print_simple("Unable to load program.");
        return TRUE;
    }
    if (!program_run()) {
        print_simple("Program failed to run.");
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

static BOOL run_login(const char *name, const char *password) {
    char pass_buf[USER_PASS_MAX];
    const char *pass = password;
    if (!name || name[0] == '\0') return FALSE;
    sima_memclr(pass_buf, (UINT16)sizeof(pass_buf));
    if (!pass || pass[0] == '\0') {
        wait_prompt("password: ", pass_buf);
        pass = pass_buf;
    }
    if (!user_login(name, pass)) {
        print_simple("Unable to switch user.");
        return TRUE;
    }
    sima_memclr(pass_buf, (UINT16)sizeof(pass_buf));
    print_simple("User switched.");
    return TRUE;
}

static BOOL run_passwd(void) {
    char old_buf[USER_PASS_MAX];
    char new_buf[USER_PASS_MAX];

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));

    wait_prompt("current password: ", old_buf);
    wait_prompt("new password: ", new_buf);

    if (new_buf[0] == '\0') {
        print_simple("New password cannot be empty.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        return TRUE;
    }

    if (!user_change_password(old_buf, new_buf)) {
        print_simple("Unable to update password.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        return TRUE;
    }

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));
    print_simple("Password updated.");
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
	char temp[256];
    if (!buffer || buffer[0] == '\0') return FALSE;

	sima_strcpy(temp,sizeof(temp),buffer);
    argc = parse_argv(temp, argv, (UINT16)16);
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
            return run_man(argv[1]);
        }
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "man") == STRC_SAME) {
        if (argc == 1) return run_man((const char*)0);
        if (argc == 2) return run_man(argv[1]);
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
        char data_buf[256];
        UINT16 i;
        sima_memclr(data_buf, (UINT16)sizeof(data_buf));
        for (i = 2; i < argc; ++i) {
            if (i != 2) {
                sima_strcat(data_buf, (UINT16)sizeof(data_buf), " ");
            }
            sima_strcat(data_buf, (UINT16)sizeof(data_buf), argv[i]);
        }
        return run_write(argv[1], data_buf);
    }
    if (sima_strcmp(argv[0], "edit") == STRC_SAME && argc == 2) {
        return run_edit(argv[1]);
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
    if ((sima_strcmp(argv[0], "login") == STRC_SAME ||
         sima_strcmp(argv[0], "su") == STRC_SAME)) {
        if (argc == 2) return run_login(argv[1], NULL);
        if (argc == 3) return run_login(argv[1], argv[2]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "id") == STRC_SAME && argc == 1) {
        return run_id();
    }
    if (sima_strcmp(argv[0], "passwd") == STRC_SAME && argc == 1) {
        return run_passwd();
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
        sima_strcmp(argv[0], "whoami") == STRC_SAME ||
        sima_strcmp(argv[0], "users") == STRC_SAME ||
        sima_strcmp(argv[0], "useradd") == STRC_SAME ||
        sima_strcmp(argv[0], "login") == STRC_SAME ||
        sima_strcmp(argv[0], "su") == STRC_SAME ||
        sima_strcmp(argv[0], "id") == STRC_SAME ||
        sima_strcmp(argv[0], "passwd") == STRC_SAME) {
        return wrong_command_usage(buffer);
    }
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.0. All Rights Reserved.");
}
