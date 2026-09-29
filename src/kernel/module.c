/* ============================================================================
 * TNDDOS -- kernel module framework
 *
 * Previously kernel.efi was one blob and dependency order was pure luck.
 * Now the separable kernel subsystems are modules: registered in one place,
 * initialised in order, each reporting its own status.
 * Order is dependency:
 *   vfs  ->  pmm  ->  heap  ->  drv  ->  conf
 * (conf needs vfs to read files; heap needs pmm for pages; drv loads images)
 * ==========================================================================*/
#include "tnd.h"

static const TND_MODULE_DEF *gMods[] = {
    &gModVfs,
    &gModPmm,
    &gModHeap,
    &gModDrv,
    &gModTnx,
    &gModConf
};

#define TND_MOD_COUNT ((int)(sizeof(gMods) / sizeof(gMods[0])))

static int gOk[TND_MAX_MODULES];
static int gCount = 0;
static int gFailed = 0;

static void pad(const char *s, int width) {
    UINTN n = t_strlen(s);
    con_puts(s);
    for (UINTN i = n; i < (UINTN)width; i++) con_puts(" ");
}

void module_init_all(void) {
    con_puts("\r\n  [Kernel modules]\r\n");
    log_puts("[log] module init begin\r\n");

    gCount = TND_MOD_COUNT;
    if (gCount > TND_MAX_MODULES) gCount = TND_MAX_MODULES;

    for (int i = 0; i < gCount; i++) {
        const TND_MODULE_DEF *m = gMods[i];
        int ok = m->init ? m->init() : 1;
        gOk[i] = ok;
        if (!ok) gFailed++;

        con_puts("    ");
        pad(m->name, 6);
        con_puts(ok ? "OK    " : "FAIL  ");
        con_puts(m->desc);
        con_puts("\r\n");

        log_puts("[log] module "); log_puts(m->name); log_puts(ok ? " OK\r\n" : " FAIL\r\n");
    }

    con_puts("    ");
    con_u64((UINT64)gCount); con_puts(" modules, "); con_u64((UINT64)gFailed);
    con_puts(" failed\r\n");
    log_kv_u64("module.count", (UINT64)gCount);
    log_kv_u64("module.failed", (UINT64)gFailed);
}

void module_report(void) {
    con_puts("\r\n  Kernel modules:\r\n");
    for (int i = 0; i < gCount; i++) {
        con_puts("    ");
        pad(gMods[i]->name, 8);
        con_puts(gOk[i] ? "OK     " : "FAIL   ");
        con_puts(gMods[i]->desc);
        con_puts("\r\n");
    }
}
