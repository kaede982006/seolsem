#include "commands.h"
#include "sima_conv.h"
#include "sima_fs.h"
#include "sima_mem.h"
#include "sima_io.h"
#include "sima_program.h"
#include "sima_env.h"
#include "sima_ide.h"
#include "sima_user.h"
#include "sima_perm.h"

static char wrong_command_message[256];
static char message_buffer[256];
static void print_simple(const char *text);

#define EDIT_COLS 80
#define EDIT_ROWS 23
#define EDIT_HEADER_ROW 0
#define EDIT_CONTENT_ROW 1
#define EDIT_STATUS_ROW 24
#define EDIT_MAX_SIZE FS_BLOCK_SIZE

#define KEY_ESC 27
#define KEY_ENTER 13
#define KEY_BACKSPACE 8
#define KEY_CTRL_S 19
#define KEY_CTRL_Q 17

#define SCAN_LEFT 0x4B
#define SCAN_RIGHT 0x4D
#define SCAN_UP 0x48
#define SCAN_DOWN 0x50
#define SCAN_HOME 0x47
#define SCAN_END 0x4F
#define SCAN_DEL 0x53
#define SCAN_PGUP 0x49
#define SCAN_PGDN 0x51
#define SCAN_F1 0x3B
#define SCAN_F10 0x44

/* seolsem keyboard driver notes:
 * - Alt press (make) generates scan 0x38 (ascii 0). Break codes are ignored.
 * - Extended keys use E0 prefix; read_key() returns ascii 0 and scan in AH.
 */
#define SCAN_ALT 0x38

/* Legacy BIOS/INT16h scan codes for some Alt+extended keys.
 * seolsem/read_key() typically does NOT generate these, but keeping them
 * here doesn't hurt for future compatibility.
 */
#define SCAN_ALT_LEFT 0x9B
#define SCAN_ALT_RIGHT 0x9D
#define SCAN_ALT_UP 0x98
#define SCAN_ALT_DOWN 0xA0

/* letter scan codes (set1); used for menu shortcuts in some environments */
#define SCAN_KEY_F 0x21
#define SCAN_KEY_E 0x12
#define SCAN_KEY_H 0x23

/* Color attributes (VGA text mode: background<<4 | foreground).
 * Use strong-contrast pairs and avoid foreground=0 for selected items.
 */
#define EDIT_ATTR_TEXT            0x07  /* light gray on black */

/* DOS-like colors (VGA text mode: background<<4 | foreground).
 * Keep strong contrast and avoid "invisible" combinations.
 */
#define EDIT_ATTR_MENU_BAR_BG     0x1F  /* white on blue */
#define EDIT_ATTR_MENU_BAR_TXT    0x1F  /* white on blue */
#define EDIT_ATTR_MENU_BAR_HOTKEY 0x1E  /* yellow on blue */
#define EDIT_ATTR_MENU_BAR_SEL    0x70  /* black on light gray */

#define EDIT_ATTR_MENU_BORDER     0x1F  /* white on blue */
#define EDIT_ATTR_MENU_ITEM       0x70  /* black on light gray */
#define EDIT_ATTR_MENU_ITEM_SEL   0x1F  /* white on blue */

#define EDIT_ATTR_STATUS_BG       0x1F  /* white on blue */
#define EDIT_ATTR_STATUS_TXT      0x1F  /* white on blue */

static void editor_draw_row(UINT8 row, const char *text, UINT8 attr) {
    UINT8 col;
    for (col = 0; col < EDIT_COLS; ++col) {
        char ch = ' ';
        if (text && text[col] != '\0') ch = text[col];
        write_char(row, col, ch, attr);
    }
}


typedef struct {
    const char *label;
    const char *shortcut;
    UINT8 action;
} EDIT_MENU_ITEM;

enum {
    EDIT_ACT_NONE = 0,
    EDIT_ACT_SAVE = 1,
    EDIT_ACT_EXIT = 2,
    EDIT_ACT_QUIT = 3,
    EDIT_ACT_HELP_KEYS = 4,
    EDIT_ACT_HELP_ABOUT = 5
};

enum {
    EDIT_MENU_FILE = 0,
    EDIT_MENU_EDIT = 1,
    EDIT_MENU_HELP = 2
};

static const EDIT_MENU_ITEM edit_menu_file[] = {
    { "Save", "Ctrl+S", EDIT_ACT_SAVE },
    { "Exit", "Esc",    EDIT_ACT_EXIT },
    { "Quit", "Ctrl+Q", EDIT_ACT_QUIT }
};

static const EDIT_MENU_ITEM edit_menu_edit[] = {
    { "Cut",   "", EDIT_ACT_NONE },
    { "Copy",  "", EDIT_ACT_NONE },
    { "Paste", "", EDIT_ACT_NONE }
};

static const EDIT_MENU_ITEM edit_menu_help[] = {
    { "Keys",  "F1", EDIT_ACT_HELP_KEYS },
    { "About", "",   EDIT_ACT_HELP_ABOUT }
};

static UINT8 editor_menu_count(UINT8 menu) {
    if (menu == EDIT_MENU_FILE) return (UINT8)(sizeof(edit_menu_file) / sizeof(edit_menu_file[0]));
    if (menu == EDIT_MENU_EDIT) return (UINT8)(sizeof(edit_menu_edit) / sizeof(edit_menu_edit[0]));
    return (UINT8)(sizeof(edit_menu_help) / sizeof(edit_menu_help[0]));
}

static const EDIT_MENU_ITEM* editor_menu_item(UINT8 menu, UINT8 idx) {
    if (menu == EDIT_MENU_FILE) return &edit_menu_file[idx];
    if (menu == EDIT_MENU_EDIT) return &edit_menu_edit[idx];
    return &edit_menu_help[idx];
}

static void editor_fill(UINT8 row, UINT8 col, UINT8 width, char ch, UINT8 attr) {
    UINT8 i;
    for (i = 0; i < width && (UINT16)(col + i) < EDIT_COLS; ++i) {
        write_char(row, (UINT8)(col + i), ch, attr);
    }
}

static void editor_write_text(UINT8 row, UINT8 col, const char *text, UINT8 attr) {
    UINT8 i = 0;
    while (text && text[i] != '\0' && (UINT16)(col + i) < EDIT_COLS) {
        write_char(row, (UINT8)(col + i), text[i], attr);
        ++i;
    }
}

