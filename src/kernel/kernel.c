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

/* ============================================================================
 * ExitBootServices —— **不可逆的分界点**
 *
 * 成功之后固件的一切服务全部失效：ConIn / Stall / AllocatePool / 文件系统 /
 * LoadImage…… 处理器从此完全归我们。
 *
 * 所以这不是「调一个系统调用」那么简单 —— 它要先把**退出之后还要活着的东西**
 * 安顿好。按顺序四件事：
 *
 *   1  **显示控制台切到 fb 后端。**
 *      默认后端是 uefi(ConOut)，退出后立刻死。con_fb 自己往线性帧缓冲写像素，
 *      但它的 Init（LocateProtocol 拿 GOP + AllocatePool 影子缓冲）
 *      **必须在退出之前做完** —— 退出后没法再找协议、也没法再分配。
 *
 *   2  **校准时基**（t_time_init）。
 *      它内部要一次 BS->Stall 来量 TSC 频率。退出后没有 Stall，而 t_delay_us
 *      到处都要用。**必须在退出前校好**，否则所有等待循环都只剩空转。
 *
 *   3  **重取内存表 + ExitBootServices，失败就重试。**
 *      MapKey 在任何分配之后都会失效，所以规范要求循环重取内存表直到成功。
 *      失败不是错误，是「期间有人分配过，再来一次」。
 *      每次取表要**多留两个描述符的余量** —— GetMemoryMap 自己也会占内存。
 *
 * 返回值：0 = 成功（此时 gBsGone = 1），-1 = 失败（Boot Services 仍然可用）。
 * ==========================================================================*/
int boot_exit(void) {
    EFI_STATUS st = EFI_DEVICE_ERROR;      /* 只是初值；循环里会被覆盖 */
    void  *map  = 0;
    UINTN  descSize = 0;
    UINT32 descVer = 0;
    int    tries;

    if (gBsGone) return 0;
    if (!gEnv.BS || !gEnv.BS->ExitBootServices) return -1;

    /* 1  显示控制台切到 fb 后端（它自己往帧缓冲写，不经过固件）。 */
    if (!con_select("fb")) {
        log_puts("[log] boot_exit: no fb console -- refusing to exit\r\n");
        return -1;
    }

    /* 2  时基必须在退出前校好。 */
    t_time_init();

    /* 3  循环重取 + 退出。 */
    for (tries = 0; tries < 8; tries++) {
        UINTN sz = 0, key = 0;

        gEnv.BS->GetMemoryMap(&sz, 0, &key, &descSize, &descVer);
        sz += descSize * 2;                      /* 余量：两次调用之间会变大 */
        map = 0;
        if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, sz, &map)) || !map) {
            map = 0;
            break;
        }
        st = gEnv.BS->GetMemoryMap(&sz, map, &key, &descSize, &descVer);
        if (!EFI_ERROR(st)) {
            st = gEnv.BS->ExitBootServices(gEnv.ImageHandle, key);
            if (!EFI_ERROR(st)) {
                gBsGone = 1;
                return 0;
            }
        }
        gEnv.BS->FreePool(map);
        map = 0;
    }
    if (map) gEnv.BS->FreePool(map);

    log_puts("[log] boot_exit: FAILED (boot services still alive)\r\n");
    return -1;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;
    int pmmOk = 0, heapOk = 0;

    gEnv.ImageHandle = ImageHandle;
    gEnv.ST = SystemTable;
    gEnv.BS = SystemTable->BootServices;
    /* 内核也可能被固件**直接**启动（不走 BOOTX64.EFI），所以这里也要关一次。
     * 关两次是无害的。 */
    t_disable_watchdog();
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
