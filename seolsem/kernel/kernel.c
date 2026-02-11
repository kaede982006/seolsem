#include "sima_io.h"
#include "sima_type.h"
#include "sima_mem.h"
#include "commands.h"
#include "sima_fs.h"
#include "sima_env.h"
#include "sima_user.h"
#include "sima_heap.h"

static char current_path[128];
static char buffer[64];
static char auth_pass[64];

#define PROMPT_PATH_MAX 48

static void build_prompt(char *out, UINT16 out_cap) {
    char cwd[FS_PATH_MAX];
    const char *user;
    const char *home;
    UINT16 display_len = 0;
    const char *display_ptr = cwd;
    UINT16 keep_len;
    UINT16 i;

    sima_memclr(cwd, (UINT16)sizeof(cwd));
    if (!fs_get_cwd(cwd, (UINT16)sizeof(cwd))) {
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "/");
    }
    sima_memclr(out, out_cap);
    home = env_get("HOME");
    if (home && home[0] != '\0' &&
        !(home[0] == '/' && home[1] == '\0')) {
        /* If cwd starts with HOME, display it as ~... (Linux-like prompt). */
        UINT16 j = 0;
        while (home[j] != '\0' && cwd[j] == home[j]) ++j;
        if (home[j] == '\0' && (cwd[j] == '\0' || cwd[j] == '/' || cwd[j] == '\\')) {
            sima_strcpy(out, out_cap, "~");
            sima_strcat(out, out_cap, &cwd[j]);
            display_ptr = out;
        }
    }
    display_len = sima_strlen(display_ptr);
    if (display_len > PROMPT_PATH_MAX) {
        sima_memclr(cwd, (UINT16)sizeof(cwd));
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "...");
        keep_len = (UINT16)(PROMPT_PATH_MAX - 3);
        i = (UINT16)(display_len - keep_len);
        sima_strcat(cwd, (UINT16)sizeof(cwd), &display_ptr[i]);
        display_ptr = cwd;
    }
    if (display_ptr == out) {
        sima_memclr(cwd, (UINT16)sizeof(cwd));
        sima_strcpy(cwd, (UINT16)sizeof(cwd), out);
        display_ptr = cwd;
    }

    user = env_get("USER");
    if (!user || user[0] == '\0') user = "ROOT";

    sima_memclr(out, out_cap);
    sima_strcpy(out, out_cap, "[");
    sima_strcat(out, out_cap, user);
    sima_strcat(out, out_cap, "@");
    sima_strcat(out, out_cap, display_ptr);
    sima_strcat(out, out_cap, "]$ ");
}

static void show_shell_banner(void) {
    clear_screen();
    print_message("Seolsem OS Version 1.1");
    print_message("Type HELP to see available commands.");
    print_message("");
}

static void login_screen(void) {
    for (;;) {
        clear_screen();
        print_message("Seolsem OS Version 1.1");
        print_message("Login required.");
        print_message("");
        for (;;) {
            sync_ds();
            wait_prompt("login: ", buffer);
            sync_ds();
            if (buffer[0] == '\0') {
                print_message("Login name required.");
                continue;
            }
            wait_prompt_masked("password: ", auth_pass);
            sync_ds();
            if (user_login(buffer, auth_pass)) {
                (void)sima_memclr(buffer, (UINT16)sizeof(buffer));
                (void)sima_memclr(auth_pass, (UINT16)sizeof(auth_pass));
                show_shell_banner();
                return;
            }
            print_message("Login incorrect.");
            (void)sima_memclr(buffer, (UINT16)sizeof(buffer));
            (void)sima_memclr(auth_pass, (UINT16)sizeof(auth_pass));
        }
    }
}

void kernel_main(void) {
    BOOL fs_ok;
    BOOL env_ok;
    UINT16 cmd_ok;
    /* Keep IRQs disabled during early init, then enable after IDT/PIC are ready. */
    disable_irq();
    sync_ds();

    show_shell_banner();
    if (!sima_memclr(current_path, (UINT16)sizeof(current_path))) {
        print_message("Unable to Initialize Local Path");
        for(;;) {}
    }
    fs_ok = fs_init();
    if (!fs_ok) {
        print_message("Filesystem init failed.");
    }
    env_ok = env_init();
    if (!env_ok) {
        print_message("Environment init failed.");
    }
    if (!fs_ok) {
        print_message("Login database fallback: in-memory ROOT only.");
    }
    (void)user_init();
    enable_irq();

    build_prompt(current_path, (UINT16)sizeof(current_path));
	for (;;) {
        if (!user_is_logged_in()) {
            login_screen();
        }
        sync_ds();
        build_prompt(current_path, (UINT16)sizeof(current_path));

		wait_prompt(current_path, buffer);

        sync_ds();

		if (buffer[0]=='\0') {
			continue;
        }

        cmd_ok = run_buffer(buffer) ? 1U : 0U;
		if (cmd_ok == 0U) {
            sima_memclr(current_path, (UINT16)sizeof(current_path));
            sima_strcpy(current_path, (UINT16)sizeof(current_path), "Command not found: ");
            sima_strcat(current_path, (UINT16)sizeof(current_path), buffer);
			print_message(current_path);
            sync_ds();
		}
		(void)sima_memclr(buffer, (UINT16)sizeof(buffer));
	}
}
