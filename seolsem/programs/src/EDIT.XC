#include <TUI.XH>

#define ED_BUF_MAX 1536
#define ED_CMD_MAX 31

#define ED_VIEW_TOP 2
#define ED_VIEW_ROWS 20
#define ED_STATUS_ROW 22
#define ED_CMD_ROW 24

#define ED_MODE_NORMAL 0
#define ED_MODE_INSERT 1
#define ED_MODE_COMMAND 2

#define ED_STATUS_READY 0
#define ED_STATUS_DIRTY 1
#define ED_STATUS_SAVED 2
#define ED_STATUS_UNSAVED_BLOCK 3
#define ED_STATUS_WRITE_UNSUPPORTED 4
#define ED_STATUS_BAD_COMMAND 5
#define ED_STATUS_HELP 6
#define ED_STATUS_BUFFER_FULL 7
#define ED_STATUS_NO_FILENAME 8
#define ED_STATUS_WRITE_DENIED 9
#define ED_STATUS_WRITE_IS_DIR 10
#define ED_STATUS_WRITE_FAILED 11
#define ED_STATUS_WRITE_INVALID 12

struct EditorState {
  int running;
  int mode;
  int status;
  int dirty;
  int cursor;
  int buf_len;
  int top_line;
  int cmd_len;
};

#define ED_NAME_MAX 63

char g_cmd[32];
char g_buf[1537];
char g_name[64];

void ed_set_name(char *arg) {
  int i = 0;
  while (i <= ED_NAME_MAX) {
    g_name[i] = 0;
    i = i + 1;
  }
  if (!arg) return;
  i = 0;
  while (arg[i] != 0 && i < ED_NAME_MAX) {
    g_name[i] = arg[i];
    i = i + 1;
  }
  g_name[i] = 0;
}

void ed_clear_cmd(struct EditorState* st) {
  int i = 0;
  st->cmd_len = 0;
  while (i <= ED_CMD_MAX) {
    g_cmd[i] = 0;
    i = i + 1;
  }
}

void ed_init(struct EditorState* st) {
  int i = 0;
  st->running = 1;
  st->mode = ED_MODE_NORMAL;
  st->status = ED_STATUS_READY;
  st->dirty = 0;
  st->cursor = 0;
  st->buf_len = 0;
  st->top_line = 0;
  while (i <= ED_BUF_MAX) {
    g_buf[i] = 0;
    i = i + 1;
  }
  ed_clear_cmd(st);
}

int ed_min(int a, int b) {
  if (a < b) return a;
  return b;
}

int ed_max(int a, int b) {
  if (a > b) return a;
  return b;
}

int ed_line_start(struct EditorState* st, int idx) {
  int i = idx;
  if (i > st->buf_len) i = st->buf_len;
  while (i > 0) {
    if (g_buf[i - 1] == 10) break;
    i = i - 1;
  }
  return i;
}

int ed_line_end(struct EditorState* st, int idx) {
  int i = idx;
  if (i > st->buf_len) i = st->buf_len;
  while (i < st->buf_len) {
    if (g_buf[i] == 10) break;
    i = i + 1;
  }
  return i;
}

int ed_line_no_from_idx(struct EditorState* st, int idx) {
  int i = 0;
  int line = 0;
  if (idx > st->buf_len) idx = st->buf_len;
  while (i < idx) {
    if (g_buf[i] == 10) line = line + 1;
    i = i + 1;
  }
  return line;
}

int ed_idx_from_line_no(struct EditorState* st, int line_no) {
  int i = 0;
  int line = 0;
  if (line_no <= 0) return 0;
  while (i < st->buf_len) {
    if (line == line_no) return i;
    if (g_buf[i] == 10) line = line + 1;
    i = i + 1;
  }
  if (line == line_no) return st->buf_len;
  return -1;
}

void ed_ensure_visible(struct EditorState* st) {
  int cur_line = ed_line_no_from_idx(st, st->cursor);
  if (cur_line < st->top_line) st->top_line = cur_line;
  if (cur_line >= st->top_line + ED_VIEW_ROWS) {
    st->top_line = cur_line - ED_VIEW_ROWS + 1;
  }
  if (st->top_line < 0) st->top_line = 0;
}

