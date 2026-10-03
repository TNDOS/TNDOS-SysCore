/* ============================================================================
 * 控制台后端：UEFI ConOut
 *
 * 这是原来 log.c 里的那套，搬到后端接口后面。行为一字未改 ——
 * 搬家的目的是让内核不再**写死**在 ConOut 上，而不是改变它的行为。
 * ==========================================================================*/
#include "tnd.h"

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *gOut = 0;

static int uefi_init(void) {
    gOut = gEnv.ST ? gEnv.ST->ConOut : 0;
    if (!gOut) return 0;
    /* 光标默认可见。固件的默认状态不保证，而看不见光标的屏幕在 EDIT 里没法用。 */
    if (gOut->EnableCursor) gOut->EnableCursor(gOut, 1);
    return 1;
}

static void uefi_write(const char *s, UINTN n) {
    static CHAR16 wbuf[1024];
    char tmp[512];
    UINTN off = 0;
    if (!gOut || !s || !n) return;
    while (off < n) {
        UINTN k = n - off;
        if (k > sizeof(tmp) - 1) k = sizeof(tmp) - 1;
        t_memcpy(tmp, s + off, k);
        tmp[k] = 0;
        t_utf8_to_u16(tmp, wbuf, 1024);
        gOut->OutputString(gOut, wbuf);
        off += k;
    }
}

static void uefi_clear(void)  { if (gOut && gOut->ClearScreen) gOut->ClearScreen(gOut); }
static void uefi_gotoxy(UINTN x, UINTN y) { if (gOut && gOut->SetCursorPosition) gOut->SetCursorPosition(gOut, x, y); }
static void uefi_setattr(UINTN a) { if (gOut && gOut->SetAttribute) gOut->SetAttribute(gOut, a); }
static void uefi_cursor(int v)    { if (gOut && gOut->EnableCursor) gOut->EnableCursor(gOut, v ? 1 : 0); }

static UINTN uefi_cols(void) {
    UINTN c = 80, r = 25;
    if (gOut && gOut->Mode) gOut->QueryMode(gOut, gOut->Mode->Mode, &c, &r);
    return c ? c : 80;
}
static UINTN uefi_rows(void) {
    UINTN c = 80, r = 25;
    if (gOut && gOut->Mode) gOut->QueryMode(gOut, gOut->Mode->Mode, &c, &r);
    return r ? r : 25;
}

const TND_CONSOLE gConUefi = {
    "uefi", uefi_init, uefi_write, uefi_clear, uefi_gotoxy,
    uefi_setattr, uefi_cursor, uefi_cols, uefi_rows
};
