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
    UINT16 size;
    if (!name) return FALSE;
    if (!fs_is_executable(name)) return FALSE;
    if (!fs_read(name, program_buffer, PROGRAM_MAX_SIZE, &size)) return FALSE;

    program_size = size;
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
        if (program_buffer[i] == '\r') continue;
        if (program_buffer[i] == '\n' || program_buffer[i] == '\0') {
            char line[128];
            UINT16 len = (UINT16)(i - start);
            if (len >= sizeof(line)) len = (UINT16)sizeof(line) - 1;
            sima_memcpy(line, &program_buffer[start], len);
            line[len] = '\0';
            program_handle_line(line);
            if (!program_loaded) break;
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
