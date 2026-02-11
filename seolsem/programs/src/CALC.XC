#include <TUI.XH>

#define CALC_FOCUS_A 0
#define CALC_FOCUS_OP 1
#define CALC_FOCUS_B 2

#define CALC_OP_NONE 0
#define CALC_OP_ADD 1
#define CALC_OP_SUB 2
#define CALC_OP_MUL 3
#define CALC_OP_DIV 4
#define CALC_OP_MOD 5
#define CALC_OP_AND 6
#define CALC_OP_OR 7
#define CALC_OP_XOR 8
#define CALC_OP_SHL 9
#define CALC_OP_SHR 10

#define CALC_STATUS_READY 0
#define CALC_STATUS_EDITING 1
#define CALC_STATUS_OK 2
#define CALC_STATUS_CLEARED 3
#define CALC_STATUS_OVERFLOW 4
#define CALC_STATUS_MISSING_A 5
#define CALC_STATUS_MISSING_B 6
#define CALC_STATUS_MISSING_OP 7
#define CALC_STATUS_DIV_ZERO 8
#define CALC_STATUS_MOD_ZERO 9
#define CALC_STATUS_SHIFT_RANGE 10
#define CALC_STATUS_UNKNOWN_KEY 11

struct CalcState {
  int running;
  int focus;
  int input_active;
  int op;
  int a;
  int a_digits;
  int b;
  int b_digits;
  int has_result;
  int result;
  int status;
};

int calc_u16_xform(int x) {
  return x ^ 32768;
}

int calc_u16_ge(int a, int b) {
  if (calc_u16_xform(a) < calc_u16_xform(b)) return 0;
  return 1;
}

int calc_count_digits(int value) {
  if (calc_u16_ge(value, 10000)) return 5;
  if (calc_u16_ge(value, 1000)) return 4;
  if (calc_u16_ge(value, 100)) return 3;
  if (calc_u16_ge(value, 10)) return 2;
  return 1;
}

void calc_reset(struct CalcState* st) {
  st->running = 1;
  st->focus = CALC_FOCUS_A;
  st->input_active = 0;
  st->op = CALC_OP_NONE;
  st->a = 0;
  st->a_digits = 0;
  st->b = 0;
  st->b_digits = 0;
  st->has_result = 0;
  st->result = 0;
  st->status = CALC_STATUS_READY;
}

int calc_has_a(struct CalcState* st) {
  if (st->a_digits > 0) return 1;
  return 0;
}

int calc_has_b(struct CalcState* st) {
  if (st->b_digits > 0) return 1;
  return 0;
}

int calc_op_from_key(int ch) {
  if (ch == KEY_A) return CALC_OP_ADD;
  if (ch == KEY_S) return CALC_OP_SUB;
  if (ch == KEY_D) return CALC_OP_MUL;
  if (ch == KEY_F) return CALC_OP_DIV;
  if (ch == KEY_G) return CALC_OP_MOD;
  if (ch == KEY_H) return CALC_OP_AND;
  if (ch == KEY_J) return CALC_OP_OR;
  if (ch == KEY_K) return CALC_OP_XOR;
  if (ch == KEY_U) return CALC_OP_SHL;
  if (ch == KEY_I) return CALC_OP_SHR;
  return CALC_OP_NONE;
}

int calc_to_lower(int ch) {
  if (ch >= 65 && ch <= 90) return ch + 32;
  return ch;
}

int calc_decode_key(int raw_key) {
  int key = tui_key_decode(raw_key);
  if (key != 0) return key;
  return raw_key & 255;
}

int calc_append_digit(int value, int digit, int* out_value) {
  int next = 0;
  if (calc_u16_ge(value, 6554)) return 0;
  if (value == 6553 && digit > 5) return 0;
  next = value * 10 + digit;
  *out_value = next;
  return 1;
}

