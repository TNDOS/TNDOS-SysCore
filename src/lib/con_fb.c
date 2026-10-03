/* ============================================================================
 * 控制台后端：framebuffer
 *
 * 自己往帧缓冲里写像素，不再经过固件的 ConOut。
 *
 * **为什么这件事重要**：ConOut 是 Boot Services，ExitBootServices 之后它和它
 * 的字体一起消失。而帧缓冲那块**内存**还在。所以只要在 Exit 之前把
 * Base / Stride / 格式抄下来，再自带一套点阵字体，输出就完全属于我们了。
 *
 * 这一版只支持 32bpp。PixelBltOnly 直接拒绝 —— 那种模式下没有可写的
 * 线性帧缓冲，只能通过 Blt 画，我们不做。
 *
 * 光标是**静止**的（不闪）。闪烁需要定时器，而 ticks() 现在还是 0。
 * ==========================================================================*/
#include "tnd.h"

#define FB_CELL_W 8
#define FB_CELL_H 12

/* 影子缓冲里每个格子只有一个字节，装不下码点，所以约定三个**哨兵**：
 *   0x01 全角字符的左半   0x02 全角字符的右半   0x03 有字宽但还没字形
 * 代价是 CP437 的 0x01-0x03（三个笑脸）不显示了 —— 控制字符本来也不该打印。
 * 等 CJK 点阵字体接上来，0x03 会被真字形替掉，哨兵机制本身不用改。 */
#define FB_WIDE_L  0x01
#define FB_WIDE_R  0x02
#define FB_UNKNOWN 0x03

extern const UINT8  gFontVga[];
extern const UINTN  gFontGlyphWidth;
extern const UINTN  gFontGlyphHeight;

/* VGA 标准 16 色调色板。顺序和 DOS 属性字节的低 4 位 / 高 4 位完全对应。 */
static const UINT32 gPal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static struct {
    int      ok;
    UINT32  *fb;
    UINTN    stride;        /* 每扫描行像素数 */
    UINTN    width, height; /* 像素 */
    UINTN    cellW, cellH;  /* 缩放后的格子像素尺寸 */
    int      scalePct;      /* 0 = 自动 */
    UINTN    cols, rows;    /* 字符网格 */
    UINTN    x, y;          /* 光标所在格 */
    UINTN    attr;
    int      cursorOn;
    UINT8   *shadow;        /* 每格 2 字节：字符 + 属性 */
    EFI_GRAPHICS_PIXEL_FORMAT fmt;
    EFI_PIXEL_BITMASK masks;
} gFb;

/* 把一个 8 位分量按掩码放进目标位域 */
static UINT32 fb_cvt(UINT32 mask, UINT32 v) {
    UINT32 shift = 0, m = mask, bits = 0;
    if (!mask) return 0;
    while (!(m & 1)) { m >>= 1; shift++; }
    while (m & 1) { m >>= 1; bits++; }
    if (bits == 0) return 0;
    if (bits < 8) v >>= (8 - bits);
    else if (bits > 8) v <<= (bits - 8);
    return (v << shift) & mask;
}

static UINT32 fb_pixel(UINT32 rgb) {
    UINT32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    switch (gFb.fmt) {
    case PixelRedGreenBlueReserved8BitPerColor: return r | (g << 8) | (b << 16);
    case PixelBlueGreenRedReserved8BitPerColor: return b | (g << 8) | (r << 16);
    case PixelBitMask:
        return fb_cvt(gFb.masks.RedMask, r) | fb_cvt(gFb.masks.GreenMask, g) | fb_cvt(gFb.masks.BlueMask, b);
    default: return 0;
    }
}

