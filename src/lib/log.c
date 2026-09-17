/* ============================================================================
 * TNDDOS — 输出层
 *   log_* : 只走串口，带 [log] 前缀 —— 给机器看的，用来做构建验证
 *   con_* : 走 UEFI ConOut（用户肉眼可见），同时镜像到串口 —— 给日志留完整记录
 * ==========================================================================*/
#include "tnd.h"

static EFI_SERIAL_IO_PROTOCOL *gSer = 0;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *gOut = 0;

void log_init(EFI_SYSTEM_TABLE *st) {
    VOID *ser = 0;
    gOut = st->ConOut;
    if (st->BootServices && st->BootServices->LocateProtocol) {
        if (!EFI_ERROR(st->BootServices->LocateProtocol(&gEfiSerialIoProtocolGuid, 0, &ser)))
            gSer = (EFI_SERIAL_IO_PROTOCOL *)ser;
    }
    gEnv.Ser = gSer;
}

static void ser_raw(const char *s) {
    if (!gSer) return;
    while (*s) { UINTN n = 1; gSer->Write(gSer, &n, (void *)s); s++; }
}

void log_putc(char c) { char b[2]; b[0] = c; b[1] = 0; ser_raw(b); }
void log_puts(const char *s) { ser_raw(s); }
void log_u64(UINT64 v) { char b[24]; t_utoa(v, b); ser_raw(b); }

void log_hex(UINT64 v) {
    char b[19]; const char *h = "0123456789ABCDEF"; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++) b[2 + i] = h[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    ser_raw(b);
}

void log_kv(const char *k, const char *v) { ser_raw("[log] "); ser_raw(k); ser_raw(" = "); ser_raw(v); ser_raw("\r\n"); }
void log_kv_u64(const char *k, UINT64 v) { ser_raw("[log] "); ser_raw(k); ser_raw(" = "); log_u64(v); ser_raw("\r\n"); }

/* ------------------------------------------------------------- 用户控制台 */

/* 注意：故意不再手工把 con_* 镜像到串口。
 * UEFI 的 ConOut 是个 console splitter，通常已经同时连着视频控制台和串口终端，
 * 再镜像一遍日志里每条就是双份。这里只走 ConOut 一条路。 */
#define TND_CON_MIRROR_SERIAL 0

void con_puts(const char *s) {
    static CHAR16 wbuf[4096];
#if TND_CON_MIRROR_SERIAL
    ser_raw(s);
#endif
    if (!gOut) return;
    t_utf8_to_u16(s, wbuf, 4096);      /* ConOut 吃 UCS-2，源码是 UTF-8，必须先解码 */
    gOut->OutputString(gOut, wbuf);
}

void con_putc(char c) { char b[2]; b[0] = c; b[1] = 0; con_puts(b); }

void con_u64(UINT64 v) { char b[24]; t_utoa(v, b); con_puts(b); }

void con_clear(void) { if (gOut && gOut->ClearScreen) gOut->ClearScreen(gOut); }

/* 对齐的 "标签 : 数字 单位"。标签宽度按字节算，所以标签请用 ASCII。 */
void con_kv(const char *k, UINT64 v, const char *unit) {
    con_puts("  ");
    con_puts(k);
    for (UINTN i = t_strlen(k); i < 12; i++) con_puts(" ");
    con_puts(": ");
    con_u64(v);
    if (unit) { con_puts(" "); con_puts(unit); }
    con_puts("\r\n");
}