static void editor_draw_menu_overlay(UINT8 menu_sel, UINT8 item_sel) {
    /* DOS-like dropdown box under the menu bar (colored + bordered) */
    UINT8 i;
    UINT8 count = editor_menu_count(menu_sel);
    UINT8 box_top = (UINT8)(EDIT_HEADER_ROW + 1);
    UINT8 box_left = 1;
    UINT8 max_label = 0;
    UINT8 max_short = 0;
    UINT8 inner_w;
    UINT8 box_w;
    UINT8 box_h;

    if (menu_sel == EDIT_MENU_EDIT) box_left = 8;
    if (menu_sel == EDIT_MENU_HELP) box_left = 15;

    /* compute a reasonable width so text doesn't overlap */
    for (i = 0; i < count; ++i) {
        const EDIT_MENU_ITEM *it = editor_menu_item(menu_sel, i);
        UINT8 ll = (UINT8)sima_strlen(it->label);
        if (ll > max_label) max_label = ll;
        if (it->shortcut && it->shortcut[0] != '\0') {
            UINT8 sl = (UINT8)sima_strlen(it->shortcut);
            if (sl > max_short) max_short = sl;
        }
    }

    /* inside the border: [marker+space] + label + [2 spaces + shortcut] */
    inner_w = (UINT8)(2 + max_label);
    if (max_short > 0) inner_w = (UINT8)(inner_w + 2 + max_short);
    if (inner_w < 18) inner_w = 18;
    if (inner_w > 50) inner_w = 50;

    box_w = (UINT8)(inner_w + 2); /* border left/right */
    box_h = (UINT8)(count + 2);   /* border top/bottom */

    /* keep box fully on screen */
    if ((UINT16)(box_left + box_w) > EDIT_COLS) {
        if (box_w < EDIT_COLS) box_left = (UINT8)(EDIT_COLS - box_w);
        else box_left = 0;
    }
    if ((UINT16)(box_top + box_h) > EDIT_STATUS_ROW) {
        if (box_h < EDIT_STATUS_ROW) box_top = (UINT8)(EDIT_STATUS_ROW - box_h);
    }

    /* fill background */
    for (i = 0; i < box_h; ++i) {
        editor_fill((UINT8)(box_top + i), box_left, box_w, ' ', EDIT_ATTR_MENU_ITEM);
    }

    /* border */
    {
        UINT8 c;
        UINT8 r;
        for (c = 0; c < box_w; ++c) {
            write_char(box_top, (UINT8)(box_left + c), '-', EDIT_ATTR_MENU_BORDER);
            write_char((UINT8)(box_top + box_h - 1), (UINT8)(box_left + c), '-', EDIT_ATTR_MENU_BORDER);
        }
        for (r = 0; r < box_h; ++r) {
            write_char((UINT8)(box_top + r), box_left, '|', EDIT_ATTR_MENU_BORDER);
            write_char((UINT8)(box_top + r), (UINT8)(box_left + box_w - 1), '|', EDIT_ATTR_MENU_BORDER);
        }
        write_char(box_top, box_left, '+', EDIT_ATTR_MENU_BORDER);
        write_char(box_top, (UINT8)(box_left + box_w - 1), '+', EDIT_ATTR_MENU_BORDER);
        write_char((UINT8)(box_top + box_h - 1), box_left, '+', EDIT_ATTR_MENU_BORDER);
        write_char((UINT8)(box_top + box_h - 1), (UINT8)(box_left + box_w - 1), '+', EDIT_ATTR_MENU_BORDER);
    }

    /* items */
    for (i = 0; i < count; ++i) {
        const EDIT_MENU_ITEM *it = editor_menu_item(menu_sel, i);
        UINT8 r = (UINT8)(box_top + 1 + i);
        UINT8 attr = (i == item_sel) ? EDIT_ATTR_MENU_ITEM_SEL : EDIT_ATTR_MENU_ITEM;
        UINT8 inner_left = (UINT8)(box_left + 1);
        UINT8 inner_right = (UINT8)(box_left + box_w - 2);

        /* fill row background */
        editor_fill(r, inner_left, (UINT8)(box_w - 2), ' ', attr);

        /* selector marker */
        write_char(r, inner_left, (i == item_sel) ? '>' : ' ', attr);

        /* label */
        editor_write_text(r, (UINT8)(inner_left + 2), it->label, attr);

        /* shortcut aligned to the right */
        if (it->shortcut && it->shortcut[0] != '\0') {
            UINT8 slen = (UINT8)sima_strlen(it->shortcut);
            UINT8 sc = inner_left;
            if (slen <= (UINT8)(inner_right - inner_left)) {
                sc = (UINT8)(inner_right - slen + 1);
                if (sc < (UINT8)(inner_left + 2)) sc = (UINT8)(inner_left + 2);
            }
            editor_write_text(r, sc, it->shortcut, attr);
        }
    }
}

static void editor_popup_message(const char *l1, const char *l2, const char *l3, const char *l4) {
    /* centered popup; any NULL line is skipped */
    const char *lines[4];
    UINT8 i;
    UINT8 count = 0;
    UINT8 box_w = 60;
    UINT8 box_h;
    UINT8 top = 6;
    UINT8 left = 10;

    lines[0] = l1;
    lines[1] = l2;
    lines[2] = l3;
    lines[3] = l4;

    for (i = 0; i < 4; ++i) {
        if (lines[i] && lines[i][0] != '\0') ++count;
    }
    if (count == 0) return;

    box_h = (UINT8)(count + 2);

    for (i = 0; i < box_h; ++i) {
        editor_fill((UINT8)(top + i), left, box_w, ' ', EDIT_ATTR_MENU_ITEM);
    }

    /* border (ASCII) */
    for (i = 0; i < box_w; ++i) {
        write_char(top, (UINT8)(left + i), '-', EDIT_ATTR_MENU_BORDER);
        write_char((UINT8)(top + box_h - 1), (UINT8)(left + i), '-', EDIT_ATTR_MENU_BORDER);
    }
    write_char(top, left, '+', EDIT_ATTR_MENU_BORDER);
    write_char(top, (UINT8)(left + box_w - 1), '+', EDIT_ATTR_MENU_BORDER);
    write_char((UINT8)(top + box_h - 1), left, '+', EDIT_ATTR_MENU_BORDER);
    write_char((UINT8)(top + box_h - 1), (UINT8)(left + box_w - 1), '+', EDIT_ATTR_MENU_BORDER);

    /* text */
    {
        UINT8 row = (UINT8)(top + 1);
        for (i = 0; i < 4; ++i) {
            if (lines[i] && lines[i][0] != '\0') {
                editor_write_text(row, (UINT8)(left + 2), lines[i], EDIT_ATTR_MENU_BAR_TXT);
                ++row;
            }
        }
    }

    /* wait any key */
    read_key();
}


static void editor_draw_header(const char *name, BOOL modified, BOOL menu_open, UINT8 menu_sel) {
    /* DOS-like menu bar with visible hotkeys (Alt+F/E/H) */
    UINT16 i;
    char right[EDIT_COLS + 1];
    UINT16 len;
    UINT16 start;

    UINT8 attr_file = (menu_open && menu_sel == EDIT_MENU_FILE) ? EDIT_ATTR_MENU_BAR_SEL : EDIT_ATTR_MENU_BAR_TXT;
    UINT8 attr_edit = (menu_open && menu_sel == EDIT_MENU_EDIT) ? EDIT_ATTR_MENU_BAR_SEL : EDIT_ATTR_MENU_BAR_TXT;
    UINT8 attr_help = (menu_open && menu_sel == EDIT_MENU_HELP) ? EDIT_ATTR_MENU_BAR_SEL : EDIT_ATTR_MENU_BAR_TXT;

    /* fill header row */
    for (i = 0; i < EDIT_COLS; ++i) {
        write_char(EDIT_HEADER_ROW, (UINT8)i, ' ', EDIT_ATTR_MENU_BAR_BG);
    }

    /* menu labels (fixed columns) */
    editor_write_text(EDIT_HEADER_ROW, 1,  " File ", attr_file);
    editor_write_text(EDIT_HEADER_ROW, 8,  " Edit ", attr_edit);
    editor_write_text(EDIT_HEADER_ROW, 15, " Help ", attr_help);

    /* hotkey letters */
    write_char(EDIT_HEADER_ROW, 2,  'F', (menu_open && menu_sel == EDIT_MENU_FILE) ? attr_file : EDIT_ATTR_MENU_BAR_HOTKEY);
    write_char(EDIT_HEADER_ROW, 9,  'E', (menu_open && menu_sel == EDIT_MENU_EDIT) ? attr_edit : EDIT_ATTR_MENU_BAR_HOTKEY);
    write_char(EDIT_HEADER_ROW, 16, 'H', (menu_open && menu_sel == EDIT_MENU_HELP) ? attr_help : EDIT_ATTR_MENU_BAR_HOTKEY);

    /* right side: filename and modified marker */
    sima_memset(right, ' ', (UINT16)EDIT_COLS);
    right[EDIT_COLS] = '\0';
    sima_strcpy(right, (UINT16)sizeof(right), "EDIT: ");
    sima_strcat(right, (UINT16)sizeof(right), name);
    if (modified) sima_strcat(right, (UINT16)sizeof(right), " *");

    len = sima_strlen(right);
    start = 0;
    if (len < EDIT_COLS) start = (UINT16)(EDIT_COLS - len);

    for (i = 0; i < len && (start + i) < EDIT_COLS; ++i) {
        write_char(EDIT_HEADER_ROW, (UINT8)(start + i), right[i], EDIT_ATTR_MENU_BAR_TXT);
    }
}


