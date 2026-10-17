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
 * ExitBootServices 之后的 Boot Services 桩表
 *
 * 退出之后，固件的服务表**和它指向的代码**全都没了 —— 那块内存已经还给系统。
 *
 * 我们之前只是把 gBsGone 置上，却没管 gEnv.BS 还指着那块被回收的内存。
 * 于是**任何一条漏掉的路径都会跳进去**：实测敲 DIR，
 *
 *     *** CPU EXCEPTION ***  vector 13  general protection
 *     RIP : 0x0000000000001001
 *
 * —— 一个既不在内核也不在驱动窗口里的离谱地址。
 *
 * **知道会失败的东西，应该干净地失败，而不是炸掉。**
 * 把整张表换成「什么都说 Unsupported」之后：
 *
 *   DIR / TYPE / FIND  -> VFS 报「找不到文件系统」，shell 继续活着
 *   其它漏网路径        -> 拿到一个干净的失败返回值
 *
 * 一路可查，比只剩一行地址强得多。
 * ==========================================================================*/
static EFI_STATUS gBsStubFn(void) { return EFI_UNSUPPORTED; }
static EFI_BOOT_SERVICES gStubBs;

/* ---------------------------------------------------------------- 能兑现的三格
 *
 * 桩表里大部分槽只能返回「不支持」—— 固件的代码真的没了，我们没法凭空造出
 * 一个 LoadImage 或者一个 SimpleFileSystem。
 *
 * 但**有三格我们真的能兑现**，把它们换成自己的实现，退出之后所有调用点
 * 自动转到我们这边，**一个调用点都不用改**：
 *
 *   AllocatePool  ->  kmalloc      （内核本来就有堆）
 *   FreePool      ->  kfree
 *   Stall         ->  t_delay_us   （已校准的 TSC）
 *
 * 这就是「写个模块在 exitboot 之后把空缺填上」：缺口不是靠改几十处代码补的，
 * 是靠**在这张表上换几个函数指针**补的。剩下的真补不了的（LoadImage、
 * LocateHandleBuffer、GetMemoryMap……）才如实返回不支持。
 *
 * 顺序也有意义：**这三格必须能兑现，否则内核自己就没法分配内存** ——
 * 而文件系统、目录枚举全都要分配。它们一塌，后面全完。 */
static EFI_STATUS EFIAPI bs_pool_alloc(UINT32 type, UINTN size, void **out) {
    void *p;
    (void)type;
    if (!out) return EFI_UNSUPPORTED;
    p = kmalloc(size ? (UINTN)size : 1);
    if (!p) return EFI_UNSUPPORTED;
    *out = p;
    return 0;
}
static EFI_STATUS EFIAPI bs_pool_free(void *p) { if (p) kfree(p); return 0; }
static EFI_STATUS EFIAPI bs_stall(UINTN us) { t_delay_us((UINT64)us); return 0; }

static void bs_install_takeover(void) {
    void **slot  = (void **)&gStubBs;
    UINTN   n    = sizeof(gStubBs) / sizeof(void *);
    /* Hdr 是 24 字节的数据（不是函数指针），前 3 个槽要跳过 */
    UINTN   first = sizeof(EFI_TABLE_HEADER) / sizeof(void *);
    UINTN   i;

    for (i = 0; i < n; i++) slot[i] = 0;
    for (i = first; i < n; i++) slot[i] = (void *)&gBsStubFn;

    /* **这三格我们真的能兑现** —— 换成真的。 */
    gStubBs.AllocatePool = bs_pool_alloc;
    gStubBs.FreePool     = bs_pool_free;
    gStubBs.Stall        = bs_stall;

    gEnv.BS = &gStubBs;

    log_puts("[log] takeover: AllocatePool/FreePool/Stall are ours now\r\n");
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

    /* **每一步都记一行。** 这个函数卡住过一次，而里面一行日志都没有 ——
     * 结果只能猜。加了这些之后，卡在哪一步是串口上直接看得到的。 */
    log_puts("[log] boot_exit: begin\r\n");

    /* 1  显示控制台切到 fb 后端（它自己往帧缓冲写，不经过固件）。 */
    if (!con_select("fb")) {
        log_puts("[log] boot_exit: no fb console -- refusing to exit\r\n");
        return -1;
    }
    log_puts("[log] boot_exit: console is ours (fb)\r\n");

    /* 2  时基必须在退出前校好。 */
    t_time_init();
    log_puts("[log] boot_exit: timer ready\r\n");

    /* 3  循环重取 + 退出。 */
    for (tries = 0; tries < 8; tries++) {
        UINTN sz = 0, key = 0;

        log_puts("[log] boot_exit: try "); log_u64((UINT64)tries); log_puts("\r\n");
        gEnv.BS->GetMemoryMap(&sz, 0, &key, &descSize, &descVer);
        log_puts("[log] boot_exit:   mapsize="); log_u64(sz);
        log_puts(" descsize="); log_u64(descSize); log_puts("\r\n");
        sz += descSize * 2;                      /* 余量：两次调用之间会变大 */
        map = 0;
        if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, sz, &map)) || !map) {
            log_puts("[log] boot_exit:   AllocatePool FAILED\r\n");
            map = 0;
            break;
        }
        st = gEnv.BS->GetMemoryMap(&sz, map, &key, &descSize, &descVer);
        log_puts("[log] boot_exit:   getmap st="); log_u64(st);
        log_puts(" key="); log_u64(key); log_puts("\r\n");
        if (!EFI_ERROR(st)) {
            log_puts("[log] boot_exit:   calling ExitBootServices...\r\n");
            st = gEnv.BS->ExitBootServices(gEnv.ImageHandle, key);
            log_puts("[log] boot_exit:   EBS returned st="); log_u64(st); log_puts("\r\n");
            if (!EFI_ERROR(st)) {
                gBsGone = 1;
                /* **把服务表换成桩。** 这一步必须在成功之后做 ——
                 * 上面那个重试循环还要用真的 BS。 */
                log_puts("[log] boot_exit:   success -- installing takeover table\r\n");
                bs_install_takeover();
                log_puts("[log] boot_exit:   takeover table installed\r\n");
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
