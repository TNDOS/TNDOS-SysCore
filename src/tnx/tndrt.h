/* ============================================================================
 * TNDDOS-SDK -- TNX 程序运行时
 *
 * 你只需要写 tnx_main()。入口桩 tnx_entry() 在这个模块里：
 * 它把加载器传进来的 API 表存好，再调你的 tnx_main。
 * 所以程序里永远写 tnd_xxx(...)，**不需要知道 API 表从哪来** ——
 * 将来 API 从"函数表"换成真正的 SYSCALL，这一层不用改，你的代码更不用改。
 * ==========================================================================*/
#ifndef TNDRT_H
#define TNDRT_H

#include "tnd_api.h"

/* 用户必须实现这个 */
int tnx_main(void);

/* --- 底层：拿到 API 表本身（一般不用） --- */
const TND_API_TABLE *tnd_api(void);

/* --- 控制台 --- */
void tnd_puts(const char *s);
void tnd_putc(int c);
void tnd_putu(tnd_u64 v);
void tnd_putx(tnd_u64 v);

/* 精简 printf：支持 %s %c %d %u %x %X %% 和 %p（16 位十六进制）
 * 不支持宽度/精度/浮点 —— 工具够用，而且不拖一个完整的格式化库进来。
 * 单次格式化结果上限 512 字节。
 *
 * 注意：这里的 \n 会自动展开成 \r\n（ConOut 不做换行翻译）。
 * 要输出原样的字节请用 tnd_write()，别用这个。 */
void tnd_printf(const char *fmt, ...);

/* --- 程序环境 --- */
int         tnd_argc(void);
const char *tnd_argv(int i);
const char *tnd_env(const char *name);

/* --- 句柄 I/O --- */
int     tnd_open(const char *path, int flags);
int     tnd_close(int fd);
tnd_i64 tnd_read(int fd, void *buf, tnd_i64 count);
tnd_i64 tnd_write(int fd, const void *buf, tnd_i64 count);
tnd_i64 tnd_seek(int fd, tnd_i64 offset, int whence);

/* --- 文件系统 --- */
int tnd_unlink(const char *path);
int tnd_mkdir(const char *path);
int tnd_rmdir(const char *path);
int tnd_rename(const char *from, const char *to);
int tnd_stat(const char *path, TND_STAT *st);

/* --- 目录遍历 --- */
int tnd_findfirst(const char *pattern, TND_FIND *out);
int tnd_findnext(int fh, TND_FIND *out);
int tnd_findclose(int fh);

/* --- 内存 / 时间 --- */
void   *tnd_alloc(tnd_size n);
void    tnd_free(void *p);
tnd_u64 tnd_ticks(void);

/* --- 便捷函数（建立在上面之上） --- */

/* 从 fd 读一行（去掉换行），返回长度；0 = EOF。
 * 这是写工具时最常用的一个：配置文件、文本处理、交互输入都靠它。 */
tnd_i64 tnd_getline(int fd, char *buf, tnd_i64 cap);

/* 整个文件读进调用方给的缓冲（会补 0），返回实际长度，<0 = 错误。
 * 小文件够用；大文件请用 open/read 流式处理。 */
tnd_i64 tnd_readfile(const char *path, char *buf, tnd_i64 cap);

/* 从 stdin 读一个键（不等待回车），返回字符，<0 = 失败。 */
int tnd_getch(void);

/* --- 字符串小工具（不依赖 libc） --- */
tnd_size tnd_strlen(const char *s);
int      tnd_strcmp(const char *a, const char *b);
int      tnd_stricmp(const char *a, const char *b);
void     tnd_memzero(void *p, tnd_size n);

#endif /* TNDRT_H */