static void editor_draw_status(const char *status, UINT16 doc_row, UINT16 doc_col) {
    /* status line with right-aligned cursor position */
    char pos[32];
    char num[8];
    UINT16 len;
    UINT16 start;

    /* fill status row background */
    editor_draw_row(EDIT_STATUS_ROW, NULL, EDIT_ATTR_STATUS_BG);

    /* left text */
    if (status && status[0] != '\0') {
        editor_write_text(EDIT_STATUS_ROW, 0, status, EDIT_ATTR_STATUS_TXT);
    }

    /* right text: 1-based row/col */
    pos[0] = '\0';
    sima_strcpy(pos, (UINT16)sizeof(pos), "Ln ");
    sima_utoa((UINT16)(doc_row + 1), num, (UINT16)sizeof(num), 10);
    sima_strcat(pos, (UINT16)sizeof(pos), num);
    sima_strcat(pos, (UINT16)sizeof(pos), " Col ");
    sima_utoa((UINT16)(doc_col + 1), num, (UINT16)sizeof(num), 10);
    sima_strcat(pos, (UINT16)sizeof(pos), num);

    len = sima_strlen(pos);
    start = 0;
    if (len < EDIT_COLS) start = (UINT16)(EDIT_COLS - len);

    editor_write_text(EDIT_STATUS_ROW, (UINT8)start, pos, EDIT_ATTR_STATUS_TXT);
}

static void editor_clear_content(void) {
    UINT8 row;
    for (row = 0; row < EDIT_ROWS; ++row) {
        editor_draw_row((UINT8)(EDIT_CONTENT_ROW + row), NULL, EDIT_ATTR_TEXT);
    }
}

static void editor_index_to_pos(const char *buffer, UINT16 size, UINT16 index, UINT16 *out_row, UINT16 *out_col) {
    UINT16 row = 0;
    UINT16 col = 0;
    UINT16 i;
    if (index > size) index = size;
    for (i = 0; i < index; ++i) {
        char ch = buffer[i];
        if (ch == '\n') {
            row++;
            col = 0;
        } else {
            col++;
            if (col >= EDIT_COLS) {
                row++;
                col = 0;
            }
        }
    }
    *out_row = row;
    *out_col = col;
}

static UINT16 editor_index_for_row_col(const char *buffer, UINT16 size, UINT16 target_row, UINT16 target_col) {
    UINT16 row = 0;
    UINT16 col = 0;
    UINT16 i = 0;

    while (i < size) {
        if (row == target_row && col >= target_col) return i;
        if (buffer[i] == '\n') {
            if (row == target_row) return i;
            row++;
            col = 0;
            i++;
            continue;
        }
        col++;
        i++;
        if (col >= EDIT_COLS) {
            if (row == target_row) return i;
            row++;
            col = 0;
        }
    }
    return size;
}

static UINT8 editor_fix_lr_scan(UINT8 scan) {
    /* Some environments report left/right swapped; normalize here for EDIT only. */
    if (scan == SCAN_LEFT) return SCAN_RIGHT;
    if (scan == SCAN_RIGHT) return SCAN_LEFT;
    return scan;
}

static UINT16 editor_row_length(const char *buffer, UINT16 size, UINT16 target_row) {
    /* length of a visual row (0..EDIT_COLS) considering '\n' and hard wrap */
    UINT16 row = 0;
    UINT16 col = 0;
    UINT16 i;

    for (i = 0; i < size; ++i) {
        char ch = buffer[i];
        if (row == target_row) {
            if (ch == '\n') return col;
            col++;
            if (col >= EDIT_COLS) return EDIT_COLS;
        } else {
            if (ch == '\n') {
                row++;
                col = 0;
            } else {
                col++;
                if (col >= EDIT_COLS) {
                    row++;
                    col = 0;
                }
            }
            if (row > target_row) return 0;
        }
    }

    if (row == target_row) return col;
    return 0;
}

static void editor_render(const char *name, const char *buffer, UINT16 size,
                          UINT16 cur_row, UINT16 cur_col,
                          UINT16 *scroll_row, BOOL modified, const char *status,
                          BOOL menu_open, UINT8 menu_sel, UINT8 menu_item_sel) {
    UINT16 row;
    UINT16 col;
    UINT16 i;

    if (cur_col >= EDIT_COLS) cur_col = (UINT16)(EDIT_COLS - 1);

    /* follow the virtual cursor, even past EOF */
    if (cur_row < *scroll_row) {
        *scroll_row = cur_row;
    } else if (cur_row >= (UINT16)(*scroll_row + EDIT_ROWS)) {
        *scroll_row = (UINT16)(cur_row - EDIT_ROWS + 1);
    }

    editor_draw_header(name, modified, menu_open, menu_sel);
    editor_draw_status(status, cur_row, cur_col);
    editor_clear_content();

    /* draw document text */
    row = 0;
    col = 0;
    for (i = 0; i < size; ++i) {
        char ch = buffer[i];
        if (ch == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (row >= *scroll_row && row < (UINT16)(*scroll_row + EDIT_ROWS)) {
            UINT8 screen_row = (UINT8)(EDIT_CONTENT_ROW + (row - *scroll_row));
            write_char(screen_row, (UINT8)col, ch, EDIT_ATTR_TEXT);
        }
        col++;
        if (col >= EDIT_COLS) {
            row++;
            col = 0;
        }
    }

    if (menu_open) {
        editor_draw_menu_overlay(menu_sel, menu_item_sel);
        /* keep the hardware cursor away from the dropdown to avoid color artifacts */
        set_cursor(EDIT_STATUS_ROW, (UINT8)(EDIT_COLS - 1));
        return;
    }

    /* place cursor at the virtual position */
    if (cur_row >= *scroll_row && cur_row < (UINT16)(*scroll_row + EDIT_ROWS)) {
        UINT8 screen_row = (UINT8)(EDIT_CONTENT_ROW + (cur_row - *scroll_row));
        set_cursor(screen_row, (UINT8)cur_col);
    } else {
        set_cursor(EDIT_CONTENT_ROW, 0);
    }
}


static BOOL editor_insert_char(char *buffer, UINT16 *size, UINT16 *cursor, char ch) {
    if (*size >= EDIT_MAX_SIZE) return FALSE;
    sima_memmove(buffer + *cursor + 1, buffer + *cursor, (UINT16)(*size - *cursor));
    buffer[*cursor] = ch;
    (*size)++;
    (*cursor)++;
    buffer[*size] = '\0';
    return TRUE;
}

static BOOL editor_ensure_virtual_pos(char *buffer, UINT16 *size, UINT16 vrow, UINT16 vcol, UINT16 *out_index) {
    /* Expand the buffer (adding newlines/spaces) so (vrow,vcol) becomes a valid insertion point. */
    UINT16 end_row;
    UINT16 end_col;
    UINT16 row_start;
    UINT16 i;
    UINT16 col;
    UINT16 row_end_index;
    UINT16 cursor;

    if (vcol >= EDIT_COLS) vcol = (UINT16)(EDIT_COLS - 1);

    editor_index_to_pos(buffer, *size, *size, &end_row, &end_col);

    /* ensure enough rows */
    while (end_row < vrow) {
        cursor = *size;
        if (!editor_insert_char(buffer, size, &cursor, '\n')) return FALSE;
        end_row++;
        end_col = 0;
    }

    /* ensure enough columns in the target visual row (pad spaces before newline/row end) */
    row_start = editor_index_for_row_col(buffer, *size, vrow, 0);

    i = row_start;
    col = 0;
    while (i < *size) {
        char ch = buffer[i];
        if (ch == '\n') break;
        col++;
        i++;
        if (col >= EDIT_COLS) break; /* hard wrap */
    }
    row_end_index = i;

    while (col < vcol) {
        cursor = row_end_index;
        if (!editor_insert_char(buffer, size, &cursor, ' ')) return FALSE;
        row_end_index = cursor; /* keep inserting at end (before newline) */
        col++;
    }

    /* insertion index: start + vcol */
    row_start = editor_index_for_row_col(buffer, *size, vrow, 0);
    *out_index = (UINT16)(row_start + vcol);
    if (*out_index > *size) *out_index = *size;
    return TRUE;
}


static BOOL editor_delete_before(char *buffer, UINT16 *size, UINT16 *cursor) {
    if (*cursor == 0 || *size == 0) return FALSE;
    sima_memmove(buffer + *cursor - 1, buffer + *cursor, (UINT16)(*size - *cursor));
    (*cursor)--;
    (*size)--;
    buffer[*size] = '\0';
    return TRUE;
}

static BOOL editor_delete_at(char *buffer, UINT16 *size, UINT16 *cursor) {
    if (*cursor >= *size) return FALSE;
    sima_memmove(buffer + *cursor, buffer + *cursor + 1, (UINT16)(*size - *cursor - 1));
    (*size)--;
    buffer[*size] = '\0';
    return TRUE;
}

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

static BOOL is_space_char(char ch) {
    return (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
}

/* Minimal Linux-like argv parser:
 * - Splits on spaces/tabs
 * - Supports single/double quotes to include spaces
 * - Treats backslash as an escape ONLY for whitespace and quotes (so paths like "U\\A" keep the '\\')
 * - Edits the input buffer in-place and writes tokens back into it (dst <= src).
 */
static UINT16 parse_argv(char *line, char **argv, UINT16 argv_cap) {
    UINT16 argc = 0;
    char *src;
    char *dst;

    BOOL in_single = FALSE;
    BOOL in_double = FALSE;

    if (!line || !argv || argv_cap == 0) return 0;
    src = line;
    dst = line;

    while (*src != '\0') {
        while (is_space_char(*src)) ++src;
        if (*src == '\0') break;
        if (argc >= argv_cap) break;

        argv[argc++] = dst;
        in_single = FALSE;
        in_double = FALSE;

        while (*src != '\0') {
            char ch = *src;
            if (!in_single && !in_double && is_space_char(ch)) {
                /* Consume the delimiter so dst==src in-place parsing keeps progressing. */
                ++src;
                break;
            }

            ++src;
            if (!in_double && ch == '\'') {
                in_single = (BOOL)!in_single;
                continue;
            }
            if (!in_single && ch == '"') {
                in_double = (BOOL)!in_double;
                continue;
            }

            if (!in_single && ch == '\\') {
                char next = *src;
                if (next == ' ' || next == '\t' || next == '\\' || next == '"' || next == '\'') {
                    if (next != '\0') {
                        ch = next;
                        ++src;
                    }
                }
            }

            *dst++ = ch;
        }

        *dst++ = '\0';
        while (is_space_char(*src)) ++src;
    }

    return argc;
}

/* Moved to sima_perm.c as perm_is_root() */

/* Permission helper (now uses sima_perm.c) */
static BOOL deny_if_forbidden(const char *path, BOOL allow_bin) {
    if (perm_allow_path(path, allow_bin)) return FALSE;
    print_simple("Permission denied.");
    return TRUE;
}

static BOOL has_path_separator(const char *path) {
    if (!path) return FALSE;
    while (*path) {
        if (*path == '/' || *path == '\\') return TRUE;
        ++path;
    }
    return FALSE;
}

static void print_simple(const char *text) {
    sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
    sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), text);
    print_message(message_buffer);
}

