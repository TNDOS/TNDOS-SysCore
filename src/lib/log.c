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

    /* 把光标打开。固件的默认状态不保证是"可见"，
     * 而一个看不见光标的屏幕在 EDIT 里根本没法用。 */
    if (gOut && gOut->EnableCursor) gOut->EnableCursor(gOut, 1);
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

/* --------------------------------------------------------- 换行翻译
 * OVMF 的 ConOut 把单独的 LF 当成「下移一行」——**不回到行首**。
 * 所以字符串里写 "\n" 就会一路斜下去，而且上一行末尾的东西会被卷到
 * 下一行开头（看起来像丢字符）。
 *
 * 修在控制台层，而不是要求每个调用方都记得写 "\r\n" ——
 * 后者已经出过一次事故了：EXECOM 用 tnd_puts 输出带 \n 的行，
 * 光标停在行中间，紧接着内核打印的 "returned 1" 就从那里接着打，
 * 看起来像内核坏了，其实是被程序的坏光标带歪的。
 *
 * DOS 的 BIOS teletype 也是这么做的：LF 就是"回车+换行"。
 * 已经写了 \r\n 的不会被翻倍（检查前一个字符是不是 \r）。 */
void con_write(const char *s, UINTN n) {
    static CHAR16 wbuf[1024];
    char tmp[512];
    UINTN off = 0;

    if (!s || !n) return;

    while (off < n) {
        UINTN k = 0;
        while (off < n && k < sizeof(tmp) - 3) {
            char c = s[off++];
            if (c == '\n' && (k == 0 || tmp[k - 1] != '\r')) tmp[k++] = '\r';
            tmp[k++] = c;
        }
        tmp[k] = 0;
        if (gOut) {
            t_utf8_to_u16(tmp, wbuf, 1024);
            gOut->OutputString(gOut, wbuf);
        }
    }
}

void con_puts(const char *s) {
    if (!s) return;
#if TND_CON_MIRROR_SERIAL
    ser_raw(s);
#endif
    con_write(s, t_strlen(s));
}

void con_putc(char c) { char b[2]; b[0] = c; b[1] = 0; con_puts(b); }

/* --------------------------------------------------------- 颜色
 * UEFI 的 EFI_TEXT_ATTR(fg, bg) = fg | (bg << 4) —— 和 DOS 的 VGA 属性字节
 * **恰好一模一样**（0=黑 1=蓝 2=绿 3=青 4=红 5=品红 6=棕 7=浅灰 …）。
 * 所以 DOS 的颜色常量可以直接用，不需要任何翻译。 */
void con_set_attr(UINTN attr) {
    if (gOut && gOut->SetAttribute) gOut->SetAttribute(gOut, attr);
}

void con_reset_attr(void) { con_set_attr(0x07); }   /* 浅灰 on 黑，和 DOS 默认一致 */

/* 光标显隐。之前内核从来没碰过它，结果就是**屏幕上没有光标** ——
 * 在 Shell 里还能忍（有提示符），在 EDIT 里就是灾难：不知道字会插到哪。
 *
 * 注意：UEFI 文本模式**不提供设置光标形状**的接口，所以 DOS 那种
 * "下划线闪烁 / Ins 后变成整块"没法照搬。固件给什么形状就是什么形状，
 * 想要整块只能自己用反白画（EDIT 就是这么做的）。 */
void con_cursor(int visible) {
    if (gOut && gOut->EnableCursor) gOut->EnableCursor(gOut, visible ? 1 : 0);
}

void con_u64(UINT64 v) { char b[24]; t_utoa(v, b); con_puts(b); }

void con_clear(void) { if (gOut && gOut->ClearScreen) gOut->ClearScreen(gOut); }

/* 0x + 16 位十六进制，和 log_hex 对齐。地址一律用它，别再内联写循环。 */
void con_hex(UINT64 v) {
    const char *hx = "0123456789ABCDEF";
    con_puts("0x");
    for (int i = 15; i >= 0; i--) con_putc(hx[(v >> (i * 4)) & 0xF]);
}

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
