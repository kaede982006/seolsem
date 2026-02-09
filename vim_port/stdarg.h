/*
 * stdarg.h - Variable argument macros for vim on seolsem
 * Minimal implementation for 16-bit
 */

#ifndef _STDARG_H_SEOLSEM
#define _STDARG_H_SEOLSEM

typedef char *va_list;

/* For 16-bit small model, arguments are word-aligned */
#define _INTSIZEOF(n)   ((sizeof(n) + 1) & ~1)

#define va_start(ap, v) ((void)(ap = (va_list)&v + _INTSIZEOF(v)))
#define va_arg(ap, t)   (*(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)))
#define va_end(ap)      ((void)(ap = (va_list)0))
#define va_copy(d, s)   ((void)(d = s))

#endif /* _STDARG_H_SEOLSEM */
