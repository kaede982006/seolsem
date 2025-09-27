#include "simaio.h"
#include "sima_type.h"
#include "sima_mem.h"

static char current_path[256];

void kernel_main() {
	BOOL ok = memcl(current_path, (UINT16)sizeof(current_path));
	if (!ok) {
		print_message("Unable to Initialize Local Path");
		while (1) { }
	}

	ok = sima_strcpy(current_path, (UINT16)sizeof(current_path), "[drive0]:# $ ");
	if (!ok) {
		print_message("Path buffer too small");
		while (1) { }
	}
	while (1) {
		wait_prompt(current_path);
	}
}