static void fb_draw_cell(UINTN cx, UINTN cy) {
    UINTN off, px, py, r, c;
    UINT8 ch, attr;
    UINT32 fg, bg;
    const UINT8 *gl;

    if (!gFb.ok || cx >= gFb.cols || cy >= gFb.rows) return;
    off  = cy * gFb.cols + cx;
    ch   = gFb.shadow[off * 2];
    attr = gFb.shadow[off * 2 + 1];
    fg   = fb_pixel(gPal[attr & 0xF]);
    bg   = fb_pixel(gPal[(attr >> 4) & 0xF]);

    /* 字体只有 256 个字形（CP437）。超出范围画空格，不要越界读。 */
    px = cx * gFb.cellW;
    py = cy * gFb.cellH;

    /* 哨兵格：画一个占位框，而不是空白。
     * 空白看起来像"丢了字符"，框看起来像"这里有个字，只是还没有字形" ——
     * 调试时这两个的差别很大。全角的左右两半各画自己那一侧，合起来是一个框。 */
    if (ch >= FB_WIDE_L && ch <= FB_UNKNOWN) {
        for (r = 0; r < gFb.cellH; r++) {
            UINT32 *line = gFb.fb + (py + r) * gFb.stride + px;
            int top = (r == 0), bot = (r == gFb.cellH - 1);
            for (c = 0; c < gFb.cellW; c++) {
                int left  = (c == 0)               && (ch != FB_WIDE_R);
                int right = (c == gFb.cellW - 1)   && (ch != FB_WIDE_L);
                line[c] = (top || bot || left || right) ? fg : bg;
            }
        }
        return;
    }

    gl = gFontGlyphHeight ? (gFontVga + (UINTN)ch * gFontGlyphHeight) : 0;

    /* 缩放用定点最近邻：目标第 dy 行取源的第 dy*H/cellH 行。
     * 这样任意比例（1.5 也行）都能整除到位，不会因为四舍五入把笔画弄断。
     * 保证 gl 那一行取不到时按空行处理 —— 不然放大倍数一大就越界读。 */
    for (r = 0; r < gFb.cellH; r++) {
        UINTN sy = (r * gFontGlyphHeight) / gFb.cellH;
        UINT8 bits = (gl && sy < gFontGlyphHeight) ? gl[sy] : 0;
        UINT32 *line = gFb.fb + (py + r) * gFb.stride + px;
        for (c = 0; c < gFb.cellW; c++) {
            UINTN sx = (c * 8) / gFb.cellW;
            line[c] = (bits & (0x80 >> sx)) ? fg : bg;
        }
    }
}

/* 光标画成一格底下两条线（下划线）。不闪 —— 需要定时器。 */
static void fb_draw_cursor(int on) {
    UINTN px, py, r, c;
    UINT8 attr;
    UINT32 col;
    if (!gFb.ok || gFb.x >= gFb.cols || gFb.y >= gFb.rows) return;
    attr = gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2 + 1];
    col  = on ? fb_pixel(gPal[attr & 0xF]) : 0;
    if (!on) { fb_draw_cell(gFb.x, gFb.y); return; }
    px = gFb.x * gFb.cellW;
    py = gFb.y * gFb.cellH + gFb.cellH - 2;
    for (r = 0; r < 2; r++) {
        UINT32 *line = gFb.fb + (py + r) * gFb.stride + px;
        for (c = 0; c < gFb.cellW; c++) line[c] = col;
    }
}

static void fb_scroll(void) {
    UINTN rowbytes = gFb.cols * 2;
    if (gFb.rows < 2) return;
    t_memcpy(gFb.shadow, gFb.shadow + rowbytes, rowbytes * (gFb.rows - 1));
    t_memzero(gFb.shadow + rowbytes * (gFb.rows - 1), rowbytes);
    for (UINTN cy = 0; cy < gFb.rows; cy++)
        for (UINTN cx = 0; cx < gFb.cols; cx++) fb_draw_cell(cx, cy);
}

/* ------------------------------------------------------------------ 接口 */

