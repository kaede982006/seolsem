#include "sima_mem.h"
#include "sima_io.h"
/* 메모리 clear */
BOOL sima_memclr(char *buffer, UINT16 size) {
    UINT16 i;
    if (buffer == NULL) return FALSE;
    for (i = 0; i < size; ++i) buffer[i] = '\0';
    return TRUE;
}

/* 안전 strcpy: 전부 복사되면 TRUE, 잘림이면 FALSE */
BOOL sima_strcpy(char *dest, UINT16 dest_cap, const char *src) {
    UINT16 i;
    if (!dest || !src || dest_cap == 0) return FALSE;

    i = 0;
    while ((UINT16)(i + 1) < dest_cap && src[i] != '\0') {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
    return (src[i] == '\0');   /* 남은 문자가 없으면 TRUE, 아니면 FALSE(잘림) */
}

/* strcmp: 같으면 STRC_SAME, 다르면 STRC_DIFF, 인자 오류면 STRC_ERR */
STRC sima_strcmp(const char *s1, const char* s2) {
    UINT16 i;
    if (!s1 || !s2) return STRC_ERR;
    for (i = 0; ; ++i) {
        char a = s1[i];
        char b = s2[i];
        if (a != b) return STRC_DIFF;
        if (a == '\0') break;  /* a==b=='\0' → 끝 */
    }
    return STRC_SAME;
}

STRC sima_strncmp(const char *s1, const char *s2, UINT16 n) {
    UINT16 i;
    if (n == 0) return STRC_SAME;   /* 0글자 비교는 같음 */
    if (!s1 || !s2) return STRC_ERR;

    for (i = 0; i < n; ++i) {
        char a = (char)s1[i];
        char b = (char)s2[i];
        if (a != b) return STRC_DIFF;  /* 첫 차이에서 종료 */
        if (a == '\0') return STRC_SAME; /* a==b=='\0' → 동일 */
    }
    return STRC_SAME;  /* n글자 모두 동일 */
}

/* 안전 strcat: 가능한 만큼만 이어 붙임, 전부 붙이면 TRUE, 잘리면 FALSE */
BOOL sima_strcat(char *dest, UINT16 dest_cap, const char* src) {
    UINT16 i, dest_len, avail;
    if (!dest || !src || dest_cap == 0) return FALSE;

    /* dest의 현재 길이 찾기 (널 위치) */
    dest_len = 0;
    while (dest_len < dest_cap && dest[dest_len] != '\0') ++dest_len;
    if (dest_len >= dest_cap) {           /* 널이 없으면 비정상 */
        dest[dest_cap - 1] = '\0';
        return FALSE;
    }

    /* 남은 공간(널 자리 1 포함해서 계산) */
    avail = (UINT16)(dest_cap - dest_len - 1);
    i = 0;
    while (i < avail && src[i] != '\0') {
        dest[dest_len + i] = src[i];
        ++i;
    }
    dest[dest_len + i] = '\0';
    return (src[i] == '\0');              /* 다 붙였으면 TRUE, 아니면 FALSE(잘림) */
}

/* strlen (NULL 입력은 0으로 간주—원하면 STRC_ERR 정책으로 일관 가능) */
UINT16 sima_strlen(const char* s) {
    UINT16 n = 0;
    if (!s) return 0;
    while (s[n] != '\0') ++n;
    return n;
}
/* memset: dst가 NULL이면 FALSE, 그 외 n바이트를 value로 채움 */
BOOL sima_memset(void *dst, UINT8 value, UINT16 n) {
    UINT16 i;
    UINT8 *d;
    if (!dst) return FALSE;
    d = (UINT8*)dst;
    for (i = 0; i < n; ++i) {
        d[i] = value;
    }
    return TRUE;
}

/* memcpy: 겹침(overlap) 금지. 겹치면 FALSE 반환 */
BOOL sima_memcpy(void *dst, const void *src, UINT16 n) {
    UINT16 i;
    UINT8 *d;
    const UINT8 *s;
    if (!dst || !src) return FALSE;

    d = (UINT8*)dst;
    s = (const UINT8*)src;

    /* 겹침 검사: [d, d+n) 와 [s, s+n) 교차하면 금지 */
    if ((d < s && d + n > s) || (s < d && s + n > d)) {
        return FALSE; /* 겹치면 memmove 사용 권장 */
    }

    for (i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return TRUE;
}

/* memmove: 겹침 허용 */
BOOL sima_memmove(void *dst, const void *src, UINT16 n) {
    UINT16 i;
    UINT8 *d;
    const UINT8 *s;
    if (!dst || !src) return FALSE;

    d = (UINT8*)dst;
    s = (const UINT8*)src;

    if (d == s || n == 0) return TRUE;

    if (d < s) {              /* 앞→뒤 복사 */
        for (i = 0; i < n; ++i) d[i] = s[i];
    } else {                  /* 뒤→앞 복사 */
        for (i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return TRUE;
}

MEMC sima_memcmp(const void *a, const void *b, UINT16 n) {
    const UINT8 *pa, *pb;
    UINT16 i;

    if ((!a || !b) && n != 0) return MEMC_ERR;  /* 인자 검사 */
    if (n == 0) return MEMC_SAME;               /* 길이 0은 같다고 간주 */

    pa = (const UINT8*)a;
    pb = (const UINT8*)b;

    for (i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return MEMC_DIFF;
    }
    return MEMC_SAME;
}

/*
의미: 널로 종료된 문자열 s 안에서 문자 ch가 처음 나타나는 위치를 찾는다.
반환값: 찾으면 그 위치를 가리키는 포인터, 없으면 NULL.
특이사항: 표준 strchr처럼 ch=='\0'을 찾는 것도 허용(종단 널의 주소를 반환).
*/
const char* sima_strchr(const char *s, char ch)
{
    char *p;        /* 선언을 맨 앞에 */
    char target;

    if (!s) return NULL;

    p = (char*)s;   /* 그 다음에 대입 */
    target = ch;

    while (*p) {
        if (*p == target) return p;
        ++p;
    }
    /* 문자열 끝: ch가 '\0'이면 그 위치 반환 */
    return (target == '\0') ? p : NULL;
}

/*
의미: 문자열 s 안에서 부분문자열 pat의 첫 발생 위치를 찾는다.
반환값: 찾으면 그 시작 위치 포인터, 없으면 NULL.
특이사항: pat이 빈 문자열("")이면 항상 s를 반환(표준 동작).
*/
const char* sima_strstr(const char *s, const char *pat) {
    UINT16 i, j;

    if (!s || !pat) return (const char*)0;
    if (*pat == '\0') return s;

    for (i = 0; s[i] != '\0'; ++i) {
        if (s[i] == pat[0]) {
            for (j = 0; pat[j] != '\0'; ++j) {
                if (s[i + j] == '\0' || s[i + j] != pat[j])
                    break;
            }
            if (pat[j] == '\0') return s + i;  /* 패턴 끝까지 일치 */
        }
    }
    return (const char*)0;
}
UINT16 sima_strcspl(char *s, char ch, char *outv[], UINT16 max_out) {
    UINT16 n;
    char *p;
	if (!s || !outv || max_out == 0) {
		return 0;
	}

    n = 0; p = s;
	
	while (*p == ch) ++p;
    while (*p != '\0' && n < max_out) {
        outv[n++] = p;                         /* 토큰 시작 */
        while (*p != '\0' && *p != ch) ++p;     /* 토큰 끝 */
        if (*p == '\0') break;
        *p++ = '\0';                            /* in-place 종결 */
        while (*p == ch) ++p;                   /* 연속 구분자 스킵 */
    }
    return n;
}