void ed_insert_char(struct EditorState* st, int ch) {
  int i = 0;
  if (st->buf_len >= ED_BUF_MAX) {
    st->status = ED_STATUS_BUFFER_FULL;
    return;
  }
  i = st->buf_len;
  while (i > st->cursor) {
    g_buf[i] = g_buf[i - 1];
    i = i - 1;
  }
  g_buf[st->cursor] = ch;
  st->cursor = st->cursor + 1;
  st->buf_len = st->buf_len + 1;
  g_buf[st->buf_len] = 0;
  st->dirty = 1;
  st->status = ED_STATUS_DIRTY;
}

void ed_delete_at(struct EditorState* st, int idx) {
  int i = idx;
  if (idx < 0 || idx >= st->buf_len) return;
  while (i < st->buf_len - 1) {
    g_buf[i] = g_buf[i + 1];
    i = i + 1;
  }
  st->buf_len = st->buf_len - 1;
  g_buf[st->buf_len] = 0;
  if (st->cursor > st->buf_len) st->cursor = st->buf_len;
  st->dirty = 1;
  st->status = ED_STATUS_DIRTY;
}

void ed_move_left(struct EditorState* st) {
  if (st->cursor > 0) st->cursor = st->cursor - 1;
}

void ed_move_right(struct EditorState* st) {
  if (st->cursor < st->buf_len) st->cursor = st->cursor + 1;
}

void ed_move_up(struct EditorState* st) {
  int start = ed_line_start(st, st->cursor);
  int col = st->cursor - start;
  int prev_end = 0;
  int prev_start = 0;
  int prev_len = 0;
  if (start == 0) return;
  prev_end = start - 1;
  prev_start = ed_line_start(st, prev_end);
  prev_len = prev_end - prev_start;
  st->cursor = prev_start + ed_min(col, prev_len);
}

void ed_move_down(struct EditorState* st) {
  int start = ed_line_start(st, st->cursor);
  int col = st->cursor - start;
  int end = ed_line_end(st, st->cursor);
  int next_start = 0;
  int next_end = 0;
  int next_len = 0;
  if (end >= st->buf_len) return;
  next_start = end + 1;
  next_end = ed_line_end(st, next_start);
  next_len = next_end - next_start;
  st->cursor = next_start + ed_min(col, next_len);
}

int ed_cmd_eq(struct EditorState* st, char *text) {
  int i = 0;
  while (1) {
    if (g_cmd[i] != text[i]) return 0;
    if (g_cmd[i] == 0) return 1;
    i = i + 1;
  }
}

int ed_cmd_match(char *cmd, int *out_arg_idx) {
  int i = 0;
  if (out_arg_idx) {
    *out_arg_idx = -1;
  }
  while (cmd[i] != 0) {
    if (g_cmd[i] != cmd[i]) return 0;
    i = i + 1;
  }
  if (g_cmd[i] == 0) return 1;
  if (g_cmd[i] != 32) return 0;
  while (g_cmd[i] == 32) {
    i = i + 1;
  }
  if (g_cmd[i] == 0) return 1;
  if (out_arg_idx) {
    *out_arg_idx = i;
  }
  return 1;
}

void ed_set_name_from_cmd(int start) {
  int i = 0;
  int j = 0;
  while (i <= ED_NAME_MAX) {
    g_name[i] = 0;
    i = i + 1;
  }
  if (start < 0) return;
  while (g_cmd[start] == 32) {
    start = start + 1;
  }
  while (g_cmd[start] != 0 && g_cmd[start] != 32 && j < ED_NAME_MAX) {
    g_name[j] = g_cmd[start];
    j = j + 1;
    start = start + 1;
  }
  g_name[j] = 0;
}

