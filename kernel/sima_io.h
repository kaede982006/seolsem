#ifndef __SIMAIO__
#define __SIMAIO__

#include "sima_type.h"

extern void print_message(const char* str);
extern void wait_prompt(const char* str, char* buffer);
extern void clear_screen(void);
extern UINT16 read_key(void);
extern void set_cursor(UINT8 row, UINT8 col);
extern void write_char(UINT8 row, UINT8 col, char ch, UINT8 attr);
#endif
