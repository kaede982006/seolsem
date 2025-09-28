#include "commands.h"
#include "sima_mem.h"
#include "sima_io.h"

BOOL run_buffer(char *buffer)
{
	int i;
    UINT16 argc;
    char *argv[16];
    if (!buffer || buffer[0] == '\0') return FALSE;

    argc = sima_strcspl(buffer, ' ',argv, (UINT16)16); /* ← 반드시 argv, &argv 아님 */
	print_message(argv[0]);
    if (argc == 0 || !argv[0] || argv[0][0] == '\0') return FALSE;
	
    /* argv[0] 가 명령어 */
    if (sima_strcmp(argv[0], "ver") == STRC_SAME) {
        run_ver();
        return TRUE;
    }
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.0. All Rights Reserved.");
}
