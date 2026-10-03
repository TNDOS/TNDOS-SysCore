/* ============================================================================
 * TNDDOS -- 输出层
 *   log_* : 只走串口，带 [log] 前缀 —— 给机器看的，用来做构建验证
 *   con_* : **路由到控制台服务层**（见 tnd.h 的 TND_CONSOLE）
 *
 * 这一层只做两件跟后端无关的事：
 *   1. 换行翻译（LF -> CRLF）
 *   2. 后端注册与选择
 * 真正的输出在 lib/con_uefi.c 和 lib/con_fb.c 里。
 * ==========================================================================*/
#include "tnd.h"

static EFI_SERIAL_IO_PROTOCOL *gSer = 0;

/* 已知的后端。数组固定大小，嵌入式不允许动态增长。 */
#define CON_MAX_BACKENDS 4
static const TND_CONSOLE *gBackends[CON_MAX_BACKENDS];
static int  gBackendCount = 0;
static const TND_CONSOLE *gCon = 0;

/* 属性由**路由层**自己记着。
 * 理由和光标一样：ConOut->Mode->Attribute 在切到 fb 之后是过期值，
 * 而 fb 的当前属性只有它自己知道。让路由层记账，两边就统一了，
 * 后端也不用各写一遍 —— UEFI 那边本来也读不到自己的当前属性。 */
static UINTN gConAttr = 0x07;

void log_init(EFI_SYSTEM_TABLE *st) {
    VOID *ser = 0;
    if (st->BootServices && st->BootServices->LocateProtocol) {
        if (!EFI_ERROR(st->BootServices->LocateProtocol(&gEfiSerialIoProtocolGuid, 0, &ser)))
            gSer = (EFI_SERIAL_IO_PROTOCOL *)ser;
    }
    gEnv.Ser = gSer;

    /* 注册两个后端，默认用 UEFI 的。
     *
     * **fb 只注册、不 Init** —— 它的 Init 要分配影子缓冲，而 log_init 跑在
     * 内核堆就绪之前。所以切换成 fb 是内核启动后、由用户显式触发的事。 */
    con_register(&gConUefi);
    con_register(&gConFb);
    con_select("uefi");
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

/* ------------------------------------------------------------ 服务层注册 */
void con_register(const TND_CONSOLE *c) {
    if (!c || gBackendCount >= CON_MAX_BACKENDS) return;
    for (int i = 0; i < gBackendCount; i++) if (gBackends[i] == c) return;
    gBackends[gBackendCount++] = c;
}

static int con_eq(const char *a, const char *b) { return t_stricmp(a, b) == 0; }

int con_select(const char *name) {
    for (int i = 0; i < gBackendCount; i++) {
        if (!con_eq(name, gBackends[i]->Name)) continue;
        /* 已经选中的就不重跑 Init —— 重入会把状态搞乱 */
        if (gCon == gBackends[i]) return 1;
        if (gBackends[i]->Init && !gBackends[i]->Init()) {
            log_puts("[log] con backend init FAILED: "); log_puts(name); log_puts("\r\n");
            return 0;
        }
        gCon = gBackends[i];
        /* 把路由层记着的属性补设给新后端 —— 否则切过去之后颜色会丢掉 */
        if (gCon->SetAttr) gCon->SetAttr(gConAttr);
        log_puts("[log] console backend = "); log_puts(name); log_puts("\r\n");
        return 1;
    }
    log_puts("[log] con backend not found: "); log_puts(name); log_puts("\r\n");
    return 0;
}

const char *con_current(void) { return gCon ? gCon->Name : "(none)"; }

void con_report(void) {
    for (int i = 0; i < gBackendCount; i++) {
        con_puts(gBackends[i] == gCon ? "  * " : "    ");
        con_puts(gBackends[i]->Name);
        con_puts("\r\n");
    }
}

/* --------------------------------------------------------- 后端无关的部分 */

/* 换行翻译：OVMF 的 ConOut 把单独的 LF 当成「下移一行」——**不回到行首**。
 * 所以字符串里写 "\n" 就会一路斜下去，行尾的东西还会被卷到下一行开头
 * （看起来像丢字符）。修在这里而不是要求每个调用方记得写 CRLF ——
 * 后者已经出过事故了。DOS 的 BIOS teletype 也是 LF = 回车+换行。 */
void con_write(const char *s, UINTN n) {
    char tmp[512];
    UINTN off = 0;
    if (!gCon || !s || !n) return;
    while (off < n) {
        UINTN k = 0;
        while (off < n && k < sizeof(tmp) - 3) {
            char c = s[off++];
            if (c == '\n' && (k == 0 || tmp[k - 1] != '\r')) tmp[k++] = '\r';
            tmp[k++] = c;
        }
        gCon->Write(tmp, k);
    }
}

void con_puts(const char *s) { if (s) con_write(s, t_strlen(s)); }
void con_putc(char c) { char b[2]; b[0] = c; b[1] = 0; con_puts(b); }

void con_set_attr(UINTN attr)  { gConAttr = attr & 0xFF; if (gCon && gCon->SetAttr) gCon->SetAttr(gConAttr); }
UINTN con_get_attr(void)       { return gConAttr; }
void con_reset_attr(void)      { con_set_attr(0x07); }
void con_cursor(int visible)   { if (gCon && gCon->Cursor) gCon->Cursor(visible); }
void con_clear(void)           { if (gCon && gCon->Clear) gCon->Clear(); }
void  con_gotoxy(UINTN x, UINTN y) { if (gCon && gCon->GotoXY) gCon->GotoXY(x, y); }
void  con_getxy(UINTN *x, UINTN *y) { if (gCon && gCon->GetXY) gCon->GetXY(x, y); else { if (x) *x = 0; if (y) *y = 0; } }

int   con_set_scale(int pct)   { return (gCon && gCon->SetScale) ? gCon->SetScale(pct) : 0; }
int   con_get_scale(void)      { return (gCon && gCon->GetScale) ? gCon->GetScale() : 100; }

UINTN con_cols(void)           { return (gCon && gCon->Cols) ? gCon->Cols() : 80; }
UINTN con_rows(void)           { return (gCon && gCon->Rows) ? gCon->Rows() : 25; }

/* ------------------------------------------------------------ 批量原语
 * 后端有快速路径就用；没有就逐格兜底。
 * 兜底路径是**故意保留的** —— UEFI 的 ConOut 本来就只能逐格设属性，
 * 与其让两个后端都写一遍循环，不如让路由层当那个默认实现。 */
void con_write_cells(UINTN x, UINTN y, UINTN w, UINTN h, const TND_CELL *cells, UINTN stride) {
    UINTN r, c;
    if (!cells) return;
    if (gCon && gCon->WriteCells) { gCon->WriteCells(x, y, w, h, cells, stride); return; }
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            con_gotoxy(x + c, y + r);
            con_set_attr(cells[r * stride + c].Attr);
            con_putc((char)(cells[r * stride + c].Ch & 0xFF));
        }
}