static BOOL run_diskinfo(void) {
    UINT32 phys_sectors = 0;
    UINT32 vol_sectors = 0;
    UINT32 vol_base_lba = 0;
    UINT8 vol_spc = 0;
    char line[128];

    print_message("Disk information:");

    if (ide_identify_total_sectors(&phys_sectors)) {
        UINT16 hi = (UINT16)(phys_sectors >> 16);
        UINT16 lo = (UINT16)(phys_sectors & 0xFFFF);
        char hi_s[8];
        char lo_s[8];

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Physical sectors: ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2); /* skip "0x" */
        print_message(line);
    } else {
        print_message("  Physical sectors: (ATA IDENTIFY failed)");
    }

    if (fs_get_volume_info32(&vol_sectors, &vol_spc, &vol_base_lba)) {
        UINT16 base_hi = (UINT16)(vol_base_lba >> 16);
        UINT16 base_lo = (UINT16)(vol_base_lba & 0xFFFF);
        UINT16 vol_hi = (UINT16)(vol_sectors >> 16);
        UINT16 vol_lo = (UINT16)(vol_sectors & 0xFFFF);
        UINT16 approx_mib = (UINT16)(vol_sectors >> 11); /* sectors/2048 */
        char hi_s[8];
        char lo_s[8];
        char n1[8];
        char n2[8];
        UINT16 cluster_bytes = (UINT16)((UINT16)vol_spc << 9);

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(base_hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(base_lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Partition base LBA: ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2);
        print_message(line);

        sima_memclr(hi_s, (UINT16)sizeof(hi_s));
        sima_memclr(lo_s, (UINT16)sizeof(lo_s));
        sima_itoh(vol_hi, hi_s, (UINT16)sizeof(hi_s));
        sima_itoh(vol_lo, lo_s, (UINT16)sizeof(lo_s));

        sima_memclr(n1, (UINT16)sizeof(n1));
        sima_memclr(n2, (UINT16)sizeof(n2));
        sima_utoa(approx_mib, n1, (UINT16)sizeof(n1), 10);
        sima_utoa(cluster_bytes, n2, (UINT16)sizeof(n2), 10);

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Volume sectors (BPB): ");
        sima_strcat(line, (UINT16)sizeof(line), hi_s);
        sima_strcat(line, (UINT16)sizeof(line), ":");
        sima_strcat(line, (UINT16)sizeof(line), lo_s + 2);
        sima_strcat(line, (UINT16)sizeof(line), " (~");
        sima_strcat(line, (UINT16)sizeof(line), n1);
        sima_strcat(line, (UINT16)sizeof(line), " MiB)");
        print_message(line);

        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  Sectors/cluster: ");
        sima_memclr(n1, (UINT16)sizeof(n1));
        sima_utoa(vol_spc, n1, (UINT16)sizeof(n1), 10);
        sima_strcat(line, (UINT16)sizeof(line), n1);
        sima_strcat(line, (UINT16)sizeof(line), " (");
        sima_strcat(line, (UINT16)sizeof(line), n2);
        sima_strcat(line, (UINT16)sizeof(line), " bytes)");
        print_message(line);
    } else {
        print_message("  Volume info (BPB): unavailable");
    }

    return TRUE;
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

static BOOL load_program_with_path(const char *name) {
    const char *path;
    char path_buffer[64];
    char *dirs[8];
    UINT16 count;
    UINT16 i;

    if (program_load(name)) return TRUE;
    path = env_get("PATH");
    if (!path) return FALSE;
    if (!sima_strcpy(path_buffer, (UINT16)sizeof(path_buffer), path)) {
        return FALSE;
    }
    count = sima_strcspl(path_buffer, ';', dirs, (UINT16)8);
    for (i = 0; i < count; ++i) {
        if (program_load_in_dir(dirs[i], name)) return TRUE;
    }
    return FALSE;
}

static UINT8 man_upper(UINT8 ch) {
    if (ch >= 'a' && ch <= 'z') return (UINT8)(ch - 'a' + 'A');
    return ch;
}

static BOOL man_topic_eq(const char *lhs, const char *rhs) {
    UINT16 i = 0;
    if (!lhs || !rhs) return FALSE;
    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (man_upper((UINT8)lhs[i]) != man_upper((UINT8)rhs[i])) return FALSE;
        ++i;
    }
    return (lhs[i] == '\0' && rhs[i] == '\0') ? TRUE : FALSE;
}

static BOOL man_wait_next(void) {
    for (;;) {
        UINT16 key = read_key();
        UINT8 ascii = (UINT8)(key & 0xFF);
        if (ascii == 'q' || ascii == 'Q' || ascii == 27) return FALSE;
        if (ascii == ' ' || ascii == 13) return TRUE;
    }
}

static BOOL man_show_page(const char *title, const char *const *lines, UINT16 line_count) {
    UINT16 index = 0;
    const UINT16 body_lines = 18;

    while (index < line_count) {
        UINT16 shown = 0;
        clear_screen();
        print_message("Seolsem Manual");
        print_message(title);
        print_message("");
        while (index < line_count && shown < body_lines) {
            print_message(lines[index]);
            ++index;
            ++shown;
        }
        if (index >= line_count) break;
        print_message("");
        print_message("[SPACE/ENTER] next  [Q/ESC] quit");
        if (!man_wait_next()) return TRUE;
    }
    return TRUE;
}

