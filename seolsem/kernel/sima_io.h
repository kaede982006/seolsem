#ifndef __SIMAIO__
#define __SIMAIO__

#include "sima_type.h"

extern void print_message(const char* str);
extern void wait_prompt(const char* str, char* buffer);
extern void wait_prompt_masked(const char* str, char* buffer);
extern void clear_screen(void);
extern UINT16 read_key(void);
extern void set_cursor(UINT8 row, UINT8 col);
extern void write_char(UINT8 row, UINT8 col, char ch, UINT8 attr);
extern void sync_ds(void);
extern void enable_irq(void);
extern void disable_irq(void);
extern void debug_trigger_div0(void);
extern void debug_trigger_ud(void);
extern void debug_trigger_gp(void);
extern void debug_trigger_bp(void);
extern void debug_trigger_of(void);
#endif
