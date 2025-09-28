#include "simaio.h"
#include "sima_type.h"
#include "sima_mem.h"

static char current_path[256];
static char buffer[256];

void kernel_main(void) {
    if (!memcl(current_path, (UINT16)sizeof(current_path))) {
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
		print_message(buffer);
		(void)memcl(buffer, (UINT16)sizeof(buffer));   /* 전체 clear */
	}
}