static BOOL run_man(const char *topic) {
    static const char *const man_index[] = {
        "Usage:",
        "  man <topic>",
        "  help <topic>",
        "",
        "Main topics:",
        "  system  fs  exec  user  display",
        "",
        "Command topics:",
        "  ls cd pwd cat write edit rm rmdir mkdir",
        "  load run exec sync diskinfo",
        "  whoami id users useradd login su passwd",
        "",
        "Examples:",
        "  man fs",
        "  man exec",
        "  man cd"
    };
    static const char *const man_system[] = {
        "system:",
        "  ver             show OS version",
        "  help [topic]    show manual index/page",
        "  man [topic]     show manual page",
        "  diskinfo        show disk and volume info",
        "",
        "Notes:",
        "  - command args support quotes",
        "  - path and permission checks are Linux-like"
    };
    static const char *const man_fs[] = {
        "fs:",
        "  ls [path]       list directory entries",
        "  cd [path]       change directory",
        "  cd -            jump to previous directory",
        "  pwd             print working directory",
        "  cat <file>      print file content",
        "  write <f> <txt> create/overwrite file",
        "  edit <file>     open text editor",
        "  rm <file>       delete file",
        "  rmdir <dir>     remove empty directory",
        "  mkdir <dir>     create directory",
        "  sync            flush cached FAT sectors",
        "",
        "Path tips:",
        "  - absolute: /BIN/HELLO.PRG",
        "  - relative: ./memo.txt  ../",
        "  - home: ~/notes.txt",
        "  - non-root users are limited by policy"
    };
    static const char *const man_exec[] = {
        "exec:",
        "  load <file>     load .PRG script into memory",
        "  run             run currently loaded program",
        "  exec <file>     load and run immediately",
        "",
        "Program format:",
        "  PRINT <text>",
        "  CLS",
        "  EXIT",
        "",
        "Search path:",
        "  - current directory first",
        "  - then PATH entries (default BIN)"
    };
    static const char *const man_user[] = {
        "user:",
        "  whoami          print current user",
        "  id              print current uid",
        "  users           list users",
        "  useradd <n> [p] add user (root only)",
        "  login <n> [p]   switch user",
        "  su <n> [p]      alias of login",
        "  passwd          change your password",
        "",
        "Default rules:",
        "  - root uid is 0",
        "  - user homes are under /HOME/<NAME>",
        "  - /ETC/PASSWD is source of truth"
    };
    static const char *const man_display[] = {
        "display:",
        "  cls             clear text screen",
        "",
        "Editor keys:",
        "  Ctrl+S          save",
        "  Esc             save and exit",
        "  Ctrl+Q          quit without saving",
        "  Arrow/Home/End/Delete supported"
    };
    static const char *const man_cd[] = {
        "cd:",
        "  cd              move to HOME",
        "  cd /            move to root",
        "  cd -            move to previous directory",
        "  cd <path>       move to path",
        "",
        "Examples:",
        "  cd /BIN",
        "  cd ..",
        "  cd ~/DOCS"
    };

    if (!topic || topic[0] == '\0') {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }

    if (man_topic_eq(topic, "HELP") || man_topic_eq(topic, "MAN")) {
        return man_show_page("Index", man_index, (UINT16)(sizeof(man_index) / sizeof(man_index[0])));
    }
    if (man_topic_eq(topic, "SYSTEM") || man_topic_eq(topic, "VER") || man_topic_eq(topic, "DISKINFO")) {
        return man_show_page("System", man_system, (UINT16)(sizeof(man_system) / sizeof(man_system[0])));
    }
    if (man_topic_eq(topic, "FS") || man_topic_eq(topic, "LS") || man_topic_eq(topic, "PWD") ||
        man_topic_eq(topic, "CAT") || man_topic_eq(topic, "WRITE") || man_topic_eq(topic, "EDIT") ||
        man_topic_eq(topic, "RM") || man_topic_eq(topic, "RMDIR") || man_topic_eq(topic, "MKDIR") ||
        man_topic_eq(topic, "SYNC")) {
        return man_show_page("Filesystem", man_fs, (UINT16)(sizeof(man_fs) / sizeof(man_fs[0])));
    }
    if (man_topic_eq(topic, "CD")) {
        return man_show_page("cd", man_cd, (UINT16)(sizeof(man_cd) / sizeof(man_cd[0])));
    }
    if (man_topic_eq(topic, "EXEC") || man_topic_eq(topic, "LOAD") || man_topic_eq(topic, "RUN")) {
        return man_show_page("Program Execution", man_exec, (UINT16)(sizeof(man_exec) / sizeof(man_exec[0])));
    }
    if (man_topic_eq(topic, "USER") || man_topic_eq(topic, "WHOAMI") || man_topic_eq(topic, "ID") ||
        man_topic_eq(topic, "USERS") || man_topic_eq(topic, "USERADD") ||
        man_topic_eq(topic, "LOGIN") || man_topic_eq(topic, "SU") ||
        man_topic_eq(topic, "PASSWD")) {
        return man_show_page("User", man_user, (UINT16)(sizeof(man_user) / sizeof(man_user[0])));
    }
    if (man_topic_eq(topic, "DISPLAY") || man_topic_eq(topic, "CLS")) {
        return man_show_page("Display", man_display, (UINT16)(sizeof(man_display) / sizeof(man_display[0])));
    }

    print_simple("No manual entry. Try: man");
    return TRUE;
}

void run_help() {
    (void)run_man((const char*)0);
}

static BOOL cd_expand_user(const char *path, char *out, UINT16 out_cap) {
    const char *home;
    const char *rest;

    if (!out || out_cap == 0) return FALSE;
    out[0] = '\0';

    if (!path) return FALSE;
    if (path[0] != '~') {
        return sima_strcpy(out, out_cap, path);
    }

    home = env_get("HOME");
    if (!home || home[0] == '\0') {
        /* No HOME set; keep the original path (e.g., "~" literal). */
        return sima_strcpy(out, out_cap, path);
    }

    rest = path + 1; /* skip '~' */
    /* Support "~" and "~/" only (no ~user). */
    if (rest[0] != '\0' && rest[0] != '/' && rest[0] != '\\') {
        return sima_strcpy(out, out_cap, path);
    }

    if (!sima_strcpy(out, out_cap, home)) return FALSE;
    if (rest[0] == '\0') return TRUE;

    /* Join carefully to avoid '//' when HOME is '/'. */
    if (out[0] == '/' && out[1] == '\0') {
        return sima_strcat(out, out_cap, rest + 1);
    }
    return sima_strcat(out, out_cap, rest);
}

static BOOL run_ls(const char *path) {
    FS_DIR dir;
    FS_DIRENT entry;
    char size_buffer[16];
    UINT16 guard = 0;

    if (deny_if_forbidden(path && path[0] ? path : ".", FALSE)) return TRUE;

    if (!fs_dir_open(path && path[0] ? path : ".", &dir)) {
        print_simple("Path not found.");
        return TRUE;
    }

    while (fs_dir_read(&dir, &entry)) {
        if (++guard > 256U) {
            print_simple("Directory listing aborted.");
            break;
        }
        sima_memclr(message_buffer, (UINT16)sizeof(message_buffer));
        sima_strcpy(message_buffer, (UINT16)sizeof(message_buffer), entry.name);
        if (entry.is_dir) {
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " [DIR]");
        } else {
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " ");
            sima_utoa((UINT16)entry.size, size_buffer, (UINT16)sizeof(size_buffer), 10);
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), size_buffer);
            sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " bytes");
            if (entry.is_exec) {
                sima_strcat(message_buffer, (UINT16)sizeof(message_buffer), " [EXEC]");
            }
        }
        print_message(message_buffer);
    }
    return TRUE;
}

