/* ============================================================================
 * hello -- SDK 的活体自检
 *
 * 这个程序存在的唯一意义是**在真机上证明 API v2 是活的**。
 * 它按顺序验证：固定基址、RODT、DATA、BSS、内核堆、
 * argv、目录遍历、文件读取、stat。每一项都当场判定并报 OK/FAIL。
 * ==========================================================================*/
#include "tndrt.h"

static int fails = 0;

static void check(const char *what, int ok) {
    tnd_printf("  %s  ->  %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static tnd_u32 gData = 0x11111111u;      /* DATA 段 */
static tnd_u32 gBss;                     /* BSS，加载器必须清零 */
static const char gRod[] = "[rodata] read-only section is live";   /* RODT */

int tnx_main(void) {
    TND_FIND f;
    TND_STAT st;
    int fh, found = 0, fd;
    char buf[256];

    tnd_puts("\r\n");
    tnd_puts("  ================================================\r\n");
    tnd_puts("   Hello from a TNX program (API v2)\r\n");
    tnd_puts("  ================================================\r\n");

    /* --- 固定基址 --- */
    {
        tnd_u64 here = (tnd_u64)(tnd_size)&gData;
        tnd_printf("  image addr    : %p\n", here);
        check("fixed base inside 16MiB window", here >= 0x1000000ULL && here < 0x1800000ULL);
    }

    /* --- 段 --- */
    tnd_printf("  %s\n", gRod);
    gData = 0xABCDEF01u;
    check("data section read/write", gData == 0xABCDEF01u);
    check("bss zero-filled by loader", gBss == 0);

    /* --- 堆 --- */
    {
        char *p = (char *)tnd_alloc(128);
        int ok = 0;
        if (p) { p[0] = 'X'; p[127] = 'Y'; ok = (p[0] == 'X' && p[127] == 'Y'); tnd_free(p); }
        check("kernel heap reachable", ok);
    }

    /* --- argv --- */
    {
        int ac = tnd_argc();
        tnd_printf("  argc = %d", ac);
        if (ac > 0) tnd_printf("   argv[0] = %s", tnd_argv(0));
        tnd_printf("\n");
        check("argv is wired up", ac >= 1 && tnd_strlen(tnd_argv(0)) > 0);
    }

    /* --- 目录遍历 --- */
    fh = tnd_findfirst("*.TNX", &f);
    if (fh < 0) {
        check("findfirst(*.TNX)", 0);
    } else {
        tnd_printf("  TNX files in current dir:\n");
        do {
            tnd_printf("      %s  (%u bytes)\n", f.Name, f.Size);
            found++;
        } while (tnd_findnext(fh, &f) == 0);
        tnd_findclose(fh);
        check("findfirst/findnext/findclose", found > 0);
    }

    /* --- stat --- */
    check("stat(HELLO.TNX)", tnd_stat("HELLO.TNX", &st) == 0 && st.Size > 0);
    if (st.Size) tnd_printf("  HELLO.TNX size = %u bytes\n", st.Size);

    /* --- 文件读取 --- */
    fd = tnd_open("AUTOEXEC.BAT", TND_O_RDONLY);
    if (fd < 0) {
        check("open(AUTOEXEC.BAT)", 0);
    } else {
        tnd_i64 n = tnd_read(fd, buf, sizeof(buf) - 1);
        tnd_close(fd);
        if (n > 0) buf[n] = 0;
        tnd_printf("  read %d bytes from AUTOEXEC.BAT; first line: ", (int)(n > 0 ? n : 0));
        if (n > 0) {
            for (int i = 0; i < n && buf[i] != '\n' && buf[i] != '\r'; i++) tnd_putc(buf[i]);
        }
        tnd_printf("\n");
        check("open/read/close a real file", n > 0);
    }

    /* --- 时间 --- */
    tnd_printf("  ticks         : %u   (0 = no timer subsystem yet)\n", tnd_ticks());

    tnd_printf("\n  %s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
