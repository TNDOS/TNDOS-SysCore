/* ============================================================================
 * TNDDOS kernel entry -- kernel.efi
 * Positioned like IO.SYS in MS-DOS.
 *
 * Boot order:
 *   1) set up runtime environment / logging
 *   2) locate the ESP
 *   3) init kernel modules in dependency order (vfs -> pmm -> heap -> drv -> conf)
 *   4) subsystem self-tests: PMM page read/write, heap alloc/free
 *   5) efidos.sys (system config, first) -> config.sys (user config)
 *   6) start the Shell, which runs autoexec.bat last
 * ==========================================================================*/
#include "tnd.h"

static void kernel_banner(void) {
    con_puts("\r\n");
    con_puts("  ============================================================\r\n");
    con_puts("   " TND_NAME "  (" TND_ALIAS ")   Version " TND_VERSION "\r\n");
    con_puts("   UEFI DOS  --  second generation DOS\r\n");
    con_puts("  ============================================================\r\n");
}

static void show_chain(void) {
    con_puts("\r\n  [Boot chain]\r\n");
    con_puts("    UEFI Firmware\r\n");
    con_puts("      -> \\EFI\\BOOT\\BOOTX64.EFI   (initializer, ~PBR)\r\n");
    con_puts("      -> \\EFI\\TNDOS\\kernel.efi   (kernel, ~IO.SYS)   <== now\r\n");
    con_puts("      -> \\EFI\\TNDOS\\efidos.sys   (system config, first)\r\n");
    con_puts("      -> \\EFI\\TNDOS\\config.sys   (user config)\r\n");
    con_puts("      -> \\EFI\\TNDOS\\autoexec.bat (boot script, last)\r\n");
    con_puts("      -> Shell\r\n");
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;
    int pmmOk = 0, heapOk = 0;

    gEnv.ImageHandle = ImageHandle;
    gEnv.ST = SystemTable;
    gEnv.BS = SystemTable->BootServices;
    gEnv.RT = SystemTable->RuntimeServices;
    gEnv.Root = 0;
    gEnv.EspHandle = 0;

    log_init(SystemTable);
    log_puts("[log] === kernel.efi entry ===\r\n");
    log_kv("kernel.version", TND_VERSION);

    kernel_banner();
    show_chain();

    con_puts("\r\n  Locating EFI System Partition ...\r\n");
    s = t_open_root(ImageHandle);
    if (EFI_ERROR(s)) {
        con_puts("  FAILED: kernel has no ESP, cannot continue.\r\n");
        log_kv("kernel", "t_open_root FAILED");
        return s;
    }
    con_puts("  OK\r\n");

    /* ---- 3) init kernel modules in order ---- */
    module_init_all();

    /* ---- 4) subsystem self-tests ---- */
    con_puts("\r\n  [Subsystem self-test]\r\n");

    con_puts("\r\n  -- Physical memory manager --\r\n");
    pmm_report();
    con_puts("\r\n  Running PMM self-test ...\r\n");
    pmmOk = pmm_selftest();

    con_puts("\r\n  -- Kernel heap --\r\n");
    heap_report();
    con_puts("\r\n  Running heap self-test ...\r\n");
    heapOk = heap_selftest();

    con_puts("\r\n  -- Mounted drives --\r\n");
    vfs_report();

    con_puts("\r\n  Self-test result: PMM ");
    con_puts(pmmOk ? "PASS" : "FAIL");
    con_puts("   heap ");
    con_puts(heapOk ? "PASS" : "FAIL");
    con_puts("\r\n");
    log_puts("[log] selftest pmm="); log_puts(pmmOk ? "PASS" : "FAIL");
    log_puts(" heap="); log_puts(heapOk ? "PASS" : "FAIL"); log_puts("\r\n");

    /* ---- 5) configuration ---- */
    conf_parse(TND_EFIDOS_SYS, 1);   /* system config: deployed at install time, loaded first */
    conf_parse(TND_CONFIG_SYS, 0);   /* user config */

    /* ---- 5.5) driver load results (DEVICE= already ran during config parsing) ---- */
    drv_report();

    /* ---- 5.6) TNX image window ---- */
    tnx_report();

    con_puts("\r\n");
    env_dump();

    /* ---- 6) Shell (it runs autoexec.bat as its last step) ---- */
    shell_start();

    con_puts("\r\n  kernel.efi exiting.\r\n");
    log_kv("kernel", "exit");
    return EFI_SUCCESS;
}
