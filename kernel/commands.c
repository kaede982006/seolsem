#include "commands.h"
#include "sima_mem.h"
#include "sima_io.h"

static char wrong_command_message[256];

BOOL wrong_command_usage(char* buffer) {
	
	sima_memclr(wrong_command_message, (UINT16)sizeof(wrong_command_message));
	sima_strcpy(wrong_command_message,sizeof(wrong_command_message),"Wrong command usage: ");
	sima_strcat(wrong_command_message, sizeof(wrong_command_message), buffer);
	print_message(wrong_command_message);
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
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.0. All Rights Reserved.");
}
