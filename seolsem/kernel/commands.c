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

/* Editor constants (used by Edlin-style line editor) */
#define EDIT_COLS 80
#define EDIT_MAX_SIZE FS_BLOCK_SIZE

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
        "  system  fs  exec  user  display  debug",
        "",
        "Command topics:",
        "  ver help man cls",
        "  ls cd pwd cat write edit rm rmdir mkdir",
        "  load run exec sync diskinfo fault",
        "  whoami id users useradd passwd logout",
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
        "  fault <kind>    trigger CPU exception (debug)",
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
    static const char *const man_debug[] = {
        "fault:",
        "  fault <kind>    intentionally trigger an exception",
        "",
        "Kinds:",
        "  de|div0         divide error (#DE)",
        "  bp|int3         breakpoint (#BP)",
        "  ud|invalid      invalid opcode (#UD)",
        "  gp              general protection (#GP)",
        "  of|overflow     overflow (#OF via INTO)",
        "",
        "Use this command to verify exception screen output."
    };
    static const char *const man_user[] = {
        "user:",
        "  whoami          print current user",
        "  id              print current uid",
        "  users           list users",
        "  useradd <n> [p] add user (root only)",
        "  passwd          change your password",
        "  logout          return to login screen",
        "",
        "Default rules:",
        "  - root uid is 0",
        "  - user homes are under /HOME/<NAME>",
        "  - /ETC/PASSWD is source of truth",
        "  - login is only available from login screen"
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
    if (man_topic_eq(topic, "DEBUG") || man_topic_eq(topic, "FAULT")) {
        return man_show_page("Debug", man_debug, (UINT16)(sizeof(man_debug) / sizeof(man_debug[0])));
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

/* ============================================================
 * Vi-like Visual Editor for Seolsem
 * ============================================================ */

/* Editor constants */
#define VI_ROWS 23       /* Visible text rows (screen 25 - 2 for status) */
#define VI_STATUS_ROW 24 /* Status bar row (0-indexed) */

/* Vi modes */
#define VI_MODE_NORMAL   0
#define VI_MODE_INSERT   1
#define VI_MODE_COMMAND  2

/* Key codes */
#define KEY_ESC    0x1B
#define KEY_ENTER  0x0D
#define KEY_BACKSP 0x08
#define KEY_UP     0x4800
#define KEY_DOWN   0x5000
#define KEY_LEFT   0x4B00
#define KEY_RIGHT  0x4D00
#define KEY_HOME   0x4700
#define KEY_END    0x4F00
#define KEY_PGUP   0x4900
#define KEY_PGDN   0x5100
#define KEY_DEL    0x5300

/* Vi state */
static UINT8 vi_mode = VI_MODE_NORMAL;
static char vi_buffer[EDIT_MAX_SIZE + 1];
static UINT16 vi_size = 0;
static UINT16 vi_cursor = 0;        /* Byte position in buffer */
static UINT16 vi_top_line = 0;      /* First visible line */
static UINT16 vi_cur_row = 0;       /* Cursor row (screen, 0-based) */
static UINT16 vi_cur_col = 0;       /* Cursor column (screen, 0-based) */
static char vi_cmd_buf[64];         /* Command line buffer */
static UINT16 vi_cmd_len = 0;
static BOOL vi_modified = FALSE;
static char vi_status_msg[64];      /* Status message */
static char vi_yank_buf[EDIT_COLS + 2]; /* Yanked line buffer */
static UINT16 vi_yank_len = 0;

/* Forward declarations */
static void vi_render_screen(void);
static void vi_render_status(const char *filename);
static void vi_cursor_sync(void);
static UINT16 vi_get_line_start(UINT16 pos);
static UINT16 vi_get_line_end(UINT16 pos);
static UINT16 vi_count_lines(void);
static UINT16 vi_line_to_pos(UINT16 line);
static UINT16 vi_pos_to_line(UINT16 pos);
static UINT16 vi_get_visual_col(UINT16 pos);
static UINT16 vi_pos_at_visual_col(UINT16 line_start, UINT16 target_col);
static BOOL vi_is_word_char(char ch);
static void vi_move_line_first_nonblank(void);
static void vi_move_word_forward(void);
static void vi_move_word_backward(void);
static BOOL vi_delete_to_eol(void);
static BOOL vi_change_line(void);
static BOOL vi_join_with_next_line(void);

/* Get start of line containing pos */
static UINT16 vi_get_line_start(UINT16 pos) {
    while (pos > 0 && vi_buffer[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

/* Get end of line (newline or buffer end) */
static UINT16 vi_get_line_end(UINT16 pos) {
    while (pos < vi_size && vi_buffer[pos] != '\n') {
        pos++;
    }
    return pos;
}

/* Count total lines */
static UINT16 vi_count_lines(void) {
    UINT16 lines = 1;
    UINT16 i;
    if (vi_size == 0) return 1;
    for (i = 0; i < vi_size; i++) {
        if (vi_buffer[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

/* Get buffer position for line number (0-based) */
static UINT16 vi_line_to_pos(UINT16 line) {
    UINT16 pos = 0;
    UINT16 cur = 0;
    while (pos < vi_size && cur < line) {
        if (vi_buffer[pos] == '\n') cur++;
        pos++;
    }
    return pos;
}

/* Get line number for buffer position (0-based) */
static UINT16 vi_pos_to_line(UINT16 pos) {
    UINT16 line = 0;
    UINT16 i;
    if (pos > vi_size) pos = vi_size;
    for (i = 0; i < pos && i < vi_size; i++) {
        if (vi_buffer[i] == '\n') line++;
    }
    return line;
}

/* Visual column with tab expansion for a buffer position */
static UINT16 vi_get_visual_col(UINT16 pos) {
    UINT16 line_start = vi_get_line_start(pos);
    UINT16 col = 0;
    UINT16 i = line_start;

    while (i < pos && i < vi_size && vi_buffer[i] != '\n') {
        if (vi_buffer[i] == '\t') {
            col += (8 - (col % 8));
        } else {
            col++;
        }
        i++;
    }
    return col;
}

/* Find buffer position in line that matches target visual column */
static UINT16 vi_pos_at_visual_col(UINT16 line_start, UINT16 target_col) {
    UINT16 pos = line_start;
    UINT16 col = 0;
    UINT16 line_end = vi_get_line_end(line_start);

    while (pos < line_end) {
        UINT16 next_col = col + (vi_buffer[pos] == '\t' ? (8 - (col % 8)) : 1);
        if (next_col > target_col) break;
        col = next_col;
        pos++;
    }
    return pos;
}

static BOOL vi_is_word_char(char ch) {
    return (BOOL)(
        (ch >= '0' && ch <= '9') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        ch == '_'
    );
}

/* Move to first non-blank character of current line */
static void vi_move_line_first_nonblank(void) {
    UINT16 pos = vi_get_line_start(vi_cursor);
    UINT16 end = vi_get_line_end(pos);
    while (pos < end && (vi_buffer[pos] == ' ' || vi_buffer[pos] == '\t')) {
        pos++;
    }
    vi_cursor = pos;
}

/* Sync cursor position (row/col) from buffer position */
static void vi_cursor_sync(void) {
    UINT16 cur_line = vi_pos_to_line(vi_cursor);

    vi_cur_col = vi_get_visual_col(vi_cursor);
    if (vi_cur_col >= EDIT_COLS) {
        vi_cur_col = EDIT_COLS - 1;
    }

    /* Scroll if needed */
    if (cur_line < vi_top_line) {
        vi_top_line = cur_line;
    } else if (cur_line >= vi_top_line + VI_ROWS) {
        vi_top_line = cur_line - VI_ROWS + 1;
    }

    vi_cur_row = cur_line - vi_top_line;
}

/* Render visible portion of buffer */
static void vi_render_screen(void) {
    UINT16 screen_row;
    UINT16 pos;
    UINT16 line;
    
    clear_screen();
    
    pos = vi_line_to_pos(vi_top_line);
    
    for (screen_row = 0; screen_row < VI_ROWS; screen_row++) {
        UINT16 col = 0;
        line = vi_top_line + screen_row;
        
        if (line >= vi_count_lines()) {
            /* Draw ~ for empty lines after EOF */
            write_char(screen_row, 0, '~', 0x07);
        } else {
            /* Render line content */
            while (pos < vi_size && vi_buffer[pos] != '\n' && col < EDIT_COLS - 1) {
                char ch = vi_buffer[pos];
                if (ch == '\t') {
                    /* Tab expansion */
                    UINT16 spaces = 8 - (col % 8);
                    while (spaces-- > 0 && col < EDIT_COLS - 1) {
                        write_char(screen_row, col++, ' ', 0x07);
                    }
                } else if (ch >= 32) {
                    write_char(screen_row, col++, ch, 0x07);
                }
                pos++;
            }
            /* Skip past newline */
            if (pos < vi_size && vi_buffer[pos] == '\n') pos++;
        }
    }
}

/* Render status bar */
static void vi_render_status(const char *filename) {
    char status[EDIT_COLS + 1];
    char line_info[48];
    UINT16 i;
    UINT16 cur_line = vi_pos_to_line(vi_cursor) + 1;
    UINT16 total_lines = vi_count_lines();
    UINT16 cur_col = vi_get_visual_col(vi_cursor) + 1;
    const char *mode_str;

    (void)filename;
    sima_memclr(status, sizeof(status));
    sima_memclr(line_info, sizeof(line_info));

    /* Mode indicator */
    switch (vi_mode) {
        case VI_MODE_INSERT:  mode_str = "-- INSERT --"; break;
        case VI_MODE_COMMAND: mode_str = ":"; break;
        default:              mode_str = ""; break;
    }

    if (vi_mode == VI_MODE_COMMAND) {
        /* Show command line */
        status[0] = ':';
        sima_memmove(status + 1, vi_cmd_buf, vi_cmd_len);
        status[vi_cmd_len + 1] = '\0';
    } else if (vi_status_msg[0] != '\0') {
        sima_strcpy(status, sizeof(status), vi_status_msg);
        vi_status_msg[0] = '\0'; /* Clear after display */
    } else {
        sima_strcpy(status, sizeof(status), mode_str);
    }

    /* Line/column info on right: "Ln x/y Col z" */
    sima_strcpy(line_info, sizeof(line_info), "Ln ");
    {
        char tmp[16];
        sima_utoa(cur_line, tmp, sizeof(tmp), 10);
        sima_strcat(line_info, sizeof(line_info), tmp);
    }
    sima_strcat(line_info, sizeof(line_info), "/");
    {
        char tmp[16];
        sima_utoa(total_lines, tmp, sizeof(tmp), 10);
        sima_strcat(line_info, sizeof(line_info), tmp);
    }
    sima_strcat(line_info, sizeof(line_info), " Col ");
    {
        char tmp[16];
        sima_utoa(cur_col, tmp, sizeof(tmp), 10);
        sima_strcat(line_info, sizeof(line_info), tmp);
    }

    /* Draw status bar with inverse video */
    for (i = 0; i < EDIT_COLS; i++) {
        char ch = (i < sima_strlen(status)) ? status[i] : ' ';
        write_char(VI_STATUS_ROW, i, ch, 0x70); /* Inverse */
    }

    /* Overlay line info on right */
    {
        UINT16 info_len = sima_strlen(line_info);
        UINT16 start_col = 0;
        if (info_len < EDIT_COLS) {
            start_col = EDIT_COLS - info_len - 1;
        }
        for (i = 0; i < info_len; i++) {
            write_char(VI_STATUS_ROW, start_col + i, line_info[i], 0x70);
        }
    }

    /* Show modified indicator */
    if (vi_modified) {
        write_char(VI_STATUS_ROW, EDIT_COLS - 1, '+', 0x70);
    }
}

/* Insert character at cursor */
static BOOL vi_insert_char(char ch) {
    if (vi_size >= EDIT_MAX_SIZE) return FALSE;
    
    sima_memmove(vi_buffer + vi_cursor + 1, vi_buffer + vi_cursor, vi_size - vi_cursor);
    vi_buffer[vi_cursor] = ch;
    vi_size++;
    vi_cursor++;
    vi_modified = TRUE;
    return TRUE;
}

/* Delete character at cursor */
static BOOL vi_delete_char(void) {
    if (vi_cursor >= vi_size) return FALSE;

    sima_memmove(vi_buffer + vi_cursor, vi_buffer + vi_cursor + 1, vi_size - vi_cursor - 1);
    vi_size--;
    vi_modified = TRUE;
    return TRUE;
}

/* Delete from cursor to end-of-line (excluding newline) */
static BOOL vi_delete_to_eol(void) {
    UINT16 end = vi_get_line_end(vi_cursor);
    UINT16 del_len;

    if (vi_cursor >= end) return FALSE;

    del_len = end - vi_cursor;
    sima_memmove(vi_buffer + vi_cursor, vi_buffer + end, vi_size - end);
    vi_size -= del_len;
    vi_modified = TRUE;
    return TRUE;
}

/* Delete entire line */
static BOOL vi_delete_line(void) {
    UINT16 start = vi_get_line_start(vi_cursor);
    UINT16 end = vi_get_line_end(vi_cursor);
    UINT16 del_len;
    
    /* Include newline if present */
    if (end < vi_size && vi_buffer[end] == '\n') end++;
    
    del_len = end - start;
    if (del_len == 0) return FALSE;
    
    /* Yank before delete */
    if (del_len < sizeof(vi_yank_buf)) {
        sima_memmove(vi_yank_buf, vi_buffer + start, del_len);
        vi_yank_len = del_len;
    }
    
    sima_memmove(vi_buffer + start, vi_buffer + end, vi_size - end);
    vi_size -= del_len;
    vi_cursor = start;
    
    /* Ensure cursor is valid */
    if (vi_cursor >= vi_size && vi_size > 0) {
        vi_cursor = vi_size - 1;
    }
    
    vi_modified = TRUE;
    return TRUE;
}

/* Change whole line: clear content and keep cursor at line start */
static BOOL vi_change_line(void) {
    UINT16 start = vi_get_line_start(vi_cursor);
    UINT16 end = vi_get_line_end(vi_cursor);
    UINT16 del_len;

    vi_cursor = start;
    if (start >= end) return TRUE; /* already empty line */

    del_len = end - start;
    sima_memmove(vi_buffer + start, vi_buffer + end, vi_size - end);
    vi_size -= del_len;
    vi_modified = TRUE;
    return TRUE;
}

/* Join current line with next line */
static BOOL vi_join_with_next_line(void) {
    UINT16 end = vi_get_line_end(vi_cursor);
    UINT16 next;
    UINT16 remove_len;

    if (end >= vi_size || vi_buffer[end] != '\n') return FALSE;

    next = end + 1;
    while (next < vi_size && (vi_buffer[next] == ' ' || vi_buffer[next] == '\t')) {
        next++;
    }

    if (next >= vi_size) {
        sima_memmove(vi_buffer + end, vi_buffer + end + 1, vi_size - (end + 1));
        vi_size--;
        vi_modified = TRUE;
        return TRUE;
    }

    vi_buffer[end] = ' ';
    remove_len = next - (end + 1);
    if (remove_len > 0) {
        sima_memmove(vi_buffer + end + 1, vi_buffer + next, vi_size - next);
        vi_size -= remove_len;
    }
    vi_modified = TRUE;
    return TRUE;
}

/* Paste yanked content */
static BOOL vi_paste(void) {
    UINT16 end;

    if (vi_yank_len == 0) return FALSE;
    if (vi_size + vi_yank_len > EDIT_MAX_SIZE) return FALSE;

    /* Move to next line for line paste */
    end = vi_get_line_end(vi_cursor);
    if (end < vi_size && vi_buffer[end] == '\n') end++;
    vi_cursor = end;

    sima_memmove(vi_buffer + vi_cursor + vi_yank_len, vi_buffer + vi_cursor, vi_size - vi_cursor);
    sima_memmove(vi_buffer + vi_cursor, vi_yank_buf, vi_yank_len);
    vi_size += vi_yank_len;
    vi_modified = TRUE;
    return TRUE;
}

/* Move cursor left */
static void vi_move_left(void) {
    if (vi_cursor > 0) {
        UINT16 line_start = vi_get_line_start(vi_cursor);
        if (vi_cursor > line_start) {
            vi_cursor--;
        }
    }
}

/* Move cursor right */
static void vi_move_right(void) {
    if (vi_cursor < vi_size) {
        UINT16 line_end = vi_get_line_end(vi_cursor);
        if (vi_cursor < line_end) {
            vi_cursor++;
        }
    }
}

/* Move cursor up */
static void vi_move_up(void) {
    UINT16 cur_line = vi_pos_to_line(vi_cursor);
    if (cur_line > 0) {
        UINT16 target_col = vi_get_visual_col(vi_cursor);
        UINT16 new_pos = vi_line_to_pos(cur_line - 1);
        UINT16 new_end = vi_get_line_end(new_pos);

        vi_cursor = vi_pos_at_visual_col(new_pos, target_col);
        if (vi_mode == VI_MODE_NORMAL && new_pos < new_end && vi_cursor == new_end) {
            vi_cursor = new_end - 1;
        }
    }
}

/* Move cursor down */
static void vi_move_down(void) {
    UINT16 cur_line = vi_pos_to_line(vi_cursor);
    UINT16 total = vi_count_lines();
    if (cur_line + 1 < total) {
        UINT16 target_col = vi_get_visual_col(vi_cursor);
        UINT16 new_pos = vi_line_to_pos(cur_line + 1);
        UINT16 new_end = vi_get_line_end(new_pos);

        vi_cursor = vi_pos_at_visual_col(new_pos, target_col);
        if (vi_mode == VI_MODE_NORMAL && new_pos < new_end && vi_cursor == new_end) {
            vi_cursor = new_end - 1;
        }
    }
}

/* Move to beginning of next word */
static void vi_move_word_forward(void) {
    UINT16 pos;

    if (vi_size == 0) return;
    pos = vi_cursor;
    if (pos < vi_size) pos++;

    while (pos < vi_size && !vi_is_word_char(vi_buffer[pos])) {
        pos++;
    }

    if (pos < vi_size) {
        vi_cursor = pos;
    } else {
        vi_cursor = vi_size - 1;
    }
}

/* Move to beginning of previous word */
static void vi_move_word_backward(void) {
    UINT16 pos;

    if (vi_size == 0 || vi_cursor == 0) return;
    pos = vi_cursor - 1;

    while (pos > 0 && !vi_is_word_char(vi_buffer[pos])) {
        pos--;
    }
    while (pos > 0 && vi_is_word_char(vi_buffer[pos - 1])) {
        pos--;
    }
    vi_cursor = pos;
}

/* Execute command */
static BOOL vi_exec_cmd(const char *filename) {
    /* Parse command */
    if (vi_cmd_len == 0) return TRUE;
    
    if (vi_cmd_buf[0] == 'w' && (vi_cmd_len == 1 || (vi_cmd_len == 2 && vi_cmd_buf[1] == 'q'))) {
        /* :w or :wq */
        if (!fs_write(filename, (const UINT8*)vi_buffer, (UINT32)vi_size)) {
            sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "Write failed!");
            return TRUE;
        }
        vi_modified = FALSE;
        sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "Written");
        if (vi_cmd_len == 2) return FALSE; /* :wq - quit */
    } else if (vi_cmd_buf[0] == 'q') {
        if (vi_cmd_len == 1) {
            if (vi_modified) {
                sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "No write since last change (add ! to override)");
                return TRUE;
            }
            return FALSE; /* Quit */
        } else if (vi_cmd_len == 2 && vi_cmd_buf[1] == '!') {
            return FALSE; /* :q! - force quit */
        }
    } else if (vi_cmd_buf[0] == 'x') {
        /* :x - save if modified, then quit */
        if (vi_modified) {
            if (!fs_write(filename, (const UINT8*)vi_buffer, (UINT32)vi_size)) {
                sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "Write failed!");
                return TRUE;
            }
        }
        return FALSE;
    } else {
        sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "Unknown command");
    }
    
    return TRUE;
}

/* Main editor function */
static BOOL run_edit(const char *name) {
    UINT32 read_size = 0;
    BOOL running = TRUE;
    
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    
    /* Initialize state */
    vi_mode = VI_MODE_NORMAL;
    vi_size = 0;
    vi_cursor = 0;
    vi_top_line = 0;
    vi_cur_row = 0;
    vi_cur_col = 0;
    vi_cmd_len = 0;
    vi_modified = FALSE;
    vi_yank_len = 0;
    vi_status_msg[0] = '\0';
    sima_memclr(vi_buffer, sizeof(vi_buffer));
    sima_memclr(vi_cmd_buf, sizeof(vi_cmd_buf));
    sima_memclr(vi_yank_buf, sizeof(vi_yank_buf));
    
    /* Load file */
    if (fs_read(name, (UINT8*)vi_buffer, EDIT_MAX_SIZE, &read_size)) {
        vi_size = (UINT16)read_size;
        sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "File loaded");
    } else {
        sima_strcpy(vi_status_msg, sizeof(vi_status_msg), "New file");
    }
    vi_buffer[vi_size] = '\0';
    
    /* Main loop */
    while (running) {
        UINT16 key;
        char ch;

        if (vi_cursor > vi_size) {
            vi_cursor = vi_size;
        }
        if (vi_mode == VI_MODE_NORMAL && vi_size > 0 && vi_cursor == vi_size && vi_buffer[vi_size - 1] != '\n') {
            vi_cursor = vi_size - 1;
        }

        vi_cursor_sync();
        vi_render_screen();
        vi_render_status(name);
        set_cursor(vi_cur_row, vi_cur_col);

        key = read_key();
        ch = (char)(key & 0xFF);

        switch (vi_mode) {
            case VI_MODE_NORMAL:
                if (ch == 'h' || key == KEY_LEFT) {
                    vi_move_left();
                } else if (ch == 'j' || key == KEY_DOWN) {
                    vi_move_down();
                } else if (ch == 'k' || key == KEY_UP) {
                    vi_move_up();
                } else if (ch == 'l' || key == KEY_RIGHT) {
                    vi_move_right();
                } else if (ch == 'w') {
                    vi_move_word_forward();
                } else if (ch == 'b') {
                    vi_move_word_backward();
                } else if (ch == 'i') {
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'I') {
                    vi_move_line_first_nonblank();
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'a') {
                    if (vi_cursor < vi_size && vi_buffer[vi_cursor] != '\n') {
                        vi_cursor++;
                    }
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'A') {
                    vi_cursor = vi_get_line_end(vi_cursor);
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'o') {
                    /* Open line below */
                    UINT16 end = vi_get_line_end(vi_cursor);
                    vi_cursor = end;
                    vi_insert_char('\n');
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'O') {
                    /* Open line above */
                    UINT16 start = vi_get_line_start(vi_cursor);
                    vi_cursor = start;
                    vi_insert_char('\n');
                    vi_cursor = start;
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'x' || key == KEY_DEL) {
                    vi_delete_char();
                } else if (ch == 'D') {
                    vi_delete_to_eol();
                } else if (ch == 'C') {
                    vi_delete_to_eol();
                    vi_mode = VI_MODE_INSERT;
                } else if (ch == 'd') {
                    /* Wait for second key */
                    UINT16 key2 = read_key();
                    char ch2 = (char)(key2 & 0xFF);
                    if (ch2 == 'd') {
                        vi_delete_line();
                    }
                } else if (ch == 'c') {
                    UINT16 key2 = read_key();
                    char ch2 = (char)(key2 & 0xFF);
                    if (ch2 == 'c') {
                        vi_change_line();
                        vi_mode = VI_MODE_INSERT;
                    }
                } else if (ch == 'y') {
                    /* Yank line */
                    UINT16 key2 = read_key();
                    char ch2 = (char)(key2 & 0xFF);
                    if (ch2 == 'y') {
                        UINT16 start = vi_get_line_start(vi_cursor);
                        UINT16 end = vi_get_line_end(vi_cursor);
                        if (end < vi_size && vi_buffer[end] == '\n') end++;
                        vi_yank_len = end - start;
                        if (vi_yank_len < sizeof(vi_yank_buf)) {
                            sima_memmove(vi_yank_buf, vi_buffer + start, vi_yank_len);
                        }
                    }
                } else if (ch == 'p') {
                    vi_paste();
                } else if (ch == 'J') {
                    vi_join_with_next_line();
                } else if (ch == 'r') {
                    UINT16 key2 = read_key();
                    char ch2 = (char)(key2 & 0xFF);
                    if (ch2 >= 32 && ch2 < 127 && vi_cursor < vi_size && vi_buffer[vi_cursor] != '\n') {
                        vi_buffer[vi_cursor] = ch2;
                        vi_modified = TRUE;
                    }
                } else if (ch == 'G') {
                    /* Go to end */
                    if (vi_size > 0) vi_cursor = vi_size - 1;
                } else if (ch == 'g') {
                    UINT16 key2 = read_key();
                    if ((key2 & 0xFF) == 'g') {
                        vi_cursor = 0; /* Go to start */
                    }
                } else if (ch == '^') {
                    vi_move_line_first_nonblank();
                } else if (ch == '0' || key == KEY_HOME) {
                    vi_cursor = vi_get_line_start(vi_cursor);
                } else if (ch == '$' || key == KEY_END) {
                    UINT16 end = vi_get_line_end(vi_cursor);
                    if (end > vi_get_line_start(vi_cursor)) {
                        vi_cursor = end - 1;
                    }
                } else if (ch == ':') {
                    vi_mode = VI_MODE_COMMAND;
                    vi_cmd_len = 0;
                    sima_memclr(vi_cmd_buf, sizeof(vi_cmd_buf));
                } else if (key == KEY_PGDN) {
                    UINT16 i;
                    for (i = 0; i < VI_ROWS - 1; i++) vi_move_down();
                } else if (key == KEY_PGUP) {
                    UINT16 i;
                    for (i = 0; i < VI_ROWS - 1; i++) vi_move_up();
                }
                break;

            case VI_MODE_INSERT:
                if ((key & 0xFF) == KEY_ESC) {
                    vi_mode = VI_MODE_NORMAL;
                    if (vi_cursor > 0) {
                        UINT16 ln_start = vi_get_line_start(vi_cursor);
                        if (vi_cursor > ln_start) {
                            vi_cursor--;
                        } else if (vi_cursor == vi_size && vi_size > 0 && vi_buffer[vi_size - 1] != '\n') {
                            vi_cursor--;
                        }
                    }
                } else if (ch == KEY_BACKSP) {
                    if (vi_cursor > 0) {
                        vi_cursor--;
                        vi_delete_char();
                    }
                } else if (key == KEY_LEFT) {
                    vi_move_left();
                } else if (key == KEY_RIGHT) {
                    vi_move_right();
                } else if (key == KEY_UP) {
                    vi_move_up();
                } else if (key == KEY_DOWN) {
                    vi_move_down();
                } else if (ch == KEY_ENTER) {
                    vi_insert_char('\n');
                } else if (ch >= 32 && ch < 127) {
                    vi_insert_char(ch);
                }
                break;

            case VI_MODE_COMMAND:
                if ((key & 0xFF) == KEY_ESC) {
                    vi_mode = VI_MODE_NORMAL;
                    vi_cmd_len = 0;
                } else if (ch == KEY_ENTER) {
                    running = vi_exec_cmd(name);
                    vi_mode = VI_MODE_NORMAL;
                    vi_cmd_len = 0;
                } else if (ch == KEY_BACKSP) {
                    if (vi_cmd_len > 0) {
                        vi_cmd_len--;
                        vi_cmd_buf[vi_cmd_len] = '\0';
                    } else {
                        vi_mode = VI_MODE_NORMAL;
                    }
                } else if (ch >= 32 && ch < 127 && vi_cmd_len < sizeof(vi_cmd_buf) - 1) {
                    vi_cmd_buf[vi_cmd_len++] = ch;
                    vi_cmd_buf[vi_cmd_len] = '\0';
                }
                break;
        }
    }
    
    clear_screen();
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

static BOOL run_fault(const char *kind) {
    if (!kind || kind[0] == '\0' || man_topic_eq(kind, "HELP")) {
        print_message("Usage: fault <kind>");
        print_message("Kinds: de/div0, bp/int3, ud/invalid, gp, of/overflow");
        return TRUE;
    }

    if (man_topic_eq(kind, "DE") || man_topic_eq(kind, "DIV0")) {
        debug_trigger_div0();
        return TRUE;
    }
    if (man_topic_eq(kind, "BP") || man_topic_eq(kind, "INT3")) {
        debug_trigger_bp();
        return TRUE;
    }
    if (man_topic_eq(kind, "UD") || man_topic_eq(kind, "INVALID")) {
        debug_trigger_ud();
        return TRUE;
    }
    if (man_topic_eq(kind, "GP")) {
        debug_trigger_gp();
        return TRUE;
    }
    if (man_topic_eq(kind, "OF") || man_topic_eq(kind, "OVERFLOW")) {
        debug_trigger_of();
        return TRUE;
    }

    print_simple("Unknown kind. Try: fault help");
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
    if (sima_strcmp(argv[0], "fault") == STRC_SAME) {
        if (argc == 1) return run_fault((const char*)0);
        if (argc == 2) return run_fault(argv[1]);
        return wrong_command_usage(buffer);
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
        sima_strcmp(argv[0], "fault") == STRC_SAME ||
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
