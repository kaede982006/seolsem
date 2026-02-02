#include "sima_conv.h"
#include "sima_mem.h"

static BOOL sima_is_digit(char ch, UINT16 base) {
    if (base == 10) return (ch >= '0' && ch <= '9');
    if (base == 16) {
        return (ch >= '0' && ch <= '9') ||
               (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    }
    return FALSE;
}

static UINT16 sima_digit_value(char ch) {
    if (ch >= '0' && ch <= '9') return (UINT16)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (UINT16)(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return (UINT16)(10 + ch - 'A');
    return 0;
}

BOOL sima_utoa(UINT16 v, char *dst, UINT16 cap, UINT16 base) {
    char temp[17];
    UINT16 i;
    UINT16 pos;

    if (!dst || cap == 0) return FALSE;
    if (!(base == 2 || base == 10 || base == 16)) return FALSE;

    if (v == 0) {
        if (cap < 2) return FALSE;
        dst[0] = '0';
        dst[1] = '\0';
        return TRUE;
    }

    i = 0;
    while (v != 0 && i < (UINT16)sizeof(temp) - 1) {
        UINT16 digit = (UINT16)(v % base);
        temp[i++] = (char)((digit < 10) ? ('0' + digit) : ('A' + (digit - 10)));
        v = (UINT16)(v / base);
    }
    temp[i] = '\0';

    if ((UINT16)(i + 1) > cap) {
        dst[0] = '\0';
        return FALSE;
    }

    pos = 0;
    while (i > 0) {
        dst[pos++] = temp[--i];
    }
    dst[pos] = '\0';
    return TRUE;
}

BOOL sima_itoh(UINT16 v, char *dst, UINT16 cap) {
    char hex[8];
    if (!dst || cap < 3) return FALSE;
    if (!sima_utoa(v, hex, (UINT16)sizeof(hex), 16)) return FALSE;
    if (!sima_strcpy(dst, cap, "0x")) return FALSE;
    return sima_strcat(dst, cap, hex);
}

BOOL sima_atoi(const char *s, UINT16 *out) {
    UINT16 value;
    UINT16 i;
    if (!s || !out) return FALSE;

    value = 0;
    i = 0;
    while (s[i] == ' ') ++i;
    if (!sima_is_digit(s[i], 10)) return FALSE;

    while (sima_is_digit(s[i], 10)) {
        value = (UINT16)(value * 10 + sima_digit_value(s[i]));
        ++i;
    }
    *out = value;
    return TRUE;
}

BOOL sima_atox(const char *s, UINT16 *out) {
    UINT16 value;
    UINT16 i;
    if (!s || !out) return FALSE;

    value = 0;
    i = 0;
    while (s[i] == ' ') ++i;
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;

    if (!sima_is_digit(s[i], 16)) return FALSE;

    while (sima_is_digit(s[i], 16)) {
        value = (UINT16)(value * 16 + sima_digit_value(s[i]));
        ++i;
    }
    *out = value;
    return TRUE;
}
