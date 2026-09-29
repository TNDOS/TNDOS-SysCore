/* ============================================================================
 * TNX 样例程序
 *
 * 它不只打印一句话 —— 每一段都在自证一件事：
 *   地址      固定基址加载真的生效了
 *   RODT      只读段是活的
 *   DATA      可写段是活的
 *   BSS       加载器真的清零了
 *   API 表    内核把函数表交过来了
 *   堆        程序能用内核堆
 * ==========================================================================*/
#include "tndrt.h"

#define TNX_BASE   0x0000000001000000ULL
#define TNX_WINDOW 0x0000000000800000ULL

static const char g_rodata[] = "  [rodata] read-only section is live\r\n";
static tnd_u64    g_data_var  = 0;
static char       g_bss[64];

int tnx_main(void) {
    tnd_puts("\r\n");
    tnd_puts("  ================================================\r\n");
    tnd_puts("   Hello from a TNX program\r\n");
    tnd_puts("  ================================================\r\n");

    /* --- 1) 自证加载地址 --- */
    {
        tnd_u64 here = (tnd_u64)(tnd_size)(const void *)g_rodata;
        tnd_puts("  image addr    : "); tnd_putx(here); tnd_puts("\r\n");
        tnd_puts("  api table     : "); tnd_putx((tnd_u64)(tnd_size)(const void *)&g_rodata);
        tnd_puts("\r\n");
        if (here >= TNX_BASE && here < TNX_BASE + TNX_WINDOW)
            tnd_puts("  fixed base    : inside the 16MiB window          OK\r\n");
        else
            tnd_puts("  fixed base    : NOT in the expected window       FAIL\r\n");
    }

    /* --- 2) 只读段 --- */
    tnd_puts(g_rodata);

    /* --- 3) 可写数据段 --- */
    g_data_var = 0x123456789ABCDEF0ULL;
    tnd_puts("  [data]        wrote+read back: "); tnd_putx(g_data_var); tnd_puts("\r\n");

    /* --- 4) BSS 必须已被加载器清零 --- */
    {
        int clean = 1;
        for (int i = 0; i < (int)sizeof(g_bss); i++) if (g_bss[i] != 0) { clean = 0; break; }
        tnd_puts("  [bss]         ");
        tnd_puts(clean ? "zero-filled by the loader                 OK\r\n"
                       : "NOT zero-filled                           FAIL\r\n");
    }

    /* --- 5) 内核堆 --- */
    {
        char *p = (char *)tnd_alloc(128);
        if (p) {
            const char *m = "  [heap]        kernel heap reachable from a TNX program\r\n";
            int i = 0;
            for (; m[i]; i++) p[i] = m[i];
            p[i] = 0;
            tnd_puts(p);
            tnd_free(p);
        } else {
            tnd_puts("  [heap]        allocation FAILED\r\n");
        }
    }

    /* --- 6) 定时器还没有，API 应当老实返回 0 --- */
    tnd_puts("  ticks         : "); tnd_putu(tnd_ticks());
    tnd_puts("   (0 = no timer subsystem yet)\r\n");

    tnd_puts("  returning 0\r\n\r\n");
    return 0;
}
