/*
 * os_seolsem.c - Platform-specific routines for vim on seolsem
 * Based on os_msdos.c from vim 7.3a
 */

#include "seolsem_libc.h"

/* Seolsem kernel API declarations */
extern UINT16 read_key(void);
extern void write_char(UINT16 row, UINT16 col, char c, UINT8 attr);
extern void set_cursor(UINT16 row, UINT16 col);
extern void clear_screen(void);
extern void print_message(const char *msg);
extern UINT8 kbd_ctrl;  /* Ctrl key state */
extern UINT8 kbd_shift; /* Shift key state */
extern UINT8 kbd_alt;   /* Alt key state */

/* Forward declarations for vim */
typedef unsigned char char_u;
typedef unsigned short short_u;
typedef unsigned int int_u;
typedef unsigned long long_u;

#define SIZEOF_INT 2

/* Global vim variables we need */
extern int Rows;
extern int Columns;
extern int full_screen;
extern int term_console;

/* Screen state */
static int current_row = 0;
static int current_col = 0;
static unsigned char current_attr = 0x07;  /* Normal white on black */

/*
 * mch_early_init() - Early initialization
 */
void mch_early_init(void) {
    /* Nothing needed */
}

/*
 * mch_init() - Initialize the machine-specific parts
 */
void mch_init(void) {
    Rows = 25;
    Columns = 80;
    full_screen = 1;
    term_console = 1;
    clear_screen();
}

/*
 * mch_exit() - Exit vim, return to shell
 */
void mch_exit(int r) {
    clear_screen();
    /* Return control to seolsem shell */
    (void)r;
}

/*
 * mch_get_shellsize() - Get terminal dimensions
 */
void mch_get_shellsize(void) {
    Rows = 25;
    Columns = 80;
}

/*
 * mch_set_shellsize() - Set terminal dimensions (not supported)
 */
void mch_set_shellsize(void) {
    /* Can't change terminal size */
}

/*
 * mch_new_shellsize() - Shell size changed (not applicable)
 */
void mch_new_shellsize(void) {
    /* No-op */
}

/*
 * mch_write() - Write characters to the screen
 */
void mch_write(char_u *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        
        if (c == '\n') {
            current_col = 0;
            current_row++;
            if (current_row >= 25) {
                current_row = 24;
                /* Should scroll here */
            }
        } else if (c == '\r') {
            current_col = 0;
        } else if (c == '\b') {
            if (current_col > 0) current_col--;
        } else if (c == '\t') {
            do {
                write_char(current_row, current_col++, ' ', current_attr);
            } while ((current_col % 8) != 0 && current_col < 80);
        } else if (c >= 32) {
            write_char(current_row, current_col++, c, current_attr);
            if (current_col >= 80) {
                current_col = 0;
                current_row++;
                if (current_row >= 25) current_row = 24;
            }
        }
    }
    set_cursor(current_row, current_col);
}

/*
 * mch_inchar() - Read keyboard input
 * Returns number of characters read, -1 for interrupt
 */
int mch_inchar(char_u *buf, int maxlen, long wtime, int tb_change_cnt) {
    UINT16 key;
    int count = 0;
    
    (void)wtime;
    (void)tb_change_cnt;
    
    if (maxlen <= 0) return 0;
    
    key = read_key();
    
    /* Check for Ctrl-C */
    if ((key & 0xFF) == 0x03) {
        return -1;  /* Interrupt */
    }
    
    /* Extended key (scan code in high byte) */
    if ((key & 0xFF) == 0) {
        UINT8 scan = (key >> 8) & 0xFF;
        
        /* Convert scan codes to vim key codes */
        buf[count++] = 0x80;  /* K_SPECIAL */
        
        switch (scan) {
            case 0x48: /* Up */
                buf[count++] = 'k';
                buf[count++] = 'u';
                break;
            case 0x50: /* Down */
                buf[count++] = 'k';
                buf[count++] = 'd';
                break;
            case 0x4B: /* Left */
                buf[count++] = 'k';
                buf[count++] = 'l';
                break;
            case 0x4D: /* Right */
                buf[count++] = 'k';
                buf[count++] = 'r';
                break;
            case 0x47: /* Home */
                buf[count++] = 'k';
                buf[count++] = 'h';
                break;
            case 0x4F: /* End */
                buf[count++] = '@';
                buf[count++] = '7';
                break;
            case 0x49: /* Page Up */
                buf[count++] = 'k';
                buf[count++] = 'P';
                break;
            case 0x51: /* Page Down */
                buf[count++] = 'k';
                buf[count++] = 'N';
                break;
            case 0x53: /* Delete */
                buf[count++] = 'k';
                buf[count++] = 'D';
                break;
            case 0x52: /* Insert */
                buf[count++] = 'k';
                buf[count++] = 'I';
                break;
            default:
                /* Unknown extended key */
                count = 0;
                break;
        }
    } else {
        /* Normal ASCII character */
        buf[count++] = key & 0xFF;
    }
    
    return count;
}

