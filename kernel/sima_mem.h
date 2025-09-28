#ifndef __SIMA_MEM__
#define __SIMA_MEM__

#include "sima_type.h"

BOOL		sima_memclr(char *buffer, UINT16 size);
BOOL		sima_memset(void *dst, UINT8 value, UINT16 n);
BOOL		sima_memcpy(void *dst, const void *src, UINT16 n);
BOOL		sima_memmove(void *dst, const void *src, UINT16 n);
MEMC		sima_memcmp(const void *a, const void *b, UINT16 n);

BOOL		sima_strcpy(char *dest, UINT16 dest_cap, const char *src);
BOOL		sima_strcat(char *dest, UINT16 dest_cap, const char *src);
STRC		sima_strcmp(const char *s1, const char *s2);
STRC		sima_strncmp(const char *a, const char *b, UINT16 n);
const char*	sima_strchr(const char *s, char ch);
const char*	sima_strstr(const char *s, const char *pat);
UINT16		sima_strlen(const char *s);
UINT16		sima_strcspl(char *s, char ch, char *outv[], UINT16 max_out);

#endif