static BOOL run_pwd(void) {
    char cwd[FS_PATH_MAX];
    sima_memclr(cwd, (UINT16)sizeof(cwd));
    if (!fs_get_cwd(cwd, (UINT16)sizeof(cwd))) {
        sima_strcpy(cwd, (UINT16)sizeof(cwd), "/");
    }
    print_message(cwd);
    return TRUE;
}

static BOOL run_cd(const char *path) {
    char old_path[FS_PATH_MAX];
    char target[FS_PATH_MAX];
    const char *home;
    char new_path[FS_PATH_MAX];
    static char prev_path[FS_PATH_MAX] = "";
    static BOOL has_prev = FALSE;
    BOOL swap_prev = FALSE;

    sima_memclr(old_path, (UINT16)sizeof(old_path));
    fs_get_cwd(old_path, (UINT16)sizeof(old_path));

    if (!path || path[0] == '\0') {
        home = env_get("HOME");
        path = (home && home[0] != '\0') ? home : "/";
    } else if (sima_strcmp(path, "-") == STRC_SAME) {
        if (!has_prev || prev_path[0] == '\0') {
            print_simple("No previous directory.");
            return TRUE;
        }
        path = prev_path;
        swap_prev = TRUE;
    }

    sima_memclr(target, (UINT16)sizeof(target));
    if (!cd_expand_user(path, target, (UINT16)sizeof(target))) {
        print_simple("Unable to change directory.");
        return TRUE;
    }
    if (deny_if_forbidden(target, FALSE)) return TRUE;

    if (!fs_cd(target)) {
        print_simple("Directory not found.");
        return TRUE;
    }

    /* Keep PWD/OLDPWD in sync for Linux-like behavior. */
    sima_memclr(new_path, (UINT16)sizeof(new_path));
    if (!fs_get_cwd(new_path, (UINT16)sizeof(new_path))) {
        sima_strcpy(new_path, (UINT16)sizeof(new_path), "/");
    }
    env_set_public("OLDPWD", old_path);
    env_set_public("PWD", new_path);

    /* Linux-like "cd -" swaps current and previous directories. */
    if (swap_prev) {
        sima_strcpy(prev_path, (UINT16)sizeof(prev_path), old_path);
        has_prev = TRUE;
        return run_pwd();
    }

    sima_strcpy(prev_path, (UINT16)sizeof(prev_path), old_path);
    has_prev = TRUE;
    return TRUE;
}

static BOOL run_cat(const char *name) {
    UINT8 data[FS_BLOCK_SIZE + 1];
    UINT32 size;

    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_read(name, data, FS_BLOCK_SIZE, &size)) {
        print_simple("Unable to read file.");
        return TRUE;
    }
    data[size] = '\0';
    print_message("");
    print_message((const char*)data);
    return TRUE;
}

static BOOL run_write(const char *name, const char *data) {
    UINT16 size;
    if (!name || !data) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    size = sima_strlen(data);
    if (!fs_write(name, (const UINT8*)data, (UINT32)size)) {
        print_simple("Unable to write file.");
        return TRUE;
    }
    print_simple("Write complete.");
    return TRUE;
}

