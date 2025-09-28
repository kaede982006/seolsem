#include "sima_io.h"
#include "sima_type.h"
#include "sima_mem.h"
#include "commands.h"

static char current_path[256];
static char buffer[256];

void kernel_main(void) {
    if (!sima_memclr(current_path, (UINT16)sizeof(current_path))) {
        print_message("Unable to Initialize Local Path");
        for(;;) {}
    }
    if (!sima_strcpy(current_path, (UINT16)sizeof(current_path), "[drive0]:# $ ")) {
        print_message("Path buffer too small");
        for(;;) {}
    }
	for (;;) {
		wait_prompt(current_path, buffer);
		if (buffer[0]=='\0')
			continue;
		if(run_buffer(buffer)==FALSE) {
			char temp[256];

			sima_memclr(temp, (UINT16)sizeof(temp));
			sima_strcpy(temp, sizeof(temp), "Command not found: ");
			sima_strcat(temp, sizeof(temp), buffer);
			sima_strcpy(buffer,sizeof(buffer),temp);

			print_message(buffer);
		}
		(void)sima_memclr(buffer, (UINT16)sizeof(buffer));   /* 전체 clear */
	}
}