/* fb_clear 定义在下面，但 fb_init 要用它（切换过来必须先擦掉固件画的旧像素）。
 * C99 不允许隐式声明，所以在这里先说明一下。 */
static void fb_clear(void);
static int  fb_apply_scale(int pct);
static void fb_draw_cell(UINTN cx, UINTN cy);

/* 换缩放比例。**网格尺寸变了，影子缓冲必须重分配** —— 不重分配就是缓冲区
 * 溢出，而且症状会是"屏幕上一部分正常、一部分是内存垃圾"。
 *
 * pct <= 0 表示自动：挑一个既放得下 80x25、又尽量大的比例（优先整数倍）。
 * **自动只能由 SF auto 显式触发，不是开机默认。** 默认恒为 100%。 */
static int fb_apply_scale(int pct) {
    UINTN w, h, cols, rows;

    if (pct <= 0) {
        /* **先试整数倍。** 点阵按整数倍放大最干净：每个源像素正好铺满 N×N。
         * 非整数倍（比如 210%）会出现"有的源行占 3 个目标行、有的只占 2 个"，
         * 笔画粗细不匀 —— 实测在 1280x800 上自动选到 210%，格子变成 16x25，
         * 就是这个毛病。而 200% 明明也满足 80x25。 */
        pct = -1;
        for (int s = 400; s >= 100; s -= 100) {
            UINTN cw = (FB_CELL_W * (UINTN)s) / 100, ch = (FB_CELL_H * (UINTN)s) / 100;
            if (!cw || !ch) continue;
            if (gFb.width / cw >= 80 && gFb.height / ch >= 25) { pct = s; break; }
        }
        /* 整数倍都放不下才退到 10% 步进 */
        if (pct < 0) {
            pct = 100;
            for (int s = 190; s >= 100; s -= 10) {
                UINTN cw = (FB_CELL_W * (UINTN)s) / 100, ch = (FB_CELL_H * (UINTN)s) / 100;
                if (!cw || !ch) continue;
                if (gFb.width / cw >= 80 && gFb.height / ch >= 25) { pct = s; break; }
            }
        }
    }
    if (pct < 25)  pct = 25;
    if (pct > 800) pct = 800;

    w = (FB_CELL_W * (UINTN)pct) / 100;
    h = (FB_CELL_H * (UINTN)pct) / 100;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    cols = gFb.width / w;
    rows = gFb.height / h;
    if (cols < 1 || rows < 1) { log_puts("[log] con_fb: scale too large\r\n"); return 0; }

    if (gFb.shadow) { gEnv.BS->FreePool(gFb.shadow); gFb.shadow = 0; }
    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, cols * rows * 2, (void **)&gFb.shadow)) || !gFb.shadow) {
        log_puts("[log] con_fb: out of memory for shadow\r\n");
        return 0;
    }
    gFb.cellW = w; gFb.cellH = h;
    gFb.cols = cols; gFb.rows = rows;
    gFb.scalePct = pct;
    gFb.x = gFb.y = 0;
    t_memzero(gFb.shadow, cols * rows * 2);
    fb_clear();
    return 1;
}