static BOOL run_edit(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;

    {
        char buffer[EDIT_MAX_SIZE + 1];
        UINT16 size = 0;
        UINT16 cursor = 0;      /* real insertion index (buffer) */
        UINT16 scroll_row = 0;  /* top visual row */
        UINT16 vrow = 0;        /* virtual cursor row */
        UINT16 vcol = 0;        /* virtual cursor col */
        BOOL modified = FALSE;

        BOOL menu_open = FALSE;
        UINT8 menu_sel = EDIT_MENU_FILE;
        UINT8 menu_item_sel = 0;

        char status[EDIT_COLS + 1];
        char status_msg[EDIT_COLS + 1];
        UINT32 read_size = 0;

        sima_memclr(buffer, (UINT16)sizeof(buffer));
        sima_memclr(status_msg, (UINT16)sizeof(status_msg));

        if (fs_read(name, (UINT8*)buffer, EDIT_MAX_SIZE, &read_size)) {
            size = (UINT16)read_size;
            buffer[size] = '\0';
        }

        clear_screen();

        for (;;) {
            UINT16 key;
            UINT8 ascii;
            UINT8 scan;

            /* default status */
            if (menu_open) {
                sima_strcpy(status, (UINT16)sizeof(status), "Arrows Move  Enter Select  Esc Cancel  Alt/F10 Close");
            } else {
                sima_strcpy(status, (UINT16)sizeof(status), "F1 Help  Ctrl+S Save  Alt/F10 Menu  Esc Save&Exit  Ctrl+Q Quit");
            }

            if (status_msg[0] != '\0') {
                sima_strcpy(status, (UINT16)sizeof(status), status_msg);
            }

            editor_render(name, buffer, size, vrow, vcol, &scroll_row, modified, status,
                          menu_open, menu_sel, menu_item_sel);

            key = read_key();
            ascii = (UINT8)(key & 0xFF);
            scan  = (UINT8)((key >> 8) & 0xFF);

            /* normalize left/right in EDIT (some builds report swapped arrows) */
            if (ascii == 0 || ascii == 0xE0) {
                scan = editor_fix_lr_scan(scan);
            }

            /* clear transient status on any keypress (except when menu is open) */
            if (!menu_open && status_msg[0] != '\0') {
                status_msg[0] = '\0';
            }

            /* Global help */
            if ((ascii == 0 || ascii == 0xE0) && scan == SCAN_F1) {
                editor_popup_message(
                    "Keys:",
                    "  Arrows/Home/End/PgUp/PgDn  Move   Del/Backspace  Delete",
                    "  Ctrl+S Save   Esc Save&Exit   Ctrl+Q Quit   Alt/F10 Menu",
                    "  Menu: Arrows + Enter, Esc cancel"
                );
                continue;
            }

            /* Menu handling */
            if (menu_open) {
                UINT8 count = editor_menu_count(menu_sel);

                if (ascii == KEY_ESC) {
                    menu_open = FALSE;
                    continue;
                }

                if ((ascii == 0 || ascii == 0xE0) && scan == SCAN_F10) {
                    menu_open = FALSE;
                    continue;
                }

                if (ascii == 0 && scan == SCAN_ALT) {
                    menu_open = FALSE;
                    continue;
                }

                if (ascii == KEY_ENTER) {
                    const EDIT_MENU_ITEM *it = editor_menu_item(menu_sel, menu_item_sel);

                    if (it->action == EDIT_ACT_SAVE) {
                        if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                            sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Save failed.");
                        } else {
                            sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Saved.");
                            modified = FALSE;
                        }
                    } else if (it->action == EDIT_ACT_EXIT) {
                        if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                            sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Unable to save.");
                        } else {
                            clear_screen();
                            print_simple("Memo saved.");
                            break;
                        }
                    } else if (it->action == EDIT_ACT_QUIT) {
                        clear_screen();
                        print_simple("Edit cancelled.");
                        break;
                    } else if (it->action == EDIT_ACT_HELP_KEYS) {
                        editor_popup_message(
                            "EDIT Help:",
                            "  File->Save  (Ctrl+S)",
                            "  File->Exit  (Esc: Save&Exit)",
                            "  File->Quit  (Ctrl+Q)"
                        );
                    } else if (it->action == EDIT_ACT_HELP_ABOUT) {
                        editor_popup_message(
                            "EDIT (DOS-style UI)",
                            "  Menu-driven editor inspired by MS-DOS EDIT.",
                            "  Cursor supports free movement; buffer grows on edit.",
                            ""
                        );
                    } else {
                        sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Not implemented.");
                    }

                    menu_open = FALSE;
                    continue;
                }

                if (ascii == 0 || ascii == 0xE0) {
                    /* Menu bar left/right: clamp at ends (no wrap) */
                    if (scan == SCAN_LEFT) {
                        if (menu_sel > 0) menu_sel--;
                        menu_item_sel = 0;
                        continue;
                    }
                    if (scan == SCAN_RIGHT) {
                        if (menu_sel < 2) menu_sel++;
                        menu_item_sel = 0;
                        continue;
                    }

                    /* Dropdown up/down: clamp at ends (no wrap) */
                    if (scan == SCAN_UP) {
                        if (menu_item_sel > 0) menu_item_sel--;
                        continue;
                    }
                    if (scan == SCAN_DOWN) {
                        if (menu_item_sel + 1 < count) menu_item_sel++;
                        continue;
                    }
                    if (scan == SCAN_HOME) {
                        menu_item_sel = 0;
                        continue;
                    }
                    if (scan == SCAN_END) {
                        menu_item_sel = (UINT8)(count - 1);
                        continue;
                    }
                }

                /* Letter shortcuts while menu is open */
                if (ascii == 'f' || ascii == 'F') { menu_sel = EDIT_MENU_FILE; menu_item_sel = 0; continue; }
                if (ascii == 'e' || ascii == 'E') { menu_sel = EDIT_MENU_EDIT; menu_item_sel = 0; continue; }
                if (ascii == 'h' || ascii == 'H') { menu_sel = EDIT_MENU_HELP; menu_item_sel = 0; continue; }

                continue;
            }

            /* Open menu triggers (when not open) */
            if ((ascii == 0 || ascii == 0xE0) && scan == SCAN_F10) {
                menu_open = TRUE;
                menu_sel = EDIT_MENU_FILE;
                menu_item_sel = 0;
                continue;
            }

            if (ascii == 0 && scan == SCAN_ALT) {
                menu_open = TRUE;
                menu_sel = EDIT_MENU_FILE;
                menu_item_sel = 0;
                continue;
            }

            /* Save & exit */
            if (ascii == KEY_ESC) {
                if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                    print_simple("Unable to save memo.");
                    return TRUE;
                }
                clear_screen();
                print_simple("Memo saved.");
                break;
            }

            /* Quit without saving */
            if (ascii == KEY_CTRL_Q) {
                clear_screen();
                print_simple("Edit cancelled.");
                break;
            }

            /* Save */
            if (ascii == KEY_CTRL_S) {
                if (!fs_write(name, (const UINT8*)buffer, (UINT32)size)) {
                    sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Save failed.");
                } else {
                    sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Saved.");
                    modified = FALSE;
                }
                continue;
            }

            /* Editing */
            if (ascii == KEY_BACKSPACE) {
                UINT16 len = editor_row_length(buffer, size, vrow);
                if (vcol > len) {
                    if (vcol > 0) vcol--;
                    continue;
                }
                cursor = editor_index_for_row_col(buffer, size, vrow, vcol);
                if (editor_delete_before(buffer, &size, &cursor)) {
                    modified = TRUE;
                    editor_index_to_pos(buffer, size, cursor, &vrow, &vcol);
                }
                continue;
            }

            if (ascii == KEY_ENTER) {
                UINT16 ins;
                if (!editor_ensure_virtual_pos(buffer, &size, vrow, vcol, &ins)) {
                    sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Buffer full.");
                    continue;
                }
                cursor = ins;
                if (editor_insert_char(buffer, &size, &cursor, '\n')) {
                    modified = TRUE;
                    editor_index_to_pos(buffer, size, cursor, &vrow, &vcol);
                }
                continue;
            }

            if (ascii == 0 || ascii == 0xE0) {
                /* Navigation and special keys */
                if (scan == SCAN_LEFT) {
                    if (vcol > 0) vcol--;
                    continue;
                }
                if (scan == SCAN_RIGHT) {
                    if (vcol + 1 < EDIT_COLS) vcol++;
                    continue;
                }
                if (scan == SCAN_UP) {
                    if (vrow > 0) vrow--;
                    continue;
                }
                if (scan == SCAN_DOWN) {
                    if (vrow < 0xFFFE) vrow++;
                    continue;
                }
                if (scan == SCAN_HOME) {
                    vcol = 0;
                    continue;
                }
                if (scan == SCAN_END) {
                    UINT16 len = editor_row_length(buffer, size, vrow);
                    if (len >= EDIT_COLS) vcol = (UINT16)(EDIT_COLS - 1);
                    else vcol = len;
                    continue;
                }
                if (scan == SCAN_PGUP) {
                    if (vrow >= EDIT_ROWS) vrow = (UINT16)(vrow - EDIT_ROWS);
                    else vrow = 0;
                    continue;
                }
                if (scan == SCAN_PGDN) {
                    if (vrow <= (UINT16)(0xFFFE - EDIT_ROWS)) vrow = (UINT16)(vrow + EDIT_ROWS);
                    else vrow = 0xFFFE;
                    continue;
                }
                if (scan == SCAN_DEL) {
                    UINT16 len = editor_row_length(buffer, size, vrow);
                    if (vcol > len) continue;
                    cursor = editor_index_for_row_col(buffer, size, vrow, vcol);
                    if (editor_delete_at(buffer, &size, &cursor)) {
                        modified = TRUE;
                        editor_index_to_pos(buffer, size, cursor, &vrow, &vcol);
                    }
                    continue;
                }
            }

            if (ascii >= 32 && ascii != 127) {
                UINT16 ins;
                if (!editor_ensure_virtual_pos(buffer, &size, vrow, vcol, &ins)) {
                    sima_strcpy(status_msg, (UINT16)sizeof(status_msg), "Buffer full.");
                    continue;
                }
                cursor = ins;
                if (editor_insert_char(buffer, &size, &cursor, (char)ascii)) {
                    modified = TRUE;
                    editor_index_to_pos(buffer, size, cursor, &vrow, &vcol);
                }
                continue;
            }
        }
    }
    return TRUE;
}


static BOOL run_rm(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_delete(name)) {
        print_simple("Unable to delete file.");
        return TRUE;
    }
    print_simple("File deleted.");
    return TRUE;
}

static BOOL run_rmdir(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_delete_dir(name)) {
        print_simple("Unable to remove directory.");
        return TRUE;
    }
    print_simple("Directory removed.");
    return TRUE;
}

static BOOL run_mkdir(const char *name) {
    if (!name) return FALSE;
    if (deny_if_forbidden(name, FALSE)) return TRUE;
    if (!fs_create_dir(name)) {
        print_simple("Unable to create directory.");
        return TRUE;
    }
    print_simple("Directory created.");
    return TRUE;
}

