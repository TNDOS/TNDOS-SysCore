/* ============================================================================
 * TNDDOS -- 点阵字体（TNDF v1）加载与查找
 *
 * 格式：
 *   头 32 字节（小端）
 *     0x00 char[4] 魔数 "TNDF"     0x0C u16 每像素位数（现为 1）
 *     0x04 u16 版本                 0x10 u32 索引偏移
 *     0x06 u16 格子高 cellH         0x14 u32 索引项数
 *     0x08 u16 窄字形宽             0x18 u32 字形数据偏移
 *     0x0A u16 宽字形宽              0x1C u32 保留
 *   索引项 8 字节，按码点升序：u32 码点 + u32 字形偏移（相对字形区）
 *   字形：cellH 行 x (窄|宽)/8 字节，1bpp，bit7 在最左
 *
 * **索引必须升序** —— 查找是二分，顺序错了就静默返回"没这个字"，
 * 症状是"有的中文能显示有的不能"，很难查。所以加载时校验一次。
 *
 * 为什么不用 UEFI 的字体协议：那是 Boot Services，ExitBootServices 之后
 * 就没了。字体必须是**我们自己的数据**，从磁盘读进来，之后不依赖固件。
 * ==========================================================================*/
#include "tnd.h"

#define TNDF_MAX_BYTES  (2 * 1024 * 1024)   /* 字体文件上限，防止读进一个巨大的东西 */

typedef struct {
    char   Magic[4];
    UINT16 Version;
    UINT16 CellH;
    UINT16 NarrowW;
    UINT16 WideW;
    UINT16 Bpp;
    UINT16 Reserved;
    UINT32 IndexOffset;
    UINT32 IndexCount;
    UINT32 GlyphOffset;
    UINT32 Pad;
} TNDF_HEADER;

static UINT8              *gData = 0;
static const TNDF_HEADER  *gHdr  = 0;

int font_ready(void)      { return gHdr != 0; }
UINTN font_cell_h(void)   { return gHdr ? gHdr->CellH   : 0; }
UINTN font_narrow_w(void) { return gHdr ? gHdr->NarrowW : 0; }
UINTN font_wide_w(void)   { return gHdr ? gHdr->WideW   : 0; }

static UINT32 rd32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}
static UINT16 rd16(const UINT8 *p) { return (UINT16)((UINT32)p[0] | ((UINT32)p[1] << 8)); }

/* 加载字体。path 走 VFS（DOS 路径语义）。
 * 失败只记日志、返回 0 —— 调用方会退回内嵌的 8x12 字体，控制台照样能用。 */
int font_load(const char *path) {
    UINT8  *buf;
    UINTN   len = 0;
    UINT32  i, prev;
    UINT32  cellH, nw, ww, bpp, iOff, iCnt, gOff, glyphBytes = 0;

    if (gHdr) return 1;                       /* 已经加载过 */

    /* 用 t_read_file_alloc 而不是 vfs_read_all + kmalloc：
     * 它在 file.c 里，**引导器和内核两个构建都有** —— 这样本文件可以留在
     * 共用的 LibSrc 里。代价是路径按 UEFI 的来，不走 VFS 的 DOS 语义。 */
    if (EFI_ERROR(t_read_file_alloc(path, (void **)&buf, &len)) || !buf || len < 32) {
        log_puts("[log] font: cannot read "); log_puts(path); log_puts("\r\n");
        return 0;
    }

    if (buf[0] != 'T' || buf[1] != 'N' || buf[2] != 'D' || buf[3] != 'F') {
        log_puts("[log] font: bad magic (not a TNDF file)\r\n");
        return 0;
    }

    cellH = rd16(buf + 6); nw = rd16(buf + 8); ww = rd16(buf + 10); bpp = rd16(buf + 12);
    iOff  = rd32(buf + 16); iCnt = rd32(buf + 20); gOff = rd32(buf + 24);

    if (rd16(buf + 4) != 1)   { log_puts("[log] font: unsupported version\r\n"); return 0; }
    if (bpp != 1)             { log_puts("[log] font: unsupported bpp\r\n"); return 0; }
    if (!cellH || !nw || !ww) { log_puts("[log] font: bad geometry\r\n"); return 0; }
    if (iOff + (UINTN)iCnt * 8 > len || gOff > len) { log_puts("[log] font: truncated\r\n"); return 0; }

    /* 索引必须严格升序 —— 二分查找的前提。这里校验一次，
     * 而不是让它在运行时静默返回错误结果。 */
    prev = 0;
    for (i = 0; i < iCnt; i++) {
        UINT32 cp = rd32(buf + iOff + i * 8);
        if (i && cp <= prev) {
            log_puts("[log] font: index not ascending at "); log_u64(i); log_puts("\r\n");
            return 0;
        }
        prev = cp;
    }

    /* 字形区大小 = 最后一项的偏移 + 它自己的长度（最后一项按最大宽度算） */
    if (iCnt) {
        UINT32 lastOff = rd32(buf + iOff + (iCnt - 1) * 8 + 4);
        UINT32 lastW   = (rd32(buf + iOff + (iCnt - 1) * 8) < 0x100) ? nw : ww;
        glyphBytes = lastOff + ((lastW + 7) >> 3) * cellH;
    }
    if (gOff + glyphBytes > len) { log_puts("[log] font: glyph data truncated\r\n"); return 0; }

    gData = buf;
    gHdr  = (const TNDF_HEADER *)buf;
    log_puts("[log] font: loaded "); log_puts(path);
    log_puts("  cell="); log_u64(cellH); log_puts("x"); log_u64(nw);
    log_puts("/"); log_u64(ww);
    log_puts("  glyphs="); log_u64(iCnt);
    log_puts("  bytes="); log_u64(len);
    log_puts("\r\n");
    return 1;
}

/* 按码点查字形。返回指向 1bpp 位图的指针；没有就返回 0。
 * 命中的宽度通过 outW 返回 —— 加载器需要知道它占几格。 */
const UINT8 *font_lookup(UINT32 cp, UINTN *outW) {
    UINT32 lo, hi;
    if (!gHdr) return 0;
    lo = 0; hi = gHdr->IndexCount;
    while (lo < hi) {
        UINT32 m  = lo + (hi - lo) / 2;
        UINT32 c  = rd32(gData + gHdr->IndexOffset + m * 8);
        if (c == cp) {
            UINTN w = (cp < 0x100) ? gHdr->NarrowW : gHdr->WideW;
            if (outW) *outW = w;
            return gData + gHdr->GlyphOffset + rd32(gData + gHdr->IndexOffset + m * 8 + 4);
        }
        if (c < cp) lo = m + 1; else hi = m;
    }
    if (outW) *outW = 0;
    return 0;
}
