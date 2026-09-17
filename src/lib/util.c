/* ============================================================================
 * TNDDOS — 最小字符串/内存工具
 * 内核不依赖 CRT，所有这一切都自己来。故意写得很笨，胜在能看懂。
 * ==========================================================================*/
#include "tnd.h"

UINTN t_strlen(const char *s) { UINTN n = 0; while (s && s[n]) n++; return n; }

int t_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int t_strncmp(const char *a, const char *b, UINTN n) {
    UINTN i = 0;
    for (; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int t_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int t_stricmp(const char *a, const char *b) {
    while (*a && t_toupper((unsigned char)*a) == t_toupper((unsigned char)*b)) { a++; b++; }
    return t_toupper((unsigned char)*a) - t_toupper((unsigned char)*b);
}

int t_strnicmp(const char *a, const char *b, UINTN n) {
    UINTN i = 0;
    for (; i < n; i++) {
        int ca = t_toupper((unsigned char)a[i]), cb = t_toupper((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!a[i]) return 0;
    }
    return 0;
}

void t_strcpy(char *d, const char *s) { while ((*d++ = *s++) != 0) { } }

void t_strncpy(char *d, const char *s, UINTN cap) {
    UINTN i = 0;
    if (!cap) return;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

void t_memzero(void *d, UINTN n) { unsigned char *p = (unsigned char *)d; while (n--) *p++ = 0; }

void t_memcpy(void *d, const void *s, UINTN n) {
    unsigned char *dp = (unsigned char *)d; const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}

int t_is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void t_upper(char *s) { for (; s && *s; s++) *s = (char)t_toupper((unsigned char)*s); }

char *t_skip_ws(char *s) { while (*s && (*s == ' ' || *s == '\t')) s++; return s; }

void t_rtrim(char *s) {
    UINTN n = t_strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

void t_utoa(UINT64 v, char *out) {
    char b[24]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int j = 0;
    while (i) out[j++] = b[--i];
    out[j] = 0;
}

/* t_ascii_to_u16 / t_utf8_to_u16 已挪到 src/lib/utf8.c —— 驱动也要用，
 * 那边没有内核的依赖，可以被 BOOTX64 / kernel / 各驱动分别链接。 */
