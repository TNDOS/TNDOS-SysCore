#include "tndrt.h"

static const TND_API_TABLE *g_api = 0;

/* 入口桩：加载器按这个签名调用 */
int tnx_entry(const TND_API_TABLE *api) {
    g_api = api;
    return tnx_main();
}

void    tnd_puts(const char *s) { if (g_api && s) g_api->puts(s); }
void    tnd_putc(int c)         { if (g_api) g_api->putc(c); }
void    tnd_putu(tnd_u64 v)     { if (g_api) g_api->putu(v); }
void    tnd_putx(tnd_u64 v)     { if (g_api) g_api->putx(v); }
void   *tnd_alloc(tnd_size n)   { return g_api ? g_api->alloc(n) : 0; }
void    tnd_free(void *p)       { if (g_api) g_api->free(p); }
tnd_u64 tnd_ticks(void)         { return g_api ? g_api->ticks() : 0; }
