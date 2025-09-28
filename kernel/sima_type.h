#ifndef __SIMA_TYPE__
#define __SIMA_TYPE__

#ifndef NULL
  #define NULL ((void*)0)
#endif

typedef enum { FALSE = 0, TRUE = 1 } BOOL;
typedef enum { STRC_SAME = 0, STRC_DIFF = 1, STRC_ERR = 2 } STRC;
typedef enum { MEMC_SAME = 0, MEMC_DIFF = 1, MEMC_ERR = 2 } MEMC;

typedef unsigned short UINT16;  /* ← 괄호 제거 */
typedef unsigned char  UINT8;   /* ← 괄호 제거 */

#endif