int calc_u16_div10(int value) {
  int rem = value;
  int q = 0;
  while (calc_u16_ge(rem, 10000)) {
    rem = rem - 10000;
    q = q + 1000;
  }
  while (calc_u16_ge(rem, 1000)) {
    rem = rem - 1000;
    q = q + 100;
  }
  while (calc_u16_ge(rem, 100)) {
    rem = rem - 100;
    q = q + 10;
  }
  while (calc_u16_ge(rem, 10)) {
    rem = rem - 10;
    q = q + 1;
  }
  return q;
}

void calc_clear_focus(struct CalcState* st) {
  if (st->focus == CALC_FOCUS_A) {
    st->a = 0;
    st->a_digits = 0;
    return;
  }
  if (st->focus == CALC_FOCUS_OP) {
    st->op = CALC_OP_NONE;
    return;
  }
  st->b = 0;
  st->b_digits = 0;
}

void calc_move_next_focus(struct CalcState* st) {
  if (st->focus == CALC_FOCUS_A) {
    st->focus = CALC_FOCUS_OP;
    return;
  }
  if (st->focus == CALC_FOCUS_OP) {
    st->focus = CALC_FOCUS_B;
    return;
  }
  st->focus = CALC_FOCUS_A;
}

void calc_move_prev_focus(struct CalcState* st) {
  if (st->focus == CALC_FOCUS_A) {
    st->focus = CALC_FOCUS_B;
    return;
  }
  if (st->focus == CALC_FOCUS_B) {
    st->focus = CALC_FOCUS_OP;
    return;
  }
  st->focus = CALC_FOCUS_A;
}

int calc_push_digit(struct CalcState* st, int ch) {
  int digit = digit_from_char(ch);
  int next = 0;

  if (st->focus == CALC_FOCUS_OP) st->focus = CALC_FOCUS_B;

  if (st->focus == CALC_FOCUS_A) {
    if (st->a_digits >= 5) return 0;
    if (!calc_append_digit(st->a, digit, &next)) return 0;
    st->a = next;
    st->a_digits = st->a_digits + 1;
    return 1;
  }

  if (st->b_digits >= 5) return 0;
  if (!calc_append_digit(st->b, digit, &next)) return 0;
  st->b = next;
  st->b_digits = st->b_digits + 1;
  return 1;
}

void calc_backspace(struct CalcState* st) {
  if (st->focus == CALC_FOCUS_A) {
    if (st->a_digits == 0) return;
    st->a = calc_u16_div10(st->a);
    st->a_digits = st->a_digits - 1;
    return;
  }
  if (st->focus == CALC_FOCUS_OP) {
    st->op = CALC_OP_NONE;
    return;
  }
  if (st->b_digits == 0) return;
  st->b = calc_u16_div10(st->b);
  st->b_digits = st->b_digits - 1;
}

int calc_eval_binary(int a, int b, int op, int* out_result) {
  if (op == CALC_OP_ADD) {
    *out_result = (a + b) & 65535;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_SUB) {
    *out_result = (a - b) & 65535;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_MUL) {
    *out_result = (a * b) & 65535;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_DIV) {
    if (b == 0) return CALC_STATUS_DIV_ZERO;
    *out_result = a / b;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_MOD) {
    if (b == 0) return CALC_STATUS_MOD_ZERO;
    *out_result = a % b;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_AND) {
    *out_result = a & b;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_OR) {
    *out_result = a | b;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_XOR) {
    *out_result = a ^ b;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_SHL) {
    if (b < 0 || b > 15) return CALC_STATUS_SHIFT_RANGE;
    *out_result = (a << b) & 65535;
    return CALC_STATUS_OK;
  }
  if (op == CALC_OP_SHR) {
    if (b < 0 || b > 15) return CALC_STATUS_SHIFT_RANGE;
    *out_result = (a >> b) & 65535;
    return CALC_STATUS_OK;
  }
  return CALC_STATUS_MISSING_OP;
}