/*
 * mch_char_avail() - Check if a character is available
 */
int mch_char_avail(void) {
    /* Could check keyboard buffer */
    return 0;
}

/*
 * mch_delay() - Delay for msec milliseconds
 */
void mch_delay(long msec, int ignoreinput) {
    /* Simple busy-wait delay */
    volatile long i;
    (void)ignoreinput;
    for (i = 0; i < msec * 100; i++);
}

/*
 * mch_breakcheck() - Check for break (Ctrl-C)
 */
void mch_breakcheck(void) {
    /* Check keyboard state */
    if (kbd_ctrl) {
        /* Could set got_int = TRUE here */
    }
}

/*
 * mch_settmode() - Set terminal mode
 */
void mch_settmode(int tmode) {
    (void)tmode;
    /* Already in raw mode on seolsem */
}

/*
 * mch_get_user_name() - Get current user name
 */
int mch_get_user_name(char_u *s, int len) {
    strncpy((char *)s, "root", len);
    return 0;
}

/*
 * mch_get_host_name() - Get host name
 */
void mch_get_host_name(char_u *s, int len) {
    strncpy((char *)s, "seolsem", len);
}

/*
 * mch_dirname() - Get current directory name
 */
int mch_dirname(char_u *buf, int len) {
    strncpy((char *)buf, "/", len);
    return 0;
}

/*
 * mch_FullName() - Make a full path name
 */
int mch_FullName(char_u *fname, char_u *buf, int len, int force) {
    (void)force;
    strncpy((char *)buf, (char *)fname, len);
    return 0;
}

/*
 * mch_isFullName() - Check if path is absolute
 */
int mch_isFullName(char_u *fname) {
    return (fname[0] == '/' || fname[0] == '\\');
}

/*
 * mch_getperm() - Get file permissions
 */
long mch_getperm(char_u *name) {
    (void)name;
    return 0666;  /* rw-rw-rw- */
}

/*
 * mch_setperm() - Set file permissions (not supported)
 */
int mch_setperm(char_u *name, long perm) {
    (void)name;
    (void)perm;
    return 0;
}

/*
 * mch_isdir() - Check if path is a directory
 */
int mch_isdir(char_u *name) {
    (void)name;
    return 0;  /* Seolsem has flat filesystem */
}

/*
 * mch_can_exe() - Check if file is executable
 */
int mch_can_exe(char_u *name) {
    (void)name;
    return 0;
}

/*
 * mch_nodetype() - Return type of path
 */
int mch_nodetype(char_u *name) {
    (void)name;
    return 0;  /* NODE_NORMAL */
}

/*
 * mch_fopen() - Open a file
 */
FILE *mch_fopen(char *name, char *mode) {
    return fopen(name, mode);
}

/*
 * vim_strncpy() - Safe string copy with length limit
 */
void vim_strncpy(char_u *to, char_u *from, size_t len) {
    strncpy((char *)to, (char *)from, len);
    to[len] = '\0';
}

/*
 * vim_strncpy() - Safe string copy with length limit
 */