int ed_try_write(struct EditorState* st) {
  int rc = 0;
  if (g_name[0] == 0) {
    st->status = ED_STATUS_NO_FILENAME;
    return 0;
  }
  if (st->buf_len < 0 || st->buf_len > ED_BUF_MAX) {
    st->status = ED_STATUS_WRITE_INVALID;
    return 0;
  }
  rc = fs_write(g_name, g_buf, st->buf_len);
  if (rc == 0) {
    st->dirty = 0;
    st->status = ED_STATUS_SAVED;
    return 1;
  }
  if (rc == 1) st->status = ED_STATUS_WRITE_DENIED;
  else if (rc == 2) st->status = ED_STATUS_WRITE_IS_DIR;
  else if (rc == 3) st->status = ED_STATUS_WRITE_INVALID;
  else st->status = ED_STATUS_WRITE_FAILED;
  return 0;
}

void ed_load_file(struct EditorState* st) {
  int rc = 0;
  int size = 0;
  int src = 0;
  int dst = 0;

  if (!st) return;
  if (g_name[0] == 0) return;

  rc = fs_read(g_name, g_buf, ED_BUF_MAX, &size);
  if (rc == 1) {
    st->status = ED_STATUS_READY;
    return;
  }
  if (rc != 0) {
    if (rc == 2) st->status = ED_STATUS_WRITE_DENIED;
    else if (rc == 3) st->status = ED_STATUS_WRITE_IS_DIR;
    else if (rc == 5) st->status = ED_STATUS_BUFFER_FULL;
    else st->status = ED_STATUS_WRITE_FAILED;
    return;
  }

  if (size < 0) size = 0;
  if (size > ED_BUF_MAX) size = ED_BUF_MAX;

  /* Normalize CRLF to LF. */
  while (src < size) {
    if (g_buf[src] == 13) {
      src = src + 1;
      continue;
    }
    g_buf[dst] = g_buf[src];
    src = src + 1;
    dst = dst + 1;
  }
  size = dst;
  g_buf[size] = 0;

  st->buf_len = size;
  if (st->cursor > st->buf_len) st->cursor = st->buf_len;
  st->dirty = 0;
  st->status = ED_STATUS_READY;
}

void ed_exec_command(struct EditorState* st) {
  int arg_idx = -1;
  if (ed_cmd_eq(st, "q")) {
    if (st->dirty) st->status = ED_STATUS_UNSAVED_BLOCK;
    else st->running = 0;
    return;
  }
  if (ed_cmd_eq(st, "q!")) {
    st->running = 0;
    return;
  }
  if (ed_cmd_match("w", &arg_idx)) {
    if (arg_idx >= 0) {
      ed_set_name_from_cmd(arg_idx);
    }
    ed_try_write(st);
    return;
  }
  if (ed_cmd_match("wq", &arg_idx)) {
    int has_arg = 0;
    if (arg_idx >= 0) {
      has_arg = 1;
      ed_set_name_from_cmd(arg_idx);
    }
    if (g_name[0] == 0) {
      st->status = ED_STATUS_NO_FILENAME;
      return;
    }
    if (!st->dirty && !has_arg) {
      st->running = 0;
      return;
    }
    if (ed_try_write(st)) st->running = 0;
    return;
  }
  if (ed_cmd_eq(st, "help")) {
    st->status = ED_STATUS_HELP;
    return;
  }
  st->status = ED_STATUS_BAD_COMMAND;
}

void ed_draw_line_from(int row, struct EditorState* st, int start_idx) {
  int i = 0;
  int col = 0;
  tui_fill_line(row, 32, TUI_ATTR_NORMAL);
  while (start_idx + i < st->buf_len && col < TUI_COLS) {
    int ch = g_buf[start_idx + i];
    if (ch == 10) break;
    if (ch == 9) ch = 32;
    tui_putc(row, col, ch, TUI_ATTR_NORMAL);
    i = i + 1;
    col = col + 1;
  }
}