static int fb_init(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (gFb.ok) return 1;
    if (!gEnv.BS || !gEnv.BS->LocateProtocol) return 0;
    if (EFI_ERROR(gEnv.BS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (void **)&gop)) || !gop) {
        log_puts("[log] con_fb: no GOP\r\n");
        return 0;
    }
    if (!gop->Mode || !gop->Mode->Info) { log_puts("[log] con_fb: no mode info\r\n"); return 0; }
    info = gop->Mode->Info;

    if (info->PixelFormat == PixelBltOnly) {
        log_puts("[log] con_fb: PixelBltOnly is unsupported (no linear framebuffer)\r\n");
        return 0;
    }

    gFb.fb      = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFb.stride  = info->PixelsPerScanLine;
    gFb.width   = info->HorizontalResolution;
    gFb.height  = info->VerticalResolution;
    gFb.fmt     = info->PixelFormat;
    gFb.masks   = info->PixelInformation;

    if (!gFb.fb || gFb.stride < gFb.width || gFb.width < FB_CELL_W || gFb.height < FB_CELL_H) {
        log_puts("[log] con_fb: bad framebuffer geometry\r\n");
        return 0;
    }

    gFb.x = gFb.y = 0;
    gFb.attr = 0x07;
    gFb.cursorOn = 1;
    gFb.scalePct = 0;
    gFb.ok = 1;

    /* 网格、影子缓冲、清屏都在这里做 —— 切缩放时走的是同一条路。
     *
     * **默认恒为 100%，不自动放大。** 启动时安静地按原尺寸来，放大是用户
     * 显式要求的事（SF）。系统自己去判断该不该放大，是一种越权。 */
    if (!fb_apply_scale(100)) { gFb.ok = 0; return 0; }

    /* **必须清屏。**
     * 切换过来的时候帧缓冲上还留着固件 ConOut 画的旧像素，而我们是从
     * (0,0) 开始往上盖的 —— 不擦掉，旧内容就会从新内容下面露出来，
     * 屏幕上看起来像两套文字叠在一起。
     * 第一次实测就是这么栽的：截图里左边是新内容、右边是旧内容。 */
    fb_clear();

    /* 自检：把抄下来的值打出来，出问题时一眼能看出是哪个值不对 */
    log_puts("[log] con_fb: base="); log_hex(gop->Mode->FrameBufferBase);
    log_puts(" stride=");            log_u64(gFb.stride);
    log_puts(" size=");              log_u64(gFb.width); log_puts("x"); log_u64(gFb.height);
    log_puts(" fmt=");               log_u64((UINT64)info->PixelFormat);
    log_puts(" ppsl=");              log_u64(info->PixelsPerScanLine);
    log_puts(" grid=");              log_u64(gFb.cols); log_puts("x"); log_u64(gFb.rows);
    log_puts(" cell=");              log_u64(gFb.cellW); log_puts("x"); log_u64(gFb.cellH);
    log_puts(" scale=");             log_u64((UINT64)gFb.scalePct); log_puts("%\r\n");

    if (gFontGlyphHeight != FB_CELL_H || gFontGlyphWidth != FB_CELL_W) {
        log_puts("[log] con_fb: !! font is not 8x12 -- rendering will be wrong\r\n");
    }
    return 1;
}

/* 换行。全角字符放不下时也要用它 —— 全角不能跨行拆成两半。 */
static void fb_newline(void) {
    gFb.x = 0;
    if (++gFb.y >= gFb.rows) { gFb.y = gFb.rows - 1; fb_scroll(); }
}

/* 把一个码点放进当前格。
 * **这就是"宽字符"的全部**：字宽决定占几格，字体只决定长什么样。
 * 所以 CJK 点阵字体到位之前，排版就已经是对的了。 */
static void fb_put_cp(UINT32 cp) {
    int w = t_char_width(cp);

    if (w == 0) return;                 /* 组合字符：暂不叠加，也不占格 */

    if (w == 2) {
        /* 右边那一格放不下就整体换行 —— 全角字符被拆开比换行难看得多 */
        if (gFb.x + 1 >= gFb.cols) fb_newline();
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2]     = FB_WIDE_L;
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2 + 1] = (UINT8)gFb.attr;
        fb_draw_cell(gFb.x, gFb.y);
        gFb.x++;
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2]     = FB_WIDE_R;
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2 + 1] = (UINT8)gFb.attr;
        fb_draw_cell(gFb.x, gFb.y);
        gFb.x++;
    } else {
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2]     = (cp <= 0xFF) ? (UINT8)cp : FB_UNKNOWN;
        gFb.shadow[(gFb.y * gFb.cols + gFb.x) * 2 + 1] = (UINT8)gFb.attr;
        fb_draw_cell(gFb.x, gFb.y);
        gFb.x++;
    }

    if (gFb.x >= gFb.cols) fb_newline();
}

