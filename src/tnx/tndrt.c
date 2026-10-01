/* ============================================================================
 * TNDDOS-SDK -- TNX 程序运行时（实现）
 * ==========================================================================*/
#include "tndrt.h"
#include <stdarg.h>   /* clang 自带的 freestanding 头，不需要 libc */

static const TND_API_TABLE *gApi = 0;

const TND_API_TABLE *tnd_api(void) { return gApi; }

/* ------------------------------------------------------------------ 入口桩
 * 加载器调的是这里，不是你的 tnx_main。它先把 API 表存下来，再交棒。 */
int tnx_entry(const TND_API_TABLE *api) {
    gApi = api;
    if (!api) return 127;
    if (api->Version < TND_API_VERSION) return 126;   /* 内核太老，宁可明确失败 */
    return tnx_main();
}

/* ------------------------------------------------------------------ 控制台 */
void tnd_puts(const char *s) { if (gApi && gApi->puts) gApi->puts(s ? s : ""); }
void tnd_putc(int c)         { if (gApi && gApi->putc) gApi->putc(c); }
void tnd_putu(tnd_u64 v)     { if (gApi && gApi->putu) gApi->putu(v); }
void tnd_putx(tnd_u64 v)     { if (gApi && gApi->putx) gApi->putx(v); }

/* ------------------------------------------------------- 精简 printf */
typedef struct { char *buf; tnd_i64 cap; tnd_i64 len; } OUT;

static void o_ch(OUT *o, char c) { if (o->len < o->cap - 1) o->buf[o->len] = c; o->len++; }

static void o_str(OUT *o, const char *s) {
    if (!s) s = "(null)";
    while (*s) o_ch(o, *s++);
}

static void o_uint(OUT *o, tnd_u64 v, int base, int upper, int pad) {
    char tmp[24]; int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    for (int i = n; i < pad; i++) o_ch(o, '0');
    while (n--) o_ch(o, tmp[n]);
}

static void o_int(OUT *o, tnd_i64 v) {
    if (v < 0) { o_ch(o, '-'); o_uint(o, (tnd_u64)(-v), 10, 0, 0); }
    else o_uint(o, (tnd_u64)v, 10, 0, 0);
}

void tnd_printf(const char *fmt, ...) {
    char buf[512];
    OUT o;
    va_list ap;
    o.buf = buf; o.cap = (tnd_i64)sizeof(buf); o.len = 0;

    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        /* 把 \n 补成 \r\n：UEFI 的 ConOut 不做换行翻译，
         * 而这地方全是给人看的格式化输出，没理由让每个工具自己写 \r\n。 */
        if (*p == '\n') { o_ch(&o, '\r'); o_ch(&o, '\n'); continue; }
        if (*p != '%') { o_ch(&o, *p); continue; }
        p++;
        switch (*p) {
        case 's': o_str(&o, va_arg(ap, const char *)); break;
        case 'c': o_ch(&o, (char)va_arg(ap, int)); break;
        case 'd': o_int(&o, (tnd_i64)va_arg(ap, int)); break;
        case 'u': o_uint(&o, (tnd_u64)va_arg(ap, unsigned int), 10, 0, 0); break;
        case 'x': o_uint(&o, (tnd_u64)va_arg(ap, unsigned int), 16, 0, 0); break;
        case 'X': o_uint(&o, (tnd_u64)va_arg(ap, unsigned int), 16, 1, 0); break;
        case 'p': o_ch(&o, '0'); o_ch(&o, 'x');
                  o_uint(&o, va_arg(ap, tnd_u64), 16, 1, 16); break;
        case '%': o_ch(&o, '%'); break;
        case 0:   p--; break;
        default:  o_ch(&o, '%'); o_ch(&o, *p); break;
        }
    }
    va_end(ap);

    if (o.len >= o.cap) o.len = o.cap - 1;
    buf[o.len] = 0;
    tnd_puts(buf);
}

/* ------------------------------------------------------------------ 环境 */
int         tnd_argc(void) { return (gApi && gApi->argc) ? gApi->argc() : 0; }
const char *tnd_argv(int i) { return (gApi && gApi->argv) ? gApi->argv(i) : ""; }
const char *tnd_env(const char *n) { return (gApi && gApi->env) ? gApi->env(n) : ""; }

/* ------------------------------------------------------------------ 句柄 */
int tnd_open(const char *p, int f) { return gApi->open(p, f); }
int tnd_close(int fd)              { return gApi->close(fd); }

