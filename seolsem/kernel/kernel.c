#include "sima_io.h"
#include "sima_type.h"
#include "sima_mem.h"
#include "commands.h"
#include "sima_fs.h"
#include "sima_env.h"

static char current_path[256];
static char buffer[256];

static void build_prompt(char *out, UINT16 out_cap) {
    char cwd[FS_PATH_MAX];
    char display[FS_PATH_MAX];
    const char *home;

    sima_memclr(cwd, (UINT16)sizeof(cwd));
    if (!fs_get_cwd(cwd, (UINT16)sizeof(cwd))) {
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "/");
    }

    sima_memclr(display, (UINT16)sizeof(display));
    home = env_get("HOME");
    if (home && home[0] != '\0' &&
        !(home[0] == '/' && home[1] == '\0')) {
        /* If cwd starts with HOME, display it as ~... (Linux-like prompt). */
        UINT16 i = 0;
        while (home[i] != '\0' && cwd[i] == home[i]) ++i;
        if (home[i] == '\0' && (cwd[i] == '\0' || cwd[i] == '/' || cwd[i] == '\\')) {
            sima_strcpy(display, (UINT16)sizeof(display), "~");
            sima_strcat(display, (UINT16)sizeof(display), &cwd[i]);
        }
    }
    if (display[0] == '\0') {
        sima_strcpy(display, (UINT16)sizeof(display), cwd);
    }

    sima_memclr(out, out_cap);
    sima_strcpy(out, out_cap, "[");
    sima_strcat(out, out_cap, display);
    sima_strcat(out, out_cap, "]$ ");
}

void kernel_main(void) {
    /* Keep IRQs disabled during early init: BIOS IRQ handlers may clobber DS in real mode. */
    disable_irq();
    sync_ds();

    clear_screen();
    print_message("Seolsem OS Version 1.0");
    print_message("Type HELP to see available commands.");
    print_message("");
    if (!sima_memclr(current_path, (UINT16)sizeof(current_path))) {
        print_message("Unable to Initialize Local Path");
        for(;;) {}
    }
    if (!fs_init()) {
        print_message("Filesystem init failed.");
    }
    if (!env_init()) {
        print_message("Environment init failed.");
    }
    build_prompt(current_path, (UINT16)sizeof(current_path));
	for (;;) {
        /* Build prompt with IRQs off so DS-dependent C code stays safe. */
        disable_irq();
        sync_ds();
        build_prompt(current_path, (UINT16)sizeof(current_path));
        enable_irq();
		wait_prompt(current_path, buffer);
        disable_irq();
        sync_ds();
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
