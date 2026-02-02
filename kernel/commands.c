#include "commands.h"
#include "sima_conv.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_io.h"
#include "sima_program.h"

static char wrong_command_message[256];
static char message_buffer[256];

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

    editor_index_to_pos(buffer, size, cursor, &doc_row, &doc_col);
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

    editor_index_to_pos(buffer, size, cursor, &doc_row, &doc_col);
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

static void print_simple(const char *text) {
    sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
    sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), text);
    print_message(message_buffer);
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

void run_help() {
    print_message("Available commands:");
    print_message("  ver             - Show OS version");
    print_message("  help            - Show this help");
    print_message("  cls             - Clear screen");
    print_message("  ls              - List files");
    print_message("  cat <file>      - Display file contents");
    print_message("  write <file> <data> - Write text to file");
    print_message("  edit <file>     - Open memo editor");
    print_message("  rm <file>       - Delete file");
    print_message("  load <file>     - Load program to memory");
    print_message("  run             - Run loaded program");
    print_message("  exec <file>     - Load and run program");
    print_message("  sync            - Save filesystem to disk (IDE)");
}

static BOOL run_ls(void) {
    UINT16 i;
    FS_FILE entry;
    char size_buffer[16];

    for (i = 0; i < FS_MAX_FILES; ++i) {
        if (!fs_get_entry(i, &entry)) continue;
        if (!entry.used) continue;

        sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
        sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), entry.name);
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " ");
        sima_utoa(entry.size, size_buffer, (UINT16)sizeof(size_buffer), 10);
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), size_buffer);
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " bytes ");
        sima_strcat(message_buffer, (UINT16)sizeof(message_buffer),
                    entry.executable ? "[EXEC]" : "[DATA]");
        print_message(message_buffer);
    }
    return TRUE;
}

static BOOL run_cat(const char *name) {
    UINT8 data[FS_BLOCK_SIZE + 1];
    UINT16 size;

    if (!name) return FALSE;
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
    size = sima_strlen(data);
    if (!fs_write(name, (const UINT8*)data, size)) {
        print_simple("Unable to write file.");
        return TRUE;
    }
    print_simple("Write complete.");
    return TRUE;
}

static BOOL run_edit(const char *name) {
    if (!name) return FALSE;

    {
        char buffer[EDIT_MAX_SIZE + 1];
        UINT16 size = 0;
        UINT16 cursor = 0;
        UINT16 scroll_row = 0;
        UINT16 desired_col = 0;
        BOOL modified = FALSE;
        char status[EDIT_COLS + 1];
        UINT16 read_size = 0;

        sima_memclr(buffer, (UINT16)sizeof(buffer));
        if (fs_read(name, (UINT8*)buffer, EDIT_MAX_SIZE, &read_size)) {
            size = read_size;
            buffer[size] = '\0';
        }

        clear_screen();
        sima_strcpy(status, (UINT16)sizeof(status),
                    "Ctrl+S Save  Esc Save&Exit  Ctrl+Q Quit  Arrows Move");

        for (;;) {
            editor_render(name, buffer, size, cursor, &scroll_row, modified, status);
            UINT16 key = read_key();
            UINT8 ascii = (UINT8)(key & 0xFF);
            UINT8 scan = (UINT8)((key >> 8) & 0xFF);

            if (ascii == KEY_ESC) {
                if (!fs_write(name, (const UINT8*)buffer, size)) {
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
                if (!fs_write(name, (const UINT8*)buffer, size)) {
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
    if (!fs_delete(name)) {
        print_simple("Unable to delete file.");
        return TRUE;
    }
    print_simple("File deleted.");
    return TRUE;
}

static BOOL run_load(const char *name) {
    if (!name) return FALSE;
    if (!program_load(name)) {
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
    if (!program_load(name)) {
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

BOOL run_buffer(char *buffer)
{
    UINT16 argc;
    char *argv[16];
	char temp[256];
    if (!buffer || buffer[0] == '\0') return FALSE;

	sima_strcpy(temp,sizeof(temp),buffer);
    argc = sima_strcspl(temp, ' ', argv, (UINT16)16); /* ← 반드시 argv, &argv 아님 */
    if (argc == 0 || !argv[0] || argv[0][0] == '\0') return FALSE;
	
    /* argv[0] 가 명령어 */
    if (sima_strcmp(argv[0], "ver") == STRC_SAME && argc==1) {
        run_ver();
        return TRUE;
    }
	else if(sima_strcmp(argv[0], "ver") == STRC_SAME) {
		return wrong_command_usage(buffer);
	}
    if (sima_strcmp(argv[0], "help") == STRC_SAME && argc == 1) {
        run_help();
        return TRUE;
    }
    if (sima_strcmp(argv[0], "cls") == STRC_SAME && argc == 1) {
        return run_cls();
    }
    if (sima_strcmp(argv[0], "ls") == STRC_SAME && argc == 1) {
        return run_ls();
    }
    if (sima_strcmp(argv[0], "cat") == STRC_SAME && argc == 2) {
        return run_cat(argv[1]);
    }
    if (sima_strcmp(argv[0], "write") == STRC_SAME && argc >= 3) {
        const char *data = skip_tokens(buffer, 2);
        return run_write(argv[1], data);
    }
    if (sima_strcmp(argv[0], "edit") == STRC_SAME && argc == 2) {
        return run_edit(argv[1]);
    }
    if (sima_strcmp(argv[0], "rm") == STRC_SAME && argc == 2) {
        return run_rm(argv[1]);
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
    if (sima_strcmp(argv[0], "help") == STRC_SAME ||
        sima_strcmp(argv[0], "cls") == STRC_SAME ||
        sima_strcmp(argv[0], "ls") == STRC_SAME ||
        sima_strcmp(argv[0], "cat") == STRC_SAME ||
        sima_strcmp(argv[0], "write") == STRC_SAME ||
        sima_strcmp(argv[0], "edit") == STRC_SAME ||
        sima_strcmp(argv[0], "rm") == STRC_SAME ||
        sima_strcmp(argv[0], "load") == STRC_SAME ||
        sima_strcmp(argv[0], "run") == STRC_SAME ||
        sima_strcmp(argv[0], "exec") == STRC_SAME ||
        sima_strcmp(argv[0], "sync") == STRC_SAME) {
        return wrong_command_usage(buffer);
    }
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.0. All Rights Reserved.");
}
