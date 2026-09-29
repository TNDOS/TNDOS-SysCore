/* ============================================================================
 * TNX 最小运行时（SDK 的雏形）
 *
 * 加载器把 API 表作为参数传给入口，这里把它存下来，
 * 让用户代码可以永远只写 tnd_puts(...)，不必关心表从哪来。
 * 将来 API 换成 syscall，这一层不用改，用户代码更不用改。
 * ==========================================================================*/
#ifndef TNDRT_H
#define TNDRT_H

#include "tnd_api.h"

int  tnx_main(void);          /* 用户代码提供；入口桩会调它 */

void    tnd_puts(const char *s);
void    tnd_putc(int c);
void    tnd_putu(tnd_u64 v);
void    tnd_putx(tnd_u64 v);
void   *tnd_alloc(tnd_size n);
void    tnd_free(void *p);
tnd_u64 tnd_ticks(void);

#endif /* TNDRT_H */