void calc_compute(struct CalcState* st) {
  int out = 0;
  int status = CALC_STATUS_OK;

  if (!calc_has_a(st)) {
    st->status = CALC_STATUS_MISSING_A;
    return;
  }
  if (st->op == CALC_OP_NONE) {
    st->status = CALC_STATUS_MISSING_OP;
    return;
  }
  if (!calc_has_b(st)) {
    st->status = CALC_STATUS_MISSING_B;
    return;
  }

  status = calc_eval_binary(st->a, st->b, st->op, &out);
  st->status = status;
  if (status != CALC_STATUS_OK) return;

  st->result = out;
  st->has_result = 1;

  st->a = out;
  st->a_digits = calc_count_digits(out);
  st->b = 0;
  st->b_digits = 0;
  st->op = CALC_OP_NONE;
  st->focus = CALC_FOCUS_OP;
}

#define CALC_ROW_TITLE 0
#define CALC_ROW_HINT 1
#define CALC_ROW_KEYS 2
#define CALC_ROW_A 5
#define CALC_ROW_OP 7
#define CALC_ROW_B 9
#define CALC_ROW_RESULT 11
#define CALC_ROW_STATUS 24
#define CALC_COL_LABEL 2
#define CALC_COL_VALUE 8

int calc_text_len(char *text) {
  int i = 0;
  while (text[i] != 0) i = i + 1;
  return i;
}

void calc_u16_to_dec(int value, char *out, int out_cap) {
  int v = value;
  int pos = 0;
  int started = 0;
  int d = 0;

  if (out_cap <= 0) return;
  out[0] = 0;

  d = 0;
  while (calc_u16_ge(v, 10000)) { v = v - 10000; d = d + 1; }
  if (d > 0) { out[pos] = 48 + d; pos = pos + 1; started = 1; }

  d = 0;
  while (calc_u16_ge(v, 1000)) { v = v - 1000; d = d + 1; }
  if (started || d > 0) { out[pos] = 48 + d; pos = pos + 1; started = 1; }

  d = 0;
  while (calc_u16_ge(v, 100)) { v = v - 100; d = d + 1; }
  if (started || d > 0) { out[pos] = 48 + d; pos = pos + 1; started = 1; }

  d = 0;
  while (calc_u16_ge(v, 10)) { v = v - 10; d = d + 1; }
  if (started || d > 0) { out[pos] = 48 + d; pos = pos + 1; }

  out[pos] = 48 + v;
  pos = pos + 1;
  out[pos] = 0;
}

char *calc_op_text(int op) {
  if (op == CALC_OP_NONE) return "(none)";
  if (op == CALC_OP_ADD) return "+";
  if (op == CALC_OP_SUB) return "-";
  if (op == CALC_OP_MUL) return "*";
  if (op == CALC_OP_DIV) return "/";
  if (op == CALC_OP_MOD) return "%";
  if (op == CALC_OP_AND) return "&";
  if (op == CALC_OP_OR) return "|";
  if (op == CALC_OP_XOR) return "^";
  if (op == CALC_OP_SHL) return "<<";
  if (op == CALC_OP_SHR) return ">>";
  return "?";
}

char *calc_status_text(int status) {
  if (status == CALC_STATUS_READY) return "READY";
  if (status == CALC_STATUS_EDITING) return "EDITING";
  if (status == CALC_STATUS_OK) return "OK";
  if (status == CALC_STATUS_CLEARED) return "CLEARED";
  if (status == CALC_STATUS_OVERFLOW) return "NUMBER RANGE: 0..65535";
  if (status == CALC_STATUS_MISSING_A) return "INPUT A FIRST";
  if (status == CALC_STATUS_MISSING_B) return "INPUT B FIRST";
  if (status == CALC_STATUS_MISSING_OP) return "SELECT OPERATOR";
  if (status == CALC_STATUS_DIV_ZERO) return "DIVIDE BY ZERO";
  if (status == CALC_STATUS_MOD_ZERO) return "MODULO BY ZERO";
  if (status == CALC_STATUS_SHIFT_RANGE) return "SHIFT RANGE: 0..15";
  if (status == CALC_STATUS_UNKNOWN_KEY) return "UNKNOWN KEY";
  return "STATUS?";
}