static void fb_write(const char *s, UINTN n) {
    UINTN i = 0;
    if (!gFb.ok || !s) return;
    fb_draw_cursor(0);
    while (i < n) {
        char c = s[i];
        UINT32 cp;

        if (c == '\r') { gFb.x = 0; i++; continue; }
        if (c == '\n') { fb_newline(); i++; continue; }

        /* ASCII 直接走，只有多字节才解码 —— 绝大多数输出是 ASCII，
         * 让快路径保持快的。 */
        if ((UINT8)c < 0x80) { cp = (UINT32)(UINT8)c; i++; }
        else {
            int len = t_utf8_decode(s + i, &cp);
            if (len < 1) len = 1;
            i += (UINTN)len;
        }
        fb_put_cp(cp);
    }
    fb_draw_cursor(gFb.cursorOn);
}

static void fb_clear(void) {
    if (!gFb.ok) return;
    for (UINTN i = 0; i < gFb.cols * gFb.rows; i++) {
        gFb.shadow[i * 2]     = ' ';
        gFb.shadow[i * 2 + 1] = (UINT8)gFb.attr;
    }
    for (UINTN cy = 0; cy < gFb.rows; cy++)
        for (UINTN cx = 0; cx < gFb.cols; cx++) fb_draw_cell(cx, cy);
    gFb.x = gFb.y = 0;
    fb_draw_cursor(gFb.cursorOn);
}

static void fb_gotoxy(UINTN x, UINTN y) {
    if (!gFb.ok) return;
    fb_draw_cursor(0);
    gFb.x = (x < gFb.cols) ? x : gFb.cols - 1;
    gFb.y = (y < gFb.rows) ? y : gFb.rows - 1;
    fb_draw_cursor(gFb.cursorOn);
}

/* 我们自己的光标位置。**和 ConOut 的 Mode 没有任何关系** —— 那边不知道我们画到哪。 */
static void fb_getxy(UINTN *x, UINTN *y) { if (x) *x = gFb.x; if (y) *y = gFb.y; }

/* ---------------------------------------------------------------- 批量原语
 * 这三个的价值都在同一件事上：**只重画被改动的那一小块**。
 * 逐格接口下，EDIT 重画一个 80x25 的窗口要发 2000 次调用，
 * 每次都可能触发一次全屏重绘 —— 那是性能灾难。 */

static void fb_redraw_region(UINTN x, UINTN y, UINTN w, UINTN h) {
    UINTN r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) fb_draw_cell(x + c, y + r);
}

/* 把矩形裁进屏幕。越界不报错，直接裁 —— 调用方少一堆边界判断。 */
static int fb_clip(UINTN *x, UINTN *y, UINTN *w, UINTN *h) {
    if (!gFb.ok || !*w || !*h) return 0;
    if (*x >= gFb.cols || *y >= gFb.rows) return 0;
    if (*x + *w > gFb.cols) *w = gFb.cols - *x;
    if (*y + *h > gFb.rows) *h = gFb.rows - *y;
    return *w && *h;
}

static void fb_write_cells(UINTN x, UINTN y, UINTN w, UINTN h, const TND_CELL *cells, UINTN stride) {
    UINTN r, c;
    if (!cells) return;
    if (!fb_clip(&x, &y, &w, &h)) return;
    fb_draw_cursor(0);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            UINTN o = (y + r) * gFb.cols + (x + c);
            gFb.shadow[o * 2]     = (UINT8)(cells[r * stride + c].Ch   & 0xFF);
            gFb.shadow[o * 2 + 1] = (UINT8)(cells[r * stride + c].Attr & 0xFF);
        }
    fb_redraw_region(x, y, w, h);
    fb_draw_cursor(gFb.cursorOn);
}

