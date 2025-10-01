BOOL   sima_utoa(UINT16 v, char *dst, UINT16 cap, UINT16 base); /* base: 2,10,16 */
BOOL   sima_itoh(UINT16 v, char *dst, UINT16 cap);              /* 0xFFFF 형식 */
BOOL   sima_atoi(const char *s, UINT16 *out);                   /* 10진 */
BOOL   sima_atox(const char *s, UINT16 *out);                   /* 16진 "0x.." 허용 */