void ed_draw(struct EditorState* st) {
  int view = 0;
  int line_no = st->top_line;
  int cur_line = 0;
  int cur_col = 0;
  int cur_row = 0;
  int idx = 0;
  tui_clear();

  tui_fill_line(0, 32, TUI_ATTR_STATUS);
  if (st->mode == ED_MODE_NORMAL) tui_draw_text(0, 0, "EDIT  NORMAL", TUI_ATTR_STATUS);
  if (st->mode == ED_MODE_INSERT) tui_draw_text(0, 0, "EDIT  INSERT", TUI_ATTR_STATUS);
  if (st->mode == ED_MODE_COMMAND) tui_draw_text(0, 0, "EDIT  COMMAND", TUI_ATTR_STATUS);
  if (g_name[0] != 0) tui_draw_text(0, 14, g_name, TUI_ATTR_STATUS);

  tui_fill_line(1, 32, TUI_ATTR_NORMAL);
  tui_draw_text(1, 0, "h/j/k/l move  i insert  a append  x del  : cmd  esc normal", TUI_ATTR_NORMAL);

  while (view < ED_VIEW_ROWS) {
    idx = ed_idx_from_line_no(st, line_no);
    if (idx < 0) tui_fill_line(ED_VIEW_TOP + view, 32, TUI_ATTR_NORMAL);
    else ed_draw_line_from(ED_VIEW_TOP + view, st, idx);
    line_no = line_no + 1;
    view = view + 1;
  }

  tui_fill_line(ED_STATUS_ROW, 32, TUI_ATTR_STATUS);
  if (st->status == ED_STATUS_READY) tui_draw_text(ED_STATUS_ROW, 0, "READY", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_DIRTY) tui_draw_text(ED_STATUS_ROW, 0, "MODIFIED", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_SAVED) tui_draw_text(ED_STATUS_ROW, 0, "SAVED", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_UNSAVED_BLOCK) tui_draw_text(ED_STATUS_ROW, 0, "UNSAVED! use :q!", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_NO_FILENAME) tui_draw_text(ED_STATUS_ROW, 0, "NO FILE NAME", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_WRITE_DENIED) tui_draw_text(ED_STATUS_ROW, 0, "PERMISSION DENIED", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_WRITE_IS_DIR) tui_draw_text(ED_STATUS_ROW, 0, "IS A DIRECTORY", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_WRITE_FAILED) tui_draw_text(ED_STATUS_ROW, 0, "WRITE FAILED", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_WRITE_INVALID) tui_draw_text(ED_STATUS_ROW, 0, "INVALID PATH", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_BAD_COMMAND) tui_draw_text(ED_STATUS_ROW, 0, "BAD COMMAND", TUI_ATTR_STATUS);
  else if (st->status == ED_STATUS_HELP) tui_draw_text(ED_STATUS_ROW, 0, "N:h/j/k/l I:type :w :q", TUI_ATTR_STATUS);
  else tui_draw_text(ED_STATUS_ROW, 0, "BUFFER FULL", TUI_ATTR_STATUS);

  tui_fill_line(ED_CMD_ROW, 32, TUI_ATTR_NORMAL);
  if (st->mode == ED_MODE_COMMAND) {
    tui_putc(ED_CMD_ROW, 0, 58, TUI_ATTR_NORMAL);
    tui_draw_text(ED_CMD_ROW, 1, g_cmd, TUI_ATTR_NORMAL);
  } else {
    tui_draw_text(ED_CMD_ROW, 0, "--", TUI_ATTR_NORMAL);
  }

  if (st->mode == ED_MODE_COMMAND) {
    tui_cursor_on();
    tui_cursor_at(ED_CMD_ROW, 1 + st->cmd_len);
    return;
  }

  cur_line = ed_line_no_from_idx(st, st->cursor);
  cur_col = st->cursor - ed_line_start(st, st->cursor);
  cur_row = ED_VIEW_TOP + (cur_line - st->top_line);
  cur_row = ed_max(ED_VIEW_TOP, ed_min(ED_VIEW_TOP + ED_VIEW_ROWS - 1, cur_row));
  cur_col = ed_max(0, ed_min(TUI_COLS - 1, cur_col));
  tui_cursor_on();
  tui_cursor_at(cur_row, cur_col);
}