char_u *vim_strsave(char_u *string) {
    char_u *p;
    size_t len = strlen((char *)string) + 1;
    p = (char_u *)malloc(len);
    if (p != NULL) {
        memcpy(p, string, len);
    }
    return p;
}

/*
 * out_flush() - Flush output buffer
 */
void out_flush(void) {
    set_cursor(current_row, current_col);
}

/*
 * out_char() - Output a single character
 */
void out_char(int c) {
    char_u ch = (char_u)c;
    mch_write(&ch, 1);
}

/*
 * out_str() - Output a string
 */
void out_str(char_u *s) {
    if (s != NULL) {
        mch_write(s, (int)strlen((char *)s));
    }
}

/*
 * screen_start() - Screen output starts
 */
void screen_start(void) {
    /* No-op */
}

/*
 * cursor_on() - Make cursor visible
 */
void cursor_on(void) {
    set_cursor(current_row, current_col);
}

/*
 * cursor_off() - Hide cursor (not supported)
 */
void cursor_off(void) {
    /* Can't hide cursor */
}

/*
 * windgoto() - Move cursor to row, col
 */
void windgoto(int row, int col) {
    current_row = row;
    current_col = col;
    set_cursor(row, col);
}

/*
 * msg_col - Message column position
 */
int msg_col = 0;

/*
 * msg_row - Message row position
 */
int msg_row = 24;

/*
 * beep_flush() - Beep and flush output
 */
void beep_flush(void) {
    /* Beep not implemented */
}

/*
 * vim_beep() - Just beep
 */
void vim_beep(void) {
    /* Beep not implemented */
}

/*
 * scroll_region() related - stubs
 */
int scroll_region = 0;

void scroll_region_set(void *wp, int off) {
    (void)wp;
    (void)off;
}

void scroll_region_reset(void) {
}

/*
 * alloc() - Allocate memory
 */
char_u *alloc(unsigned size) {
    return (char_u *)malloc(size);
}

/*
 * alloc_clear() - Allocate zeroed memory
 */
char_u *alloc_clear(unsigned size) {
    return (char_u *)calloc(1, size);
}

/*
 * vim_free() - Free memory
 */
void vim_free(void *x) {
    free(x);
}

/*
 * lalloc() - Allocate memory (long size)
 */
char_u *lalloc(long_u size, int message) {
    (void)message;
    if (size > 0xFFFF) return NULL;  /* 16-bit limit */
    return (char_u *)malloc((size_t)size);
}

/*
 * mch_avail_mem() - Get available memory
 */
long_u mch_avail_mem(int special) {
    (void)special;
    return 32768;  /* Report 32KB available */
}

/*
 * mch_call_shell() - Call external program (not supported)
 */
int mch_call_shell(char_u *cmd, int options) {
    (void)cmd;
    (void)options;
    return -1;
}

/*
 * mch_expand_wildcards() - Expand wildcards (not supported)
 */
int mch_expand_wildcards(int num_pat, char_u **pat, int *num_file,
                         char_u ***file, int flags) {
    (void)num_pat;
    (void)pat;
    (void)flags;
    *num_file = 0;
    *file = NULL;
    return -1;
}

/*
 * mch_expandpath() - Expand path (minimal)
 */
int mch_expandpath(void *gap, char_u *path, int flags) {
    (void)gap;
    (void)path;
    (void)flags;
    return 0;
}

/*
 * mch_has_wildcard() - Check for wildcards
 */
int mch_has_wildcard(char_u *p) {
    while (*p) {
        if (*p == '*' || *p == '?') return 1;
        p++;
    }
    return 0;
}

/*
 * mch_hide() - Hide file (not supported)
 */
void mch_hide(char_u *name) {
    (void)name;
}

/*
 * mch_remove() - Remove file
 */
int mch_remove(char_u *name) {
    return remove((char *)name);
}

/*
 * Global variables needed by vim
 */
int Rows = 25;
int Columns = 80;
int full_screen = 0;
int term_console = 1;