static BOOL run_load(const char *name) {
    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;
    if (!perm_check(name, PERM_OP_READ | PERM_OP_EXEC)) {
        print_simple("Permission denied.");
        return TRUE;
    }
    if (!load_program_with_path(name)) {
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
    if (!name) return FALSE;
    if (!perm_is_root() && has_path_separator(name) && deny_if_forbidden(name, TRUE)) return TRUE;
    if (!perm_check(name, PERM_OP_READ | PERM_OP_EXEC)) {
        print_simple("Permission denied.");
        return TRUE;
    }
    if (!load_program_with_path(name)) {
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

static BOOL run_whoami(void) {
    const SIMA_USER *u = user_current();
    if (!u) {
        print_simple("ROOT");
        return TRUE;
    }
    print_message(u->name);
    return TRUE;
}

static BOOL run_users(void) {
    UINT16 i;
    const SIMA_USER *u;
    char uid_s[8];
    char line[128];

    print_message("Users:");
    for (i = 0; ; ++i) {
        u = user_at(i);
        if (!u) break;
        sima_memclr(uid_s, (UINT16)sizeof(uid_s));
        sima_utoa(u->uid, uid_s, (UINT16)sizeof(uid_s), 10);
        sima_memclr(line, (UINT16)sizeof(line));
        sima_strcpy(line, (UINT16)sizeof(line), "  ");
        sima_strcat(line, (UINT16)sizeof(line), u->name);
        sima_strcat(line, (UINT16)sizeof(line), " uid=");
        sima_strcat(line, (UINT16)sizeof(line), uid_s);
        sima_strcat(line, (UINT16)sizeof(line), " home=");
        sima_strcat(line, (UINT16)sizeof(line), u->home);
        print_message(line);
    }
    return TRUE;
}

static BOOL run_useradd(const char *name, const char *password) {
    if (!name || name[0] == '\0') return FALSE;
    if (!perm_is_root()) {
        print_simple("Permission denied. Only root can add users.");
        return TRUE;
    }
    if (!user_add(name, password)) {
        print_simple("Unable to add user.");
        return TRUE;
    }
    print_simple("User added.");
    return TRUE;
}

static BOOL run_login(const char *name, const char *password) {
    char pass_buf[USER_PASS_MAX];
    const char *pass = password;
    if (!name || name[0] == '\0') return FALSE;
    sima_memclr(pass_buf, (UINT16)sizeof(pass_buf));
    if (!pass || pass[0] == '\0') {
        wait_prompt("password: ", pass_buf);
        pass = pass_buf;
    }
    if (!user_login(name, pass)) {
        print_simple("Unable to switch user.");
        return TRUE;
    }
    sima_memclr(pass_buf, (UINT16)sizeof(pass_buf));
    print_simple("User switched.");
    return TRUE;
}

static BOOL run_passwd(void) {
    char old_buf[USER_PASS_MAX];
    char new_buf[USER_PASS_MAX];

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));

    wait_prompt("current password: ", old_buf);
    wait_prompt("new password: ", new_buf);

    if (new_buf[0] == '\0') {
        print_simple("New password cannot be empty.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        return TRUE;
    }

    if (!user_change_password(old_buf, new_buf)) {
        print_simple("Unable to update password.");
        sima_memclr(old_buf, (UINT16)sizeof(old_buf));
        sima_memclr(new_buf, (UINT16)sizeof(new_buf));
        return TRUE;
    }

    sima_memclr(old_buf, (UINT16)sizeof(old_buf));
    sima_memclr(new_buf, (UINT16)sizeof(new_buf));
    print_simple("Password updated.");
    return TRUE;
}

static BOOL run_id(void) {
    const SIMA_USER *u = user_current();
    char uid_s[8];
    char line[64];
    UINT16 uid = u ? u->uid : 0;

    sima_memclr(uid_s, (UINT16)sizeof(uid_s));
    sima_utoa(uid, uid_s, (UINT16)sizeof(uid_s), 10);
    sima_memclr(line, (UINT16)sizeof(line));
    sima_strcpy(line, (UINT16)sizeof(line), "uid=");
    sima_strcat(line, (UINT16)sizeof(line), uid_s);
    print_message(line);
    return TRUE;
}

BOOL run_buffer(char *buffer)
{
    UINT16 argc;
    char *argv[16];
	char temp[256];
    if (!buffer || buffer[0] == '\0') return FALSE;

	sima_strcpy(temp,sizeof(temp),buffer);
    argc = parse_argv(temp, argv, (UINT16)16);
    if (argc == 0 || !argv[0] || argv[0][0] == '\0') return FALSE;
	
    /* argv[0] 가 명령어 */
    if (sima_strcmp(argv[0], "ver") == STRC_SAME && argc==1) {
        run_ver();
        return TRUE;
    }
	else if(sima_strcmp(argv[0], "ver") == STRC_SAME) {
		return wrong_command_usage(buffer);
	}
    if (sima_strcmp(argv[0], "help") == STRC_SAME) {
        if (argc == 1) {
            run_help();
            return TRUE;
        }
        if (argc == 2) {
            return run_man(argv[1]);
        }
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "man") == STRC_SAME) {
        if (argc == 1) return run_man((const char*)0);
        if (argc == 2) return run_man(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "cls") == STRC_SAME && argc == 1) {
        return run_cls();
    }
    if (sima_strcmp(argv[0], "ls") == STRC_SAME) {
        if (argc == 1) return run_ls(NULL);
        if (argc == 2) return run_ls(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "cd") == STRC_SAME) {
        if (argc == 1) return run_cd(NULL);
        if (argc == 2) return run_cd(argv[1]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "pwd") == STRC_SAME && argc == 1) {
        return run_pwd();
    }
    if (sima_strcmp(argv[0], "cat") == STRC_SAME && argc == 2) {
        return run_cat(argv[1]);
    }
    if (sima_strcmp(argv[0], "write") == STRC_SAME && argc >= 3) {
        char data_buf[256];
        UINT16 i;
        sima_memclr(data_buf, (UINT16)sizeof(data_buf));
        for (i = 2; i < argc; ++i) {
            if (i != 2) {
                sima_strcat(data_buf, (UINT16)sizeof(data_buf), " ");
            }
            sima_strcat(data_buf, (UINT16)sizeof(data_buf), argv[i]);
        }
        return run_write(argv[1], data_buf);
    }
    if (sima_strcmp(argv[0], "edit") == STRC_SAME && argc == 2) {
        return run_edit(argv[1]);
    }
    if (sima_strcmp(argv[0], "rm") == STRC_SAME && argc == 2) {
        return run_rm(argv[1]);
    }
    if (sima_strcmp(argv[0], "rmdir") == STRC_SAME && argc == 2) {
        return run_rmdir(argv[1]);
    }
    if (sima_strcmp(argv[0], "mkdir") == STRC_SAME && argc == 2) {
        return run_mkdir(argv[1]);
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
    if (sima_strcmp(argv[0], "diskinfo") == STRC_SAME && argc == 1) {
        return run_diskinfo();
    }
    if (sima_strcmp(argv[0], "whoami") == STRC_SAME && argc == 1) {
        return run_whoami();
    }
    if (sima_strcmp(argv[0], "users") == STRC_SAME && argc == 1) {
        return run_users();
    }
    if (sima_strcmp(argv[0], "useradd") == STRC_SAME) {
        if (argc == 2) return run_useradd(argv[1], argv[1]);
        if (argc == 3) return run_useradd(argv[1], argv[2]);
        return wrong_command_usage(buffer);
    }
    if ((sima_strcmp(argv[0], "login") == STRC_SAME ||
         sima_strcmp(argv[0], "su") == STRC_SAME)) {
        if (argc == 2) return run_login(argv[1], NULL);
        if (argc == 3) return run_login(argv[1], argv[2]);
        return wrong_command_usage(buffer);
    }
    if (sima_strcmp(argv[0], "id") == STRC_SAME && argc == 1) {
        return run_id();
    }
    if (sima_strcmp(argv[0], "passwd") == STRC_SAME && argc == 1) {
        return run_passwd();
    }
    if (sima_strcmp(argv[0], "help") == STRC_SAME ||
        sima_strcmp(argv[0], "man") == STRC_SAME ||
        sima_strcmp(argv[0], "cls") == STRC_SAME ||
        sima_strcmp(argv[0], "ls") == STRC_SAME ||
        sima_strcmp(argv[0], "cd") == STRC_SAME ||
        sima_strcmp(argv[0], "pwd") == STRC_SAME ||
        sima_strcmp(argv[0], "cat") == STRC_SAME ||
        sima_strcmp(argv[0], "write") == STRC_SAME ||
        sima_strcmp(argv[0], "edit") == STRC_SAME ||
        sima_strcmp(argv[0], "rm") == STRC_SAME ||
        sima_strcmp(argv[0], "rmdir") == STRC_SAME ||
        sima_strcmp(argv[0], "mkdir") == STRC_SAME ||
        sima_strcmp(argv[0], "load") == STRC_SAME ||
        sima_strcmp(argv[0], "run") == STRC_SAME ||
        sima_strcmp(argv[0], "exec") == STRC_SAME ||
        sima_strcmp(argv[0], "sync") == STRC_SAME ||
        sima_strcmp(argv[0], "diskinfo") == STRC_SAME ||
        sima_strcmp(argv[0], "whoami") == STRC_SAME ||
        sima_strcmp(argv[0], "users") == STRC_SAME ||
        sima_strcmp(argv[0], "useradd") == STRC_SAME ||
        sima_strcmp(argv[0], "login") == STRC_SAME ||
        sima_strcmp(argv[0], "su") == STRC_SAME ||
        sima_strcmp(argv[0], "id") == STRC_SAME ||
        sima_strcmp(argv[0], "passwd") == STRC_SAME) {
        return wrong_command_usage(buffer);
    }
    return FALSE;
}

void run_ver() {
	print_message("Seolsem OS Version 1.0. All Rights Reserved.");
}
