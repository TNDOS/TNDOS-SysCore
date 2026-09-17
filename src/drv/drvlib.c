#include "drv.h"

void dputs(EFI_SYSTEM_TABLE *st, const char *s) {
    static CHAR16 wbuf[2048];
    if (!st || !st->ConOut) return;
    t_utf8_to_u16(s, wbuf, 2048);
    st->ConOut->OutputString(st->ConOut, wbuf);
}

void dputc(EFI_SYSTEM_TABLE *st, char c) { char b[2]; b[0] = c; b[1] = 0; dputs(st, b); }

void dnum(EFI_SYSTEM_TABLE *st, UINT64 v) {
    char b[24]; int i = 0;
    if (!v) { dputs(st, "0"); return; }
    while (v) { b[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i) dputc(st, b[--i]);
}

void dhex(EFI_SYSTEM_TABLE *st, UINT64 v) {
    const char *h = "0123456789ABCDEF";
    dputs(st, "0x");
    for (int i = 15; i >= 0; i--) dputc(st, h[(v >> (i * 4)) & 0xF]);
}