void calc_draw_u16_field(int row, int focused, char *label, int value, int digits) {
  int attr = TUI_ATTR_NORMAL;
  int label_len = 0;
  int colon_col = 0;
  int value_col = 0;
  char buf[8];

  if (focused) attr = TUI_ATTR_STATUS;
  label_len = calc_text_len(label);
  colon_col = CALC_COL_LABEL + label_len;
  value_col = CALC_COL_VALUE;
  if (value_col <= colon_col) value_col = colon_col + 2;

  tui_fill_line(row, 32, attr);
  if (focused) tui_putc(row, 0, 62, attr);
  else tui_putc(row, 0, 32, attr);

  tui_draw_text(row, CALC_COL_LABEL, label, attr);
  tui_draw_text(row, colon_col, ":", attr);

  if (digits == 0) {
    tui_draw_text(row, value_col, "(empty)", attr);
    return;
  }

  calc_u16_to_dec(value, buf, 8);
  tui_draw_text(row, value_col, buf, attr);
}

void calc_draw_text_field(int row, int focused, char *label, char *text) {
  int attr = TUI_ATTR_NORMAL;
  int label_len = 0;
  int colon_col = 0;
  int value_col = 0;
  if (focused) attr = TUI_ATTR_STATUS;
  label_len = calc_text_len(label);
  colon_col = CALC_COL_LABEL + label_len;
  value_col = CALC_COL_VALUE;
  if (value_col <= colon_col) value_col = colon_col + 2;

  tui_fill_line(row, 32, attr);
  if (focused) tui_putc(row, 0, 62, attr);
  else tui_putc(row, 0, 32, attr);

  tui_draw_text(row, CALC_COL_LABEL, label, attr);
  tui_draw_text(row, colon_col, ":", attr);
  tui_draw_text(row, value_col, text, attr);
}

void calc_apply_cursor(struct CalcState* st) {
  int row = 0;
  int col = 0;

  if (st->input_active == 0) {
    tui_cursor_off();
    return;
  }

  if (st->focus == CALC_FOCUS_A) {
    row = CALC_ROW_A;
    col = CALC_COL_VALUE + st->a_digits;
    tui_cursor_on();
    tui_cursor_at(row, col);
    return;
  }

  if (st->focus == CALC_FOCUS_B) {
    row = CALC_ROW_B;
    col = CALC_COL_VALUE + st->b_digits;
    tui_cursor_on();
    tui_cursor_at(row, col);
    return;
  }

  tui_cursor_off();
}

void calc_draw(struct CalcState* st) {
  tui_clear();

  tui_fill_line(CALC_ROW_TITLE, 32, TUI_ATTR_STATUS);
  tui_draw_text(CALC_ROW_TITLE, 0, "CALC", TUI_ATTR_STATUS);
  tui_draw_text(CALC_ROW_TITLE, 6, "TUI", TUI_ATTR_STATUS);

  tui_fill_line(CALC_ROW_HINT, 32, TUI_ATTR_NORMAL);
  tui_draw_text(CALC_ROW_HINT, 0, "See help for operator/keys usage", TUI_ATTR_NORMAL);

  tui_fill_line(CALC_ROW_KEYS, 32, TUI_ATTR_NORMAL);
  tui_draw_text(CALC_ROW_KEYS, 0, "details: see h: help", TUI_ATTR_NORMAL);

  calc_draw_u16_field(CALC_ROW_A, st->focus == CALC_FOCUS_A, "A", st->a, st->a_digits);
  calc_draw_text_field(CALC_ROW_OP, st->focus == CALC_FOCUS_OP, "OP", calc_op_text(st->op));
  calc_draw_u16_field(CALC_ROW_B, st->focus == CALC_FOCUS_B, "B", st->b, st->b_digits);

  if (st->has_result) {
    calc_draw_u16_field(CALC_ROW_RESULT, 0, "RESULT", st->result, calc_count_digits(st->result));
  } else {
    calc_draw_text_field(CALC_ROW_RESULT, 0, "RESULT", "(none)");
  }

  tui_fill_line(CALC_ROW_STATUS, 32, TUI_ATTR_STATUS);
  tui_draw_text(CALC_ROW_STATUS, 0, calc_status_text(st->status), TUI_ATTR_STATUS);

  calc_apply_cursor(st);
}

