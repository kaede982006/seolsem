#include "sima_program.h"
#include "sima_fs.h"
#include "sima_io.h"
#include "sima_mem.h"

#define PROGRAM_MAX_SIZE 1024

static UINT8 program_buffer[PROGRAM_MAX_SIZE + 1];
static UINT16 program_size;
static BOOL program_loaded;

static void program_handle_line(const char *line) {
    while (*line == ' ') ++line;
    if (*line == '\0') return;

    if (sima_strncmp(line, "PRINT ", 6) == STRC_SAME) {
        print_message(line + 6);
        return;
    }
    if (sima_strcmp(line, "CLS") == STRC_SAME) {
        clear_screen();
        return;
    }
    if (sima_strcmp(line, "EXIT") == STRC_SAME) {
        program_unload();
        return;
    }
    print_message("Program error: Unknown instruction");
}

BOOL program_load(const char *name) {
    UINT32 size;
    if (!name) return FALSE;
    if (!fs_is_executable(name)) return FALSE;
    if (!fs_read(name, program_buffer, PROGRAM_MAX_SIZE, &size)) return FALSE;

    program_size = (UINT16)size;
    program_buffer[program_size] = '\0';
    program_loaded = TRUE;
    return TRUE;
}

BOOL program_load_in_dir(const char *dir_name, const char *name) {
    UINT32 size;
    if (!dir_name || !name) return FALSE;
    if (!fs_is_executable_in_dir(dir_name, name)) return FALSE;
    if (!fs_read_in_dir(dir_name, name, program_buffer, PROGRAM_MAX_SIZE, &size)) return FALSE;

    program_size = (UINT16)size;
    program_buffer[program_size] = '\0';
    program_loaded = TRUE;
    return TRUE;
}

BOOL program_run(void) {
    UINT16 i;
    UINT16 start;
    if (!program_loaded) return FALSE;

    start = 0;
    for (i = 0; i <= program_size; ++i) {
        UINT8 ch = program_buffer[i];
        BOOL eol = FALSE;

        if (ch == '\r') eol = TRUE;
        if (ch == '\n' || ch == '\0') eol = TRUE;

        if (eol) {
            char line[128];
            UINT16 len = (UINT16)(i - start);

            if (len >= sizeof(line)) len = (UINT16)sizeof(line) - 1;
            sima_memcpy(line, &program_buffer[start], len);
            line[len] = '\0';
            program_handle_line(line);
            if (!program_loaded) break;

            /* Handle CRLF: if we ended on CR and next is LF, skip LF too. */
            if (ch == '\r' && i < program_size && program_buffer[i + 1] == '\n') {
                i = (UINT16)(i + 1);
            }
            start = (UINT16)(i + 1);
        }
    }
    return TRUE;
}

void program_unload(void) {
    program_loaded = FALSE;
    program_size = 0;
    sima_memset(program_buffer, 0, (UINT16)sizeof(program_buffer));
}