void ed_handle_normal(struct EditorState* st, int key) {
  if (key == KEY_EXT_LEFT || key == KEY_H) { ed_move_left(st); return; }
  if (key == KEY_EXT_RIGHT || key == KEY_L) { ed_move_right(st); return; }
  if (key == KEY_EXT_UP || key == KEY_K) { ed_move_up(st); return; }
  if (key == KEY_EXT_DOWN || key == KEY_J) { ed_move_down(st); return; }
  if (key == KEY_I) { st->mode = ED_MODE_INSERT; return; }
  if (key == KEY_E) { st->mode = ED_MODE_INSERT; return; }
  if (key == KEY_A) {
    if (st->cursor < st->buf_len) st->cursor = st->cursor + 1;
    st->mode = ED_MODE_INSERT;
    return;
  }
  if (key == KEY_X) {
    ed_delete_at(st, st->cursor);
    return;
  }
  if (key == KEY_0) {
    st->cursor = ed_line_start(st, st->cursor);
    return;
  }
  if (key == 36) {
    st->cursor = ed_line_end(st, st->cursor);
    return;
  }
  if (key == 58 || key == KEY_SEMI) {
    st->mode = ED_MODE_COMMAND;
    ed_clear_cmd(st);
    return;
  }
  if (key == KEY_Q) {
    if (st->dirty) st->status = ED_STATUS_UNSAVED_BLOCK;
    else st->running = 0;
    return;
  }
}

void ed_handle_insert(struct EditorState* st, int raw_key, int key) {
  int d = -1;
  if (key == KEY_ESC) {
    st->mode = ED_MODE_NORMAL;
    return;
  }
  if (key == KEY_EXT_LEFT) { ed_move_left(st); return; }
  if (key == KEY_EXT_RIGHT) { ed_move_right(st); return; }
  if (key == KEY_EXT_UP) { ed_move_up(st); return; }
  if (key == KEY_EXT_DOWN) { ed_move_down(st); return; }
  if (key == KEY_BACKSPACE) {
    if (st->cursor > 0) {
      st->cursor = st->cursor - 1;
      ed_delete_at(st, st->cursor);
    }
    return;
  }
  if (key == KEY_ENTER) {
    ed_insert_char(st, 10);
    return;
  }
  d = tui_digit_from_key(raw_key);
  if (d >= 0) {
    ed_insert_char(st, d);
    return;
  }
  if (key >= 32 && key <= 126) {
    ed_insert_char(st, key);
    return;
  }
}

void ed_handle_command(struct EditorState* st, int key) {
  if (key == KEY_ESC) {
    st->mode = ED_MODE_NORMAL;
    ed_clear_cmd(st);
    return;
  }
  if (key == KEY_ENTER) {
    ed_exec_command(st);
    st->mode = ED_MODE_NORMAL;
    ed_clear_cmd(st);
    return;
  }
  if (key == KEY_BACKSPACE) {
    if (st->cmd_len > 0) {
      st->cmd_len = st->cmd_len - 1;
      g_cmd[st->cmd_len] = 0;
    }
    return;
  }
  if (key >= 32 && key <= 126) {
    if (st->cmd_len < ED_CMD_MAX) {
      g_cmd[st->cmd_len] = key;
      st->cmd_len = st->cmd_len + 1;
      g_cmd[st->cmd_len] = 0;
    }
    return;
  }
}

void ed_handle_key(struct EditorState* st, int raw_key) {
  int key = tui_key_decode(raw_key);
  if (key == 0) key = raw_key & 255;

  if (st->mode == ED_MODE_NORMAL) ed_handle_normal(st, key);
  else if (st->mode == ED_MODE_INSERT) ed_handle_insert(st, raw_key, key);
  else ed_handle_command(st, key);

  if (st->cursor < 0) st->cursor = 0;
  if (st->cursor > st->buf_len) st->cursor = st->buf_len;
  ed_ensure_visible(st);
}

int main(char *arg) {
  struct EditorState st;
  int raw = 0;

  ed_set_name(arg);
  ed_init(&st);
  ed_load_file(&st);
  while (st.running) {
    ed_draw(&st);
    raw = read_key();
    ed_handle_key(&st, raw);
  }

  tui_cursor_on();
  tui_clear();
  print("EDIT end");
  return 0;
}
