#ifndef __SIMA_HEAP_H__
#define __SIMA_HEAP_H__

#include "sima_type.h"

/* Backed by sima_heap.asm */
void sima_heap_init(void);
void sima_heap_set(void far *base, UINT16 size);
void far *sima_malloc(UINT16 size);
void sima_free_all(void);
UINT16 sima_heap_remaining(void);
UINT16 sima_heap_mark(void);
void sima_heap_restore(UINT16 mark);

#endif