void con_fill(UINTN x, UINTN y, UINTN w, UINTN h, UINTN ch, UINTN attr) {
    UINTN r, c;
    if (gCon && gCon->Fill) { gCon->Fill(x, y, w, h, ch, attr); return; }
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) { con_gotoxy(x + c, y + r); con_set_attr(attr); con_putc((char)ch); }
}

void con_scroll(UINTN x, UINTN y, UINTN w, UINTN h, int dy, UINTN ch, UINTN attr) {
    UINTN r, c;
    if (gCon && gCon->Scroll) { gCon->Scroll(x, y, w, h, dy, ch, attr); return; }
    /* 兜底：一行一行地搬。UEFI ConOut 没有区域滚动，只能这样。 */
    for (r = 0; r < h; r++) {
        long src = (long)r + dy;
        if (src < 0 || src >= (long)h) {
            for (c = 0; c < w; c++) { con_gotoxy(x + c, y + r); con_set_attr(attr); con_putc((char)ch); }
        } else {
            for (c = 0; c < w; c++) {
                con_gotoxy(x + c, y + r);
                /* 读回源行内容做不到（没有读格子的接口），所以兜底只填空行。
                 * 这是**已知的降级**，不是 bug —— 真正要滚动的程序请用 fb 后端。 */
                con_set_attr(attr); con_putc((char)ch);
            }
        }
    }
}

void con_u64(UINT64 v) { char b[24]; t_utoa(v, b); con_puts(b); }

void con_hex(UINT64 v) {
    const char *hx = "0123456789ABCDEF";
    con_puts("0x");
    for (int i = 15; i >= 0; i--) con_putc(hx[(v >> (i * 4)) & 0xF]);
}

void con_kv(const char *k, UINT64 v, const char *unit) {
    con_puts("  ");
    con_puts(k);
    for (UINTN i = t_strlen(k); i < 12; i++) con_puts(" ");
    con_puts(": ");
    con_u64(v);
    if (unit) { con_puts(" "); con_puts(unit); }
    con_puts("\r\n");
}
