#include "sima_type.h"

BOOL memcl(char *buffer, UINT16 size) {
    UINT16 i;                /* C89: for 안에서 선언 불가 */
    if (buffer == NULL) {
        return FALSE;
    }
    for (i = 0; i < size; ++i) {
        buffer[i] = '\0';
    }
    return TRUE;
}
BOOL sima_strcpy(char *dest, UINT16 dest_cap, const char *src) {
    UINT16 i;
	if (!dest || !src || dest_cap == 0)
		return FALSE;

    i=0;
    // dest에 최소 1바이트는 널문자 자리로 남겨야 한다
    while (i + 1 < dest_cap && src[i] != '\0') {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';           // 반드시 널 종료

    // src를 끝까지 다 읽었는지로 성공/실패 판단
    return src[i] == '\0';
}