static void fb_fill(UINTN x, UINTN y, UINTN w, UINTN h, UINTN ch, UINTN attr) {
    UINTN r, c;
    if (!fb_clip(&x, &y, &w, &h)) return;
    fb_draw_cursor(0);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            UINTN o = (y + r) * gFb.cols + (x + c);
            gFb.shadow[o * 2]     = (UINT8)(ch & 0xFF);
            gFb.shadow[o * 2 + 1] = (UINT8)(attr & 0xFF);
        }
    fb_redraw_region(x, y, w, h);
    fb_draw_cursor(gFb.cursorOn);
}

/* 名字带 _region 是为了和上面那个整屏的 fb_scroll(void) 区分开。 */
static void fb_scroll_region(UINTN x, UINTN y, UINTN w, UINTN h, int dy, UINTN ch, UINTN attr) {
    UINTN r, c;
    if (!fb_clip(&x, &y, &w, &h)) return;
    if (dy == 0) return;
    fb_draw_cursor(0);

    /* **拷贝方向必须和 dy 的符号一致**，否则源行会被自己覆盖掉：
     *   dy > 0（内容上移）-> 目标行在上方 -> 从**上往下**搬
     *   dy < 0（内容下移）-> 目标行在下方 -> 从**下往上**搬
     * 搞反了就会出现"前半段正确、后半段是重复内容"。 */
    if (dy > 0) {
        for (r = 0; r < h; r++) {
            long src = (long)r + dy;
            for (c = 0; c < w; c++) {
                UINTN dst = (y + r) * gFb.cols + (x + c);
                if (src >= 0 && src < (long)h) {
                    UINTN s = (y + (UINTN)src) * gFb.cols + (x + c);
                    gFb.shadow[dst * 2]     = gFb.shadow[s * 2];
                    gFb.shadow[dst * 2 + 1] = gFb.shadow[s * 2 + 1];
                } else {
                    gFb.shadow[dst * 2]     = (UINT8)(ch & 0xFF);
                    gFb.shadow[dst * 2 + 1] = (UINT8)(attr & 0xFF);
                }
            }
        }
    } else {
        for (r = h; r-- > 0; ) {
            long src = (long)r + dy;
            for (c = 0; c < w; c++) {
                UINTN dst = (y + r) * gFb.cols + (x + c);
                if (src >= 0 && src < (long)h) {
                    UINTN s = (y + (UINTN)src) * gFb.cols + (x + c);
                    gFb.shadow[dst * 2]     = gFb.shadow[s * 2];
                    gFb.shadow[dst * 2 + 1] = gFb.shadow[s * 2 + 1];
                } else {
                    gFb.shadow[dst * 2]     = (UINT8)(ch & 0xFF);
                    gFb.shadow[dst * 2 + 1] = (UINT8)(attr & 0xFF);
                }
            }
        }
    }
    fb_redraw_region(x, y, w, h);
    fb_draw_cursor(gFb.cursorOn);
}

static int  fb_set_scale(int pct) { if (!gFb.ok) return 0; return fb_apply_scale(pct); }
static int  fb_get_scale(void)    { return gFb.scalePct ? gFb.scalePct : 100; }

static void fb_setattr(UINTN a) { gFb.attr = a & 0xFF; }
static void fb_cursor(int v)    { gFb.cursorOn = v ? 1 : 0; fb_draw_cursor(gFb.cursorOn); }
static UINTN fb_cols(void)      { return gFb.cols ? gFb.cols : 80; }
static UINTN fb_rows(void)      { return gFb.rows ? gFb.rows : 25; }

const TND_CONSOLE gConFb = {
    "fb", fb_init, fb_write, fb_clear, fb_gotoxy, fb_getxy,
    fb_setattr, fb_cursor, fb_cols, fb_rows, fb_set_scale, fb_get_scale,
    fb_write_cells, fb_fill, fb_scroll_region
};