tnd_i64 tnd_read(int fd, void *b, tnd_i64 n) {
    tnd_i64 got = 0;
    /* 底层的 read 不保证一次读满；工具期望"要么读够、要么到底" */
    while (got < n) {
        tnd_i64 r = gApi->read(fd, (char *)b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

tnd_i64 tnd_write(int fd, const void *b, tnd_i64 n) {
    tnd_i64 done = 0;
    while (done < n) {
        tnd_i64 w = gApi->write(fd, (const char *)b + done, n - done);
        if (w <= 0) break;
        done += w;
    }
    return done;
}

tnd_i64 tnd_seek(int fd, tnd_i64 off, int w) { return gApi->seek(fd, off, w); }

/* ------------------------------------------------------------ 文件系统 */
int tnd_unlink(const char *p) { return gApi->unlink(p); }
int tnd_mkdir(const char *p)  { return gApi->mkdir(p); }
int tnd_rmdir(const char *p)  { return gApi->rmdir(p); }
int tnd_rename(const char *a, const char *b) { return gApi->rename(a, b); }
int tnd_stat(const char *p, TND_STAT *st)    { return gApi->stat(p, st); }

/* ------------------------------------------------------------ 目录遍历 */
int tnd_findfirst(const char *pat, TND_FIND *o) { return gApi->findfirst(pat, o); }
int tnd_findnext(int fh, TND_FIND *o)           { return gApi->findnext(fh, o); }
int tnd_findclose(int fh)                       { return gApi->findclose(fh); }

/* ------------------------------------------------------------ 屏幕/键盘 */
void tnd_cls(void)              { if (gApi->cls) gApi->cls(); }
void tnd_gotoxy(int x, int y)   { if (gApi->gotoxy) gApi->gotoxy(x, y); }
int  tnd_getkey(void)           { return gApi->getkey(); }
int  tnd_cols(void)             { return gApi->cols ? gApi->cols() : 80; }
int  tnd_rows(void)             { return gApi->rows ? gApi->rows() : 25; }
void tnd_setattr(int attr)      { if (gApi->setattr) gApi->setattr(attr); }
int  tnd_getattr(void)          { return gApi->getattr ? gApi->getattr() : 0x07; }
void tnd_cursor(int visible)    { if (gApi->cursor) gApi->cursor(visible); }

/* ------------------------------------------------------------ 内存/时间 */
void   *tnd_alloc(tnd_size n) { return gApi->alloc(n); }
void    tnd_free(void *p)     { gApi->free(p); }
tnd_u64 tnd_ticks(void)       { return gApi->ticks(); }

/* ------------------------------------------------------------ 便捷函数 */
/* 读一行。返回值：-1 = EOF（调用方必须用它判断结束），
 * >= 0 = 行长（**空行返回 0**）。
 *
 * 这里曾经返回 0 表示 EOF —— 结果空行和文件结束分不开，
 * MORE 读到第一个空行就以为文件读完了。调用方要是写
 * "if (got <= 0) break" 就会撞上这个坑。 */
tnd_i64 tnd_getline(int fd, char *buf, tnd_i64 cap) {
    tnd_i64 n = 0;
    if (cap <= 0) return -1;
    for (;;) {
        char c;
        tnd_i64 r = gApi->read(fd, &c, 1);
        if (r <= 0) break;                       /* EOF 或错误 */
        if (c == '\n') { buf[n] = 0; return n; }
        if (c == '\r') continue;
        if (n < cap - 1) buf[n++] = c;
    }
    if (n == 0) return -1;                       /* 行首就碰到 EOF */
    buf[n] = 0;
    return n;
}

tnd_i64 tnd_readfile(const char *path, char *buf, tnd_i64 cap) {
    int fd = tnd_open(path, TND_O_RDONLY);
    tnd_i64 n;
    if (fd < 0) return -1;
    n = tnd_read(fd, buf, cap - 1);
    tnd_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

int tnd_getch(void) {
    char c;
    if (gApi->read(TND_STDIN, &c, 1) != 1) return -1;
    return (unsigned char)c;
}

/* ------------------------------------------------------------------ 字符串 */
tnd_size tnd_strlen(const char *s) { const char *p = s; if (!s) return 0; while (*p) p++; return (tnd_size)(p - s); }

int tnd_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int tnd_stricmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

void tnd_strncpy(char *dst, const char *src, tnd_size cap) {
    tnd_size i = 0;
    if (!cap) return;
    for (; i + 1 < cap && src && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void tnd_strcat(char *dst, const char *src, tnd_size cap) {
    tnd_size n = tnd_strlen(dst);
    if (n >= cap) return;
    tnd_strncpy(dst + n, src, cap - n);
}

int tnd_isdigit(int c) { return c >= '0' && c <= '9'; }

int tnd_atoi(const char *s) {
    int sign = 1, v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (tnd_isdigit((unsigned char)*s)) { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

void tnd_memzero(void *p, tnd_size n) {
    unsigned char *q = (unsigned char *)p;
    while (n--) *q++ = 0;
}
