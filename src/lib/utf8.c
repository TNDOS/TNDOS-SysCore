#include "tnd_utf8.h"

/* ASCII -> CHAR16（逐字节，用于纯 ASCII 路径串） */
void t_ascii_to_u16(const char *s, CHAR16 *d, UINTN max) {
    UINTN i = 0;
    if (!max) return;
    for (; s[i] && i + 1 < max; i++) d[i] = (CHAR16)(unsigned char)s[i];
    d[i] = 0;
}

/* UTF-8 -> UTF-16。只处理 BMP，增补平面替换成 '?'。 */
void t_utf8_to_u16(const char *s, CHAR16 *d, UINTN maxChars) {
    UINTN i = 0, o = 0;
    if (!maxChars) return;
    while (s[i] && o + 1 < maxChars) {
        unsigned char c = (unsigned char)s[i];
        UINT32 cp;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0) {
            if (((unsigned char)s[i+1] & 0xC0) != 0x80) { cp = '?'; i += 1; }
            else { cp = ((UINT32)(c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F); i += 2; }
        } else if ((c & 0xF0) == 0xE0) {
            if (((unsigned char)s[i+1] & 0xC0) != 0x80 || ((unsigned char)s[i+2] & 0xC0) != 0x80) { cp = '?'; i += 1; }
            else { cp = ((UINT32)(c & 0x0F) << 12) | ((UINT32)((unsigned char)s[i+1] & 0x3F) << 6)
                      | ((unsigned char)s[i+2] & 0x3F); i += 3; }
        } else if ((c & 0xF8) == 0xF0) {
            if (((unsigned char)s[i+1] & 0xC0) != 0x80 || ((unsigned char)s[i+2] & 0xC0) != 0x80
             || ((unsigned char)s[i+3] & 0xC0) != 0x80) { cp = '?'; i += 1; }
            else { cp = ((UINT32)(c & 0x07) << 18) | ((UINT32)((unsigned char)s[i+1] & 0x3F) << 12)
                      | ((UINT32)((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F); i += 4; }
        } else { cp = '?'; i += 1; }

        if (cp > 0xFFFF) cp = '?';
        d[o++] = (CHAR16)cp;
    }
    d[o] = 0;
}