void calc_show_help(void) {
  tui_clear();
  tui_cursor_off();
  tui_fill_line(0, 32, TUI_ATTR_STATUS);
  tui_draw_text(0, 0, "CALC HELP", TUI_ATTR_STATUS);

  tui_draw_text(2, 0, "ops: a+ s- d* f/ g% h& j| k^ u<< i>>", TUI_ATTR_NORMAL);
  tui_draw_text(3, 0, "tab/arrows focus A/OP/B", TUI_ATTR_NORMAL);
  tui_draw_text(4, 0, "enter/= compute", TUI_ATTR_NORMAL);
  tui_draw_text(5, 0, "backspace erase", TUI_ATTR_NORMAL);
  tui_draw_text(6, 0, "c clear  r reset  h help  q quit", TUI_ATTR_NORMAL);
  tui_draw_text(8, 0, "flow: A -> op -> B -> enter", TUI_ATTR_NORMAL);
  tui_draw_text(9, 0, "result becomes next A", TUI_ATTR_NORMAL);

  tui_fill_line(CALC_ROW_STATUS, 32, TUI_ATTR_STATUS);
  tui_draw_text(CALC_ROW_STATUS, 0, "press any key...", TUI_ATTR_STATUS);
  read_key();
  tui_cursor_on();
}

void calc_handle_key(struct CalcState* st, int key) {
  int ch = calc_decode_key(key);
  int digit_ch = tui_digit_from_key(key);
  int op = CALC_OP_NONE;

  if (ch == KEY_Q || ch == KEY_ESC) {
    st->running = 0;
    return;
  }

  if (ch == KEY_H) {
    calc_show_help();
    st->input_active = 0;
    st->status = CALC_STATUS_READY;
    return;
  }

  if (ch == KEY_R) {
    calc_reset(st);
    st->input_active = 0;
    st->status = CALC_STATUS_READY;
    return;
  }

  if (ch == KEY_C) {
    calc_clear_focus(st);
    st->input_active = 0;
    st->status = CALC_STATUS_CLEARED;
    return;
  }

  if (ch == KEY_TAB || key == KEY_EXT_RIGHT || key == KEY_EXT_DOWN) {
    calc_move_next_focus(st);
    st->input_active = 0;
    st->status = CALC_STATUS_READY;
    return;
  }

  if (key == KEY_EXT_LEFT || key == KEY_EXT_UP) {
    calc_move_prev_focus(st);
    st->input_active = 0;
    st->status = CALC_STATUS_READY;
    return;
  }

  if (ch == KEY_BACKSPACE) {
    calc_backspace(st);
    st->input_active = 1;
    st->status = CALC_STATUS_EDITING;
    return;
  }

  if (ch == KEY_ENTER || ch == KEY_EQ) {
    st->input_active = 0;
    calc_compute(st);
    return;
  }

  if (digit_ch >= 0) {
    if (calc_push_digit(st, digit_ch)) {
      st->input_active = 1;
      st->status = CALC_STATUS_EDITING;
    } else {
      st->input_active = 0;
      st->status = CALC_STATUS_OVERFLOW;
    }
    return;
  }

  op = calc_op_from_key(calc_to_lower(ch));
  if (op != CALC_OP_NONE) {
    st->op = op;
    st->focus = CALC_FOCUS_B;
    st->input_active = 0;
    st->status = CALC_STATUS_READY;
    return;
  }

  st->input_active = 0;
  st->status = CALC_STATUS_UNKNOWN_KEY;
}

int main() {
  struct CalcState st;
  int key = 0;

  calc_reset(&st);

  while (st.running) {
    calc_draw(&st);
    key = read_key();
    calc_handle_key(&st, key);
  }

  
  tui_clear();
  print("CALC end");
  return 0;
}
