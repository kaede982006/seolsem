#include "commands.h"
#include "sima_conv.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_io.h"
#include "sima_program.h"

static char wrong_command_message[256];
static char message_buffer[256];

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
