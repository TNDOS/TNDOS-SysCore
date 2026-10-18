/* ============================================================================
 * TNDDOS Shell
 * Built into kernel.efi for now; per the plan it becomes a separate user program.
 *
 * Commands:
 *   HELP VER CLS ECHO SET EXIT
 *   DIR CD MD RD TYPE DEL REN COPY VOL
 *   MEM MEMTEST MODULES DRIVERS LOAD REBOOT SHUTDOWN
 * It also runs autoexec.bat, the last step of the boot chain.
 *
 * All output is ASCII on purpose: the console must be readable on any firmware
 * font. Localization is a later layer, not hardcoded strings.
 * ==========================================================================*/
#include "tnd.h"

static int sEcho = 1;      /* batch @echo switch, on by default */

/* ------------------------------------------------------- %VAR% expansion */
static void expand_vars(const char *in, char *out, UINTN cap) {
    UINTN o = 0;
    for (UINTN i = 0; in[i] && o + 1 < cap; ) {
        if (in[i] == '%') {
            UINTN j = i + 1;
            while (in[j] && in[j] != '%') j++;
            if (in[j] == '%') {
                char name[64]; UINTN n = j - i - 1;
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                for (UINTN k = 0; k < n; k++) name[k] = in[i + 1 + k];
                name[n] = 0;
                if (n == 0) { i = j + 1; continue; }
                const char *v = env_get(name);
                if (v) { for (UINTN k = 0; v[k] && o + 1 < cap; k++) out[o++] = v[k]; }
                i = j + 1;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

/* split the argument into two tokens */
static void split2(char *arg, char *a, UINTN capa, char *b, UINTN capb) {
    char *p = t_skip_ws(arg);
    UINTN i = 0;
    while (*p && !t_is_space(*p) && i + 1 < capa) a[i++] = *p++;
    a[i] = 0;
    p = t_skip_ws(p);
    i = 0;
    while (*p && !t_is_space(*p) && i + 1 < capb) b[i++] = *p++;
    b[i] = 0;
}

/* ------------------------------------------------------------- MEM */
static void cmd_mem(void) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN size = 0, key = 0, dsize = 0;
    UINT32 dver = 0;
    EFI_STATUS s;
    UINT64 freePages = 0, bootPages = 0, rtPages = 0;
    UINTN count = 0;

    con_puts("\r\n  === UEFI memory map (numbers come from the firmware) ===\r\n");
    s = gEnv.BS->GetMemoryMap(&size, 0, &key, &dsize, &dver);
    size += 4096;
    if (!EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, size, (void **)&map)) && map) {
        s = gEnv.BS->GetMemoryMap(&size, map, &key, &dsize, &dver);
        if (!EFI_ERROR(s)) {
            count = dsize ? (size / dsize) : 0;
            for (UINTN i = 0; i < count; i++) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * dsize);
                switch (d->Type) {
                    case EfiConventionalMemory: freePages += d->NumberOfPages; break;
                    case EfiLoaderCode: case EfiLoaderData:
                    case EfiBootServicesCode: case EfiBootServicesData:
                        bootPages += d->NumberOfPages; break;
                    case EfiRuntimeServicesCode: case EfiRuntimeServicesData:
                        rtPages += d->NumberOfPages; break;
                    default: break;
                }
            }
        }
        gEnv.BS->FreePool(map);
    }
    con_kv("descriptors", count, 0);
    con_kv("conventional", freePages * 4 / 1024, "MB");
    con_kv("used by UEFI", bootPages * 4 / 1024, "MB");
    con_kv("firmware reserved", rtPages * 4 / 1024, "MB");

    con_puts("\r\n  === TNDDOS physical memory manager (PMM) ===\r\n");
    pmm_report();
    con_puts("\r\n  === Kernel heap ===\r\n");
    heap_report();
}

static void cmd_memtest(void) {
    int a = pmm_selftest();
    int b = heap_selftest();
    con_puts("\r\n  Memory self-test:\r\n");
    con_puts("    PMM   page alloc / write / read back / free : ");
    con_puts(a ? "PASS\r\n" : "FAIL\r\n");
    con_puts("    HEAP  kmalloc / isolation / kfree / reuse   : ");
    con_puts(b ? "PASS\r\n" : "FAIL\r\n");
    con_puts("    result: ");
    con_puts((a && b) ? "memory subsystem is healthy\r\n" : "memory subsystem has problems\r\n");
    log_kv("memtest", (a && b) ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------ HELP */
static void cmd_help(void) {
    con_puts("\r\n");
    con_puts("  TNDDOS built-in commands\r\n");
    con_puts("    HELP / VER / CLS / ECHO <text> / SET [NAME=VALUE] / EXIT\r\n");
    con_puts("    DIR [path]        list a directory\r\n");
    con_puts("    CD [path]         change directory (supports .. and drive letters)\r\n");
    con_puts("    MD / RD <dir>     make / remove a directory\r\n");
    con_puts("    TYPE <file>       print a text file\r\n");
    con_puts("    DEL <file>        delete a file\r\n");
    con_puts("    REN <old> <new>   rename (UEFI has no rename: copy then delete)\r\n");
    con_puts("    COPY <src> <dst>  copy a file\r\n");
    con_puts("    VOL               show mounted drives\r\n");
    con_puts("    PCI               list PCI devices found via ACPI ECAM\r\n");
    con_puts("    KEYS              dump raw key events (input backend debug)\r\n");
    con_puts("    MEM               memory overview (UEFI map + PMM + heap)\r\n");
    con_puts("    MEMTEST           run memory self-tests\r\n");
    con_puts("    MODULES           list kernel modules and status\r\n");
    con_puts("    DRIVERS           list loaded drivers\r\n");
    con_puts("    CONSOLE [name]    list or switch console backend (uefi / fb)\r\n");
    con_puts("    SF [n]            console scale: SF 1.5 = 150%, SF 200 = 200%\r\n");
    con_puts("    LOAD <file>       load a UEFI image (~= load fs0:\\<file>)\r\n");
    con_puts("    <prog> [args]     run a TNX program by name (extension optional)\r\n");
    con_puts("  \r\n  Editing: Up/Down = command history, Left/Right/Home/End = move, Del = delete\r\n");
    con_puts("    TNX <file.tnx>    dump header + section table, do NOT run it\r\n");
    con_puts("    TNXRUN <file>     run it with full loader trace\r\n");
    con_puts("    REBOOT / SHUTDOWN reset / power off (UEFI ResetSystem)\r\n");
    con_puts("\r\n");
}

static void cmd_ver(void) {
    con_puts("\r\n");
    con_puts("  " TND_NAME " (" TND_ALIAS ")  Version " TND_VERSION "\r\n");
    con_puts("  UEFI x86-64 / PE transitional / no BIOS, no 16-bit\r\n");
    con_puts("  Keeps the DOS interaction model, drops the DOS memory model\r\n");
    con_puts("\r\n");
}

/* ------------------------------------------------------- run one line */
/* ------------------------------------------------------------ 程序启动
 * 把命令行尾巴切成 argv。v1 只按空白切，不支持引号 —— 工具够用。
 * argv[0] 是用户敲的名字本身（DOS 的规矩），不是解析后的完整路径，
 * 这样 usage 消息里显示的是短名。 */
static int make_argv(const char *prog, const char *arg, char *store, UINTN cap, char **argv, int maxArgv) {
    UINTN used = 0;
    int n = 0;
    const char *s;

    if (n < maxArgv) {
        argv[n++] = store;
        while (prog && *prog && used + 1 < cap) store[used++] = *prog++;
        store[used++] = 0;
    }

    s = arg;
    while (s && *s && n < maxArgv) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (used + 1 >= cap) break;
        argv[n] = store + used;
        while (*s && *s != ' ' && *s != '\t' && used + 1 < cap) store[used++] = *s++;
        store[used++] = 0;
        n++;
    }
    return n;
}

/* 拼一个带扩展名的候选名（原名字里已经有 '.' 就不动）。 */
static void with_ext(const char *name, const char *ext, char *out, UINTN cap) {
    for (const char *p = name; *p; p++) {
        if (*p == '.') { t_strncpy(out, name, cap); return; }
    }
    t_strncpy(out, name, cap);
    t_strncpy(out + t_strlen(out), ext, cap - t_strlen(out));
}

/* 找程序、装参数、跑。返回 1 = 处理过了，0 = 什么都没找到。
 *
 * 搜索顺序就是 DOS 的规矩：
 *     <name>.TNX  ->  <name>.EXE  ->  <name>.COM
 *
 * .EXE / .COM 在长模式下跑不了 —— 但那不该是一句 "Bad command"，
 * 而该是一次说明。所以交给 EXECOM 去解释（见 EXECOM 的文档）。
 * EXECOM 不在时退回到 Shell 自带的兜底消息，免得用户把 EXECOM 删了就变成哑巴。 */
static int run_program(const char *cmd, const char *arg) {
    static const char *order[3] = { ".TNX", ".EXE", ".COM" };
    char prog[TND_MAX_PATH];
    char store[512];
    char *av[9];
    int ac, len, i;

    len = (int)t_strlen(cmd);

    for (i = 0; i < 3; i++) {
        char nm[TND_MAX_PATH];

        /* 用户自己写了扩展名（而且是四个字符的）就只试他写的那个 ——
         * 否则敲 PEDEMO.EXE 会先被当成 TNX 去找，找到那个 .EXE 文件、
         * 然后报 "bad magic"，永远走不到 EXECOM。 */
        if (len > 4 && cmd[len - 4] == '.') {
            if (t_stricmp(cmd + len - 4, order[i]) != 0) continue;
            t_strncpy(nm, cmd, sizeof(nm));
        } else {
            with_ext(cmd, order[i], nm, sizeof(nm));
        }

        if (!tnx_find(nm, prog, sizeof(prog))) continue;

        if (i == 0) {
            ac = make_argv(cmd, arg, store, sizeof(store), av, 9);
            tnx_run(prog, ac, av, 0);
        } else {
            char execom[TND_MAX_PATH];
            if (tnx_find("EXECOM", execom, sizeof(execom))) {
                static char ename[] = "EXECOM";
                av[0] = ename;
                av[1] = prog;
                tnx_run(execom, 2, av, 0);
            } else {
                con_puts("  "); con_puts(nm); con_puts(": a DOS/Windows executable.\r\n");
                con_puts("  Long mode cannot run 16-bit code, and TNDDOS does not load PE images.\r\n");
                con_puts("  (Put EXECOM.TNX next to it for a full diagnosis.)\r\n");
            }
        }
        return 1;
    }
    return 0;
}

/* 解析缩放参数：
 *   "1.5"  -> 150%   （有小数点，当倍率）
 *   "200"  -> 200%   （没小数点且 > 8，当百分比）
 *   "2"    -> 200%   （没小数点且 <= 8，当倍率）
 * 最后一条是有意的：SF 2 想说 200%，不是 2%。 */
static int parse_scale(const char *s) {
    int whole = 0, frac = 0, div = 1, hasDot = 0, any = 0;
    while (*s == ' ' || *s == 'x' || *s == 'X') s++;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') {
        hasDot = 1; s++;
        while (*s >= '0' && *s <= '9' && div < 100) { frac = frac * 10 + (*s - '0'); div *= 10; s++; any = 1; }
    }
    if (!any) return -1;
    if (hasDot) return whole * 100 + (frac * 100) / div;
    return (whole <= 8) ? whole * 100 : whole;
}

void shell_exec_line(char *line) {
    char cmd[32];
    UINTN i = 0;
    char *p = t_skip_ws(line);
    char *arg;

    if (!*p) return;
    if (*p == ';' || *p == '#') return;
    while (p[i] && !t_is_space(p[i]) && i + 1 < sizeof(cmd)) { cmd[i] = p[i]; i++; }
    cmd[i] = 0;
    arg = t_skip_ws(p + i);

    if (!t_stricmp(cmd, "REM")) return;

    /* DOS 的规矩：敲 "D:" 就切到 D 盘 */
    if (vfs_try_chdrive(cmd)) return;

    /* classic DOS: ECHO. / ECHO: / ECHO/ all print a blank line */
    if (!t_strnicmp(cmd, "ECHO", 4) && (cmd[4] == '.' || cmd[4] == ':' || cmd[4] == '/') && !cmd[5]) {
        con_puts("\r\n"); return;
    }

    if (!t_stricmp(cmd, "HELP")) { cmd_help(); return; }
    if (!t_stricmp(cmd, "VER"))  { cmd_ver();  return; }
    if (!t_stricmp(cmd, "CLS"))  { con_clear(); return; }

    if (!t_stricmp(cmd, "ECHO")) {
        char ex[512];
        if (!t_stricmp(arg, "OFF")) { sEcho = 0; return; }
        if (!t_stricmp(arg, "ON"))  { sEcho = 1; return; }
        expand_vars(arg, ex, sizeof(ex));
        con_puts(ex); con_puts("\r\n");
        return;
    }

    if (!t_stricmp(cmd, "MEM"))     { cmd_mem(); return; }
    if (!t_stricmp(cmd, "MEMTEST")) { cmd_memtest(); return; }
    if (!t_stricmp(cmd, "MODULES")) { module_report(); return; }
    /* TNX = 查看；TNXRUN = 带加载器跟踪地执行。
     * 想"直接跑"就敲程序名本身，见本函数末尾。 */
    if (!t_stricmp(cmd, "TNX") || !t_stricmp(cmd, "TNXINFO")) {
        if (!*arg) { con_puts("  usage: TNX <file.tnx>     dump header + section table (does not run)\r\n"); return; }
        tnx_info(arg);
        return;
    }
    if (!t_stricmp(cmd, "TNXRUN")) {
        if (!*arg) { con_puts("  usage: TNXRUN <file.tnx>  run with full loader trace\r\n"); return; }
        tnx_load(arg, 0, 1);
        return;
    }
    if (!t_stricmp(cmd, "DRIVERS")) { drv_report(); return; }
    if (!t_stricmp(cmd, "TIME")) {
        if (!t_time_on()) {
            con_puts("  no time base. set TNDDOS_TIMER=ON in efidos.sys / config.sys\r\n");
        } else {
            UINT64 ms = t_time_ms();
            con_puts("  uptime: "); con_u64(ms); con_puts(" ms");
            con_puts("   ("); con_u64(ms / 1000); con_puts(" s)\r\n");
        }
        return;
    }
    if (!t_stricmp(cmd, "SF")) {
        if (!*arg) {
            con_puts("  scale: "); con_u64((UINT64)con_get_scale()); con_puts("%");
            con_puts("   grid "); con_u64(con_cols()); con_puts("x"); con_u64(con_rows()); con_puts("\r\n");
            con_puts("  usage: SF 1.5 (=150%)  SF 200 (=200%)  SF 1 (=100%)  SF auto (fit 80x25)\r\n");
        } else {
            int pct = t_stricmp(arg, "auto") ? parse_scale(arg) : 0;
            if (pct < 0) {
                con_puts("  bad scale\r\n");
            } else if (!con_set_scale(pct)) {
                con_puts("  backend "); con_puts(con_current());
                con_puts(" does not support scaling\r\n");
            } else {
                con_puts("  scale -> "); con_u64((UINT64)con_get_scale()); con_puts("%");
                con_puts("   grid "); con_u64(con_cols()); con_puts("x"); con_u64(con_rows()); con_puts("\r\n");
            }
        }
        return;
    }
    if (!t_stricmp(cmd, "CONSOLE")) {
        if (!*arg) {
            con_puts("  console backends:\r\n");
            con_report();
            con_puts("  current: "); con_puts(con_current());
            con_puts("  ("); con_u64(con_cols()); con_puts("x"); con_u64(con_rows()); con_puts(")\r\n");
        } else {
            /* 先打招呼 —— 切到 fb 之后 ConOut 那边就没输出了 */
            con_puts("  switching console to "); con_puts(arg); con_puts(" ...\r\n");
            if (con_select(arg)) {
                /* **成功时保持安静。** 切到 fb 会清屏 —— 在这里多打一行，
                 * 它就会占掉新屏幕的第一行，把 autoexec 的欢迎语挤下去。
                 * 失败才是必须响的（下面那支），成功只记串口。 */
                log_puts("[log] shell CONSOLE -> "); log_puts(con_current()); log_puts("\r\n");
            } else {
                con_puts("  FAILED. staying on "); con_puts(con_current()); con_puts("\r\n");
                log_puts("[log] shell CONSOLE switch FAILED\r\n");
            }
        }
        return;
    }
    if (!t_stricmp(cmd, "LOAD")) {
        if (!*arg) { con_puts("  usage: LOAD <driver file>\r\n"); return; }
        if (EFI_ERROR(drv_load(arg))) con_puts("  load failed\r\n");
        else { con_puts("  OK\r\n"); drv_report(); }
        return;
    }
    if (!t_stricmp(cmd, "VOL"))     { con_puts("\r\n"); vfs_report(); return; }
    if (!t_stricmp(cmd, "PCI"))     { pci_report(); return; }
    /* **把 PCI 设备从固件的驱动手里摘下来**（v2.6 的 pci_own）。
     * 用法：OWN 0:3.0      bus:dev.fun
     * 这是为了验证 DisconnectController 真的生效 —— 摘完之后固件的驱动
     * 不该再碰那个设备（用 USB 的 QEMU trace 看得到）。 */
    /* ==================================================================
     * **PCILIST —— 我们的 PCI 枚举到底看见了哪些设备。**
     *
     * 真机上的问题：一块 Samsung NVMe（Windows 原生驱动就能用）我们认不出来。
     * 那先要回答一个更基本的问题 —— **我们看见它了吗**。
     *
     *   看见了但 NVMe 驱动没接住  -> 驱动的问题
     *   根本没看见              -> 枚举的问题（ECAM / ACPI / 总线范围）
     *
     * 这两条路的修法完全不同，不先把它们分开就是在赌。
     *
     * 输出每行是：BDF  class(4 位)  vendor:device  名字
     * class 0x010802 就是 NVMe 控制器，0x010400 是 RAID/VMD。
     * ================================================================== */
    if (!t_stricmp(cmd, "PCILIST")) {
        int n = pci_device_count(), i;
        con_puts("\r\n  PCI devices: ");
        con_u64((UINT64)n);
        con_puts("\r\n");
        for (i = 0; i < n; i++) {
            const TND_DEVICE *d = pci_device(i);
            if (!d) continue;
            con_puts("    ");
            con_puts(d->Name);
            con_puts("   class="); con_u64((UINT64)d->Class);
            con_puts("   "); con_u64((UINT64)d->Vendor);
            con_puts(":"); con_u64((UINT64)d->Device);
            if ((d->Class & 0xFFFF00) == 0x010800) con_puts("   <- NVMe");
            if (d->Class == 0x010400)               con_puts("   <- RAID/VMD");
            con_puts("\r\n");
        }
        if (!n) con_puts("    ** nothing -- ECAM or ACPI failed, see the serial log **\r\n");
        con_puts("\r\n");
        return;
    }

    /* ==================================================================
     * **FSINFO —— 把每个探测点的引导扇区解出来。**
     *
     * 用途只有一个：区分「盘上真的没文件」和「我读错了地方」。
     * 引导扇区里直接写着文件系统的全部关键参数，比任何猜测都硬：
     *
     *   偏移 11  每扇区字节数       512 还是 4096
     *   偏移 13  每簇扇区数
     *   偏移 17  根目录项数         **FAT32 这里是 0**
     *   偏移 22  每 FAT 扇区数      **FAT32 这里也是 0**
     *   偏移 54  文件系统类型串     "FAT16" / "FAT32" / "EXFAT"
     *
     * **FAT32 的两个关键字段都是 0。** 而我只认 FAT16 —— 所以看到 0
     * 就意味着"文件系统不是我能读的那种"，而不是"盘坏了"。
     * 这两个结论要赔上的东西天差地别，必须先分清。
     * ================================================================== */
    if (!t_stricmp(cmd, "FSINFO")) {
        static const UINT64 probe2[] = { 0, 1, 63, 2048 };
        static UINT8 sec[512];
        int nb = blk_count(), bi;
        unsigned k;

        con_puts("\r\n  block devices: ");
        con_u64((UINT64)nb);
        con_puts("\r\n");
        for (bi = 0; bi < nb; bi++) {
            const TND_BLOCK *b = blk_at(bi);
            con_puts("  --- ");
            con_puts(b && b->Name ? b->Name : "?");
            con_puts(" ---\r\n");
            for (k = 0; k < sizeof(probe2) / sizeof(probe2[0]); k++) {
                UINT64 lba = probe2[k];
                UINT32 bps, spc, rootEnt, fatsz, total;
                char fs[9];
                int j;

                con_puts("    LBA ");
                con_u64(lba);
                con_puts(" : ");
                if (blk_read(bi, lba, 1, sec) != 0) { con_puts("read failed\r\n"); continue; }
                if (!(sec[0] == 0xEB || sec[0] == 0xE9) || sec[510] != 0x55 || sec[511] != 0xAA) {
                    con_puts("no 55AA signature -- not a FAT volume at this LBA\r\n");
                    continue;
                }
                bps     = (UINT32)sec[11] | ((UINT32)sec[12] << 8);
                spc     = (UINT32)sec[13];
                rootEnt = (UINT32)sec[17] | ((UINT32)sec[18] << 8);
                fatsz   = (UINT32)sec[22] | ((UINT32)sec[23] << 8);
                total   = (UINT32)sec[19] | ((UINT32)sec[20] << 8);
                if (!total) total = (UINT32)sec[32] | ((UINT32)sec[33] << 8)
                                  | ((UINT32)sec[34] << 16) | ((UINT32)sec[35] << 24);
                for (j = 0; j < 8; j++) fs[j] = (char)sec[54 + j];
                fs[8] = 0;

                con_puts("bytes/sec="); con_u64((UINT64)bps);
                con_puts("  spc=");     con_u64((UINT64)spc);
                con_puts("  rootEnt="); con_u64((UINT64)rootEnt);
                con_puts("  fatsz=");   con_u64((UINT64)fatsz);
                con_puts("  total=");   con_u64((UINT64)total);
                con_puts("\r\n      type=\"");
                for (j = 0; j < 8 && fs[j]; j++) con_putc(fs[j]);
                con_puts("\"");
                if (fatsz == 0 || rootEnt == 0) con_puts("   ** this is FAT32 -- I only read FAT16 **");
                con_puts("\r\n");
            }
        }
        con_puts("\r\n");
        return;
    }

    /* ==================================================================
     * **FATDIR —— 用我们自己的块设备 + 我们自己的 FAT 列目录。**
     *
     * 和 DIR 的区别就是这件事的全部意义：
     *   DIR     走固件的 SimpleFileSystem —— **ExitBootServices 之后没有了**
     *   FATDIR  走 blk + fat          —— **一颗固件服务都不碰**
     *
     * 所以在 exitboot 之后敲这条，看到的就是"我们真的接手了"的证据。
     * ================================================================== */
    /* ==================================================================
     * **TYPE —— 把文件内容打出来。走 FatFs，不碰固件。**
     *
     * 加它有两个理由：
     *   1  它本来就该有（DOS 的基本命令）
     *   2  **它是定位手段** —— 如果 type config.sys 能读出来，而开机时
     *      conf.c 读不到，那问题在调用方式；如果 type 也读不到，
     *      那就是文件本身在盘上是坏的（EFI_LOAD_ERROR 常指坏簇链）。
     *      这两个结论完全不同，不能靠猜。
     * ================================================================== */
    if (!t_stricmp(cmd, "TYPE")) {
        char *arg = line + 4;      /* line 是原始行，cmd 只是前 32 字节的命令名 */
        static char tbuf[8192];
        UINTN tlen = 0;
        int trc;

        while (*arg == ' ') arg++;
        if (!*arg) { con_puts("  usage: TYPE <file>\r\n"); return; }
        if (!fat_ready()) {
            int bi, bn = blk_count();
            for (bi = 0; bi < bn && !fat_ready(); bi++) fat_mount(bi);
        }
        if (!fat_ready()) { con_puts("  no FAT volume\r\n"); return; }

        trc = fat_read_text(arg, tbuf, sizeof(tbuf), &tlen);
        con_puts("\r\n  fat_read_text rc="); con_u64((UINT64)trc);
        con_puts("  len="); con_u64((UINT64)tlen); con_puts("\r\n\r\n");
        if (trc == 0 && tlen) {
            UINTN i;
            for (i = 0; i < tlen; i++) {
                if (tbuf[i] == '\r') continue;
                if (tbuf[i] == '\n') con_putc('\r');
                con_putc(tbuf[i]);
            }
            con_puts("\r\n");
        }
        return;
    }

    /* ==================================================================
     * **FATWRITE —— 读写自检。**
     *
     * 读通路能列目录、写通路能下命令，**都不等于拼起来能用**。
     * 只有"写进去、读回来、一模一样"才算数。
     * ================================================================== */
    if (!t_stricmp(cmd, "FATWRITE")) {
        int ok;
        if (!fat_ready()) {
            int bi, bn = blk_count();
            for (bi = 0; bi < bn && !fat_ready(); bi++) fat_mount(bi);
        }
        ok = fat_selftest();
        con_puts(ok ? "\r\n  RESULT: WRITE PATH OK\r\n\r\n"
                    : "\r\n  RESULT: ** WRITE PATH FAILED **\r\n\r\n");
        return;
    }

    if (!t_stricmp(cmd, "FATDIR")) {
        int n = blk_count(), i, got = 0;
        con_puts("\r\n  block devices:\r\n");
        for (i = 0; i < n; i++) {
            const TND_BLOCK *b = blk_at(i);
            con_puts("    ");
            con_puts(b && b->Name ? b->Name : "?");
            con_puts("   ");
            con_u64(b ? b->Sectors : 0);
            con_puts(" sectors\r\n");
        }
        if (!n) {
            con_puts("  ** none registered -- load AHCI.TNX (or NVME.TNX) first **\r\n\r\n");
            return;
        }
        for (i = 0; i < n; i++) if (fat_mount(i)) { got = 1; break; }
        if (got) fat_report();
        else con_puts("  no FAT16 volume found on any block device\r\n\r\n");
        return;
    }

    /* ==================================================================
     * **FBWC —— 给帧缓冲开 write-combining（MTRR）。**
     *
     * 真机上刷屏慢的解法。真机帧缓冲是 UC（非缓存）内存，每个像素写都是
     * 一次独立总线事务；2K 屏下一次滚动要写 14.7 MB 的非缓存数据。
     *
     * 这里是**显式命令，开机不做** —— MTRR 范围设错会把普通内存变成 WC，
     * 那是数据损坏级的后果。fb_set_wc 里有三道闸，任何一道不过就拒绝。
     * MTRR 掉电即复位，试错了重启就回来。
     *
     * 自测用 TSC 增量做**前后对比**，所以不需要知道 CPU 频率。
     * ================================================================== */
    if (!t_stricmp(cmd, "FBWC")) {
        static const char *why[] = {
            "no framebuffer info", "CPU does not support WC", "no variable MTRRs",
            "range too small", "range not power-of-two aligned (REFUSED)",
            "** range contains normal RAM -- REFUSED **", "no free MTRR slot"
        };
        UINT64 before, after;
        int on = t_stricmp(arg, "OFF") ? 1 : 0;
        int rc;

        /* **先量、再清、最后才打结论。**
         * 上一版把 con_clear() 放在最后 —— 结论连同证据被自己擦掉了，
         * 截图上只剩一个提示符，什么都证明不了。测试设计错了。 */
        before = fb_bench();
        rc     = fb_set_wc(on);
        after  = fb_bench();

        con_clear();

        con_puts("  [FBWC] framebuffer write-combining via MTRR\r\n\r\n");
        con_puts("  4 MB sequential write into the framebuffer, TSC ticks:\r\n");
        con_puts("    before : "); con_u64(before); con_puts("\r\n");
        con_puts("    after  : "); con_u64(after);  con_puts("\r\n");
        if (before && after) {
            con_puts("    ratio  : "); con_u64(before / (after ? after : 1));
            con_puts("x faster\r\n");
        }
        con_puts("\r\n  MTRR   : ");
        if (rc) {
            int idx = -rc - 1;
            con_puts("refused -- ");
            if (idx >= 0 && idx < (int)(sizeof(why)/sizeof(why[0]))) con_puts(why[idx]);
            else con_puts("unknown");
        } else {
            con_puts(on ? "framebuffer is now write-combining"
                        : "write-combining turned off");
        }
        con_puts("\r\n\r\n");
        return;
    }

    /* ==================================================================
     * **EXITBOOT —— 内置指令：调用 ExitBootServices。**
     *
     * 不可逆。动手之前先把「退出之后什么还能用」摆清楚 ——
     * 上真机之前该看到的正是这张单子，而不是退出之后才发现键盘没了。
     * ================================================================== */
    if (!t_stricmp(cmd, "EXITBOOT")) {
        con_puts("\r\n  [EXITBOOT] ExitBootServices -- IRREVERSIBLE\r\n");
        con_puts("  ---- console backends (* = active right now) ----\r\n");
        con_report();
        con_puts("  ---- what still works afterwards ----\r\n");
        con_puts("    console : fb backend (writes the framebuffer directly)   OK\r\n");
        con_puts("    input   : ");
        con_puts(in_ready() ? in_current() : "** NONE -- firmware ConIn only, dies **");
        con_puts("\r\n    timer   : ");
        con_puts(t_time_on() ? "TSC calibrated (delays use it)" : "** not calibrated, delays spin **");
        con_puts("\r\n    memory  : PMM hands out pages from its own bitmap\r\n");
        con_puts("  ---- what stops working ----\r\n");
        con_puts("    file system           (goes through the firmware SFS)\r\n");
        con_puts("    loading more drivers  (LoadImage)\r\n");
        con_puts("    pool allocation       (DIR / FIND / anything with AllocatePool)\r\n");
        con_puts("\r\n  calling it now...\r\n");
        if (boot_exit() == 0) {
            con_puts("\r\n  ================================================\r\n");
            con_puts("   ** BOOT SERVICES ARE GONE **\r\n");
            con_puts("   the firmware no longer owns this machine.\r\n");
            con_puts("  ================================================\r\n");

            /* ============================================================
             * **退出之后自动做一次自检。**
             *
             * 理由很实在：退出之后每问一个问题都要人肉敲一遍命令，
             * 而命令行本身现在只有 PS/2 那一条路。让 EXITBOOT 一次把
             * 「块设备 / FAT / 根目录」三件事全打出来，人就只需要敲一个词。
             *
             * 顺序也刻意安排成从底层往上：先看有没有块设备（驱动），
             * 再看挂不挂得上 FAT（文件系统），最后才列目录。哪一层断了，
             * 上面那层就必然没输出 —— 一眼能看出断点在哪。
             * ============================================================ */
            con_puts("  ---- takeover self-check ----\r\n");
            {
                int i, n = blk_count();
                con_puts("    block devices : ");
                con_putc((char)('0' + (n > 9 ? 9 : n)));
                for (i = 0; i < n; i++) {
                    const TND_BLOCK *b = blk_at(i);
                    con_puts(" ");
                    con_puts(b && b->Name ? b->Name : "?");
                }
                con_puts("\r\n");

                if (n <= 0) {
                    con_puts("    ** no driver registered a block device **\r\n");
                    con_puts("    check DEVICE=NVME.TNX / AHCI.TNX in efidos.sys,\r\n");
                    con_puts("    and whether the driver's own banner appeared at boot.\r\n");
                } else {
                    if (!fat_ready())
                        for (i = 0; i < n && !fat_ready(); i++) fat_mount(i);
                    if (!fat_ready()) {
                        con_puts("    fat           : ** not found on any of them **\r\n");
                        con_puts("    the volume may be FAT32 (this reader is FAT16 only).\r\n");
                    } else {
                        con_puts("    fat           : mounted\r\n");
                        fat_report();
                    }
                }
            }
            con_puts("\r\n  VER and CLS still work; KEYS shows the input path.\r\n\r\n");
        } else {
            con_puts("\r\n  ** exit FAILED -- boot services are still alive **\r\n\r\n");
        }
        return;
    }

    if (!t_stricmp(cmd, "OWN")) {
        int v[3] = { 0, 0, 0 };
        int k = 0;
        const char *s = arg;
        while (*s && k < 3) {
            int x = 0, got = 0;
            for (;;) {
                int c = (unsigned char)*s;
                if (c >= '0' && c <= '9') x = x * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') x = x * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') x = x * 16 + (c - 'A' + 10);
                else break;
                s++; got = 1;
            }
            if (!got) break;
            v[k++] = x;
            if (*s == ':' || *s == '.') s++; else break;
        }
        if (k != 3) { con_puts("  usage: OWN bus:dev.fun    e.g. OWN 0:3.0\r\n"); return; }
        {
            const TND_API_TABLE *api = api_get_table();
            int n = (api && api->pci_own) ? api->pci_own(v[0], v[1], v[2]) : -1;
            char nb[12]; int i2;
            con_puts("  pci_own ");
            for (i2 = 0; i2 < 3; i2++) {
                const char *d = "0123456789ABCDEF";
                con_putc(d[(v[i2] >> 4) & 0xF]); con_putc(d[v[i2] & 0xF]);
                con_putc(i2 == 0 ? ':' : (i2 == 1 ? '.' : ' '));
            }
            if (n < 0) { con_puts("-> api unavailable\r\n"); return; }
            if (n > 99) n = 99;
            nb[0] = (char)('0' + (n / 10)); nb[1] = (char)('0' + (n % 10)); nb[2] = 0;
            con_puts("-> disconnected ");
            con_puts(n < 10 ? nb + 1 : nb);
            con_puts(" driver(s)\r\n");
        }
        return;
    }
    /* 输入诊断：把每个键的扫描码和字符原样打出来。
     * 接驱动的时候「到底收到了什么」比「界面对不对」有用得多。 */
    if (!t_stricmp(cmd, "KEYS")) {
        con_puts("  input backend: "); con_puts(in_current());
        con_puts("\r\n  dumping raw keys for ~5s, ESC to stop\r\n  ");
        for (int i = 0; i < 500; i++) {
            int k = in_pollkey();
            if (!k) { t_delay_us(10000); continue; }
            con_puts("[");
            con_u64((UINT64)((k >> 16) & 0xFFFF));
            con_puts(":'");
            { int ch = k & 0xFF; if (ch >= 32 && ch < 127) con_putc((char)ch); else con_puts("?"); }
            con_puts("'] ");
            if ((k & 0xFF) == 27) break;
        }
        con_puts("\r\n  done\r\n");
        return;
    }
    /* PS2 命令暂时没有 —— 它要等驱动接口。
     * 见 docs/DRIVER-SDK.md：内核是 MS ABI，碰不到端口，硬件访问只能在 TNX 驱动里。 */

    if (!t_stricmp(cmd, "DIR"))  { vfs_dir(*arg ? arg : "."); return; }
    if (!t_stricmp(cmd, "CD") || !t_stricmp(cmd, "CHDIR")) {
        if (!*arg) { con_puts("  "); con_putc(vfs_drive()); con_puts(":"); con_puts(vfs_cwd()); con_puts("\r\n"); return; }
        vfs_chdir(arg); return;
    }
    if (!t_stricmp(cmd, "MD") || !t_stricmp(cmd, "MKDIR")) { if (!*arg) { con_puts("  usage: MD <dir>\r\n"); return; } vfs_mkdir(arg); return; }
    if (!t_stricmp(cmd, "RD") || !t_stricmp(cmd, "RMDIR")) { if (!*arg) { con_puts("  usage: RD <dir>\r\n"); return; } vfs_rmdir(arg); return; }
    if (!t_stricmp(cmd, "TYPE")) { if (!*arg) { con_puts("  usage: TYPE <file>\r\n"); return; } vfs_type(arg); return; }
    if (!t_stricmp(cmd, "DEL") || !t_stricmp(cmd, "ERASE")) { if (!*arg) { con_puts("  usage: DEL <file>\r\n"); return; } vfs_unlink(arg); return; }

    if (!t_stricmp(cmd, "COPY")) {
        char a[TND_MAX_PATH], b[TND_MAX_PATH];
        split2(arg, a, sizeof(a), b, sizeof(b));
        if (!a[0] || !b[0]) { con_puts("  usage: COPY <src> <dst>\r\n"); return; }
        vfs_copy(a, b); return;
    }
    if (!t_stricmp(cmd, "REN") || !t_stricmp(cmd, "RENAME")) {
        char a[TND_MAX_PATH], b[TND_MAX_PATH];
        split2(arg, a, sizeof(a), b, sizeof(b));
        if (!a[0] || !b[0]) { con_puts("  usage: REN <old> <new>\r\n"); return; }
        vfs_rename(a, b); return;
    }

    if (!t_stricmp(cmd, "SET")) {
        if (!*arg) { con_puts("\r\n"); env_dump(); con_puts("\r\n"); return; }
        if (env_set(arg)) { con_puts("  OK\r\n"); log_puts("[log] set "); log_puts(arg); log_puts("\r\n"); }
        else con_puts("  failed (environment table full)\r\n");
        return;
    }

    if (!t_stricmp(cmd, "REBOOT")) {
        con_puts("\r\n  Rebooting ...\r\n");
        log_kv("shell", "ResetSystem(Cold)");
        gEnv.RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
        return;
    }
    if (!t_stricmp(cmd, "SHUTDOWN")) {
        con_puts("\r\n  Shutting down ...\r\n");
        log_kv("shell", "ResetSystem(Shutdown)");
        gEnv.RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, 0);
        return;
    }

    /* 不是内建命令 —— 按 DOS 的规矩当成程序名去找、去跑 */
    if (run_program(cmd, arg)) return;

    con_puts("  Bad command or file name\r\n");
    log_puts("[log] bad command: "); log_puts(cmd); log_puts("\r\n");
}

/* --------------------------------------------------------- run a batch */
void shell_exec_batch(const char *label, const char *text, UINTN len) {
    char line[TND_MAX_LINE];
    char *p = (char *)text;
    char *end = (char *)text + len;
    int n = 0;

    con_puts("\r\n  [running "); con_puts(label); con_puts("]\r\n");
    log_puts("[log] batch start: "); log_puts(label); log_puts("\r\n");

    while (p < end && *p) {
        char *eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        UINTN l = (UINTN)(eol - p);
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        for (UINTN k = 0; k < l; k++) line[k] = p[k];
        line[l] = 0;
        t_rtrim(line);

        char *q = t_skip_ws(line);
        int quiet = 0;
        if (*q == '@') { quiet = 1; q = t_skip_ws(q + 1); }

        if (*q) {
            n++;
            if (sEcho && !quiet) { con_puts("    "); con_puts(q); con_puts("\r\n"); }
            log_puts("[log] batch line: "); log_puts(q); log_puts("\r\n");
            shell_exec_line(q);
        }
        if (eol >= end) break;
        p = eol + 1;
    }
    con_puts("  [batch done, "); con_u64((UINT64)n); con_puts(" lines]\r\n");
    log_kv_u64("batch.lines", n);
}

/* ------------------------------------------------------ interactive loop */
static void show_prompt(void) {
    con_putc(vfs_drive());
    con_puts(":");
    con_puts(vfs_cwd());
    con_puts(">");
}

/* ------------------------------------------------ 命令历史 + 行编辑
 * DOS 5 的 DOSKEY 才给 Shell 加上历史和行编辑；更早的 command.com 只能
 * 从头敲。既然做得到，就没有理由不做 —— 敲错一个字符要重敲整行是很难受的。
 *
 * 关键点：**必须处理扫描码**。之前那个循环写的是
 *     if (k.UnicodeChar == 0) continue;
 * 于是方向键（UnicodeChar 为 0、只有扫描码）全被丢掉了。 */
#define HIST_MAX 16

static char gHist[HIST_MAX][TND_MAX_LINE];
static int  gHistCount = 0;

static void hist_add(const char *line) {
    int i;
    if (!line || !*line) return;
    if (gHistCount > 0 && !t_strcmp(gHist[gHistCount - 1], line)) return;   /* 连续重复不记 */
    if (gHistCount < HIST_MAX) {
        t_strncpy(gHist[gHistCount++], line, TND_MAX_LINE);
    } else {
        for (i = 1; i < HIST_MAX; i++) t_strncpy(gHist[i - 1], gHist[i], TND_MAX_LINE);
        t_strncpy(gHist[HIST_MAX - 1], line, TND_MAX_LINE);
    }
}

/* 重画当前行。maxLen 是这一轮里显示过的最长内容 ——
 * 用它把上一次留下的尾巴擦掉，不然改短之后会有残留字符。 */
static void redraw_line(UINTN col, UINTN row, const char *line, UINTN n, UINTN pos, UINTN *maxLen) {
    char tmp[TND_MAX_LINE + 8];
    UINTN i, t = 0;
    for (i = 0; i < n; i++) tmp[t++] = line[i];
    for (i = n; i < *maxLen; i++) tmp[t++] = ' ';
    tmp[t] = 0;

    con_gotoxy(col, row);
    con_puts(tmp);
    if (n > *maxLen) *maxLen = n;
    con_gotoxy(col + pos, row);
}

static void read_line(char *line, UINTN cap) {
    UINTN n = 0, pos = 0, maxLen = 0;
    UINTN startCol, row;
    int   hist = gHistCount;          /* == gHistCount 表示"还没进历史" */
    char  saved[TND_MAX_LINE];

    line[0] = 0;
    saved[0] = 0;

    /* 必须问**当前后端**，不能读 ConOut->Mode ——
     * 切到 fb 之后那个值是过期的，重画会落错位置（实测出过 "ddidir"）。 */
    con_getxy(&startCol, &row);

    for (;;) {
        EFI_INPUT_KEY k;
        /* **走输入后端，不直连固件** —— 否则 PS/2 驱动注册了 shell 也收不到。
         * 这里是行编辑的节奏点，所以用非阻塞的那个，自己 Stall。 */
        {
            int key = in_pollkey();
            if (!key) { t_delay_us(20000); continue; }
            k.ScanCode    = (UINT16)((key >> 16) & 0xFFFF);
            k.UnicodeChar = (UINT16)(key & 0xFFFF);
        }

        if (k.ScanCode) {
            int redraw = 1;
            switch (k.ScanCode) {
            case 0x01:                                   /* Up */
                if (gHistCount == 0) { redraw = 0; break; }
                if (hist == gHistCount) t_strncpy(saved, line, sizeof(saved));
                if (hist > 0) hist--;
                t_strncpy(line, gHist[hist], cap);
                n = pos = t_strlen(line);
                break;
            case 0x02:                                   /* Down */
                if (hist >= gHistCount) { redraw = 0; break; }
                hist++;
                t_strncpy(line, (hist == gHistCount) ? saved : gHist[hist], cap);
                n = pos = t_strlen(line);
                break;
            case 0x03: if (pos < n) pos++; else redraw = 0; break;   /* Right */
            case 0x04: if (pos > 0) pos--; else redraw = 0; break;   /* Left  */
            case 0x05: pos = 0; break;                                /* Home  */
            case 0x06: pos = n; break;                                /* End   */
            case 0x08:                                                /* Delete */
                if (pos < n) { UINTN i; for (i = pos; i < n; i++) line[i] = line[i + 1]; n--; }
                else redraw = 0;
                break;
            default: redraw = 0; break;
            }
            if (redraw) redraw_line(startCol, row, line, n, pos, &maxLen);
            continue;
        }

        if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n') {
            /* 先跳到行尾再换行，否则光标停在中间、后面的输出会盖住这行 */
            con_gotoxy(startCol + n, row);
            con_puts("\r\n");
            break;
        }
        if (k.UnicodeChar == 0x08) {                     /* Backspace */
            if (pos > 0) {
                UINTN i;
                for (i = pos - 1; i < n; i++) line[i] = line[i + 1];
                pos--; n--;
                redraw_line(startCol, row, line, n, pos, &maxLen);
            }
            continue;
        }
        if (k.UnicodeChar >= 0x20 && n + 1 < cap && n + 1 < TND_MAX_LINE) {
            UINTN i;
            for (i = n; i > pos; i--) line[i] = line[i - 1];
            line[pos] = (char)k.UnicodeChar;
            n++; pos++;
            redraw_line(startCol, row, line, n, pos, &maxLen);
        }
    }
    line[n] = 0;
}

int shell_start(void) {
    static char buf[TND_FILE_CAP];
    UINTN len = 0;

    con_puts("\r\n");
    con_puts("  " TND_NAME " Shell  (" TND_VERSION ")   type HELP for commands, EXIT to return\r\n");

    /* last step of the boot chain: run autoexec.bat
     *
     * **固件读不到就走 FatFs。** 同一个卷、同一个目录，
     * efidos.sys 用固件读得到，config.sys 和 autoexec.bat 却返回
     * EFI_MEDIA_CHANGED（0x800000000000000D）。
     * conf.c 里已经用这条路兜住了 config.sys（实测 fr=0, len=880），
     * autoexec.bat 用同一招。
     *
     * 注意这不是"绕过 bug" —— 最终形态里本来就只有 FatFs 这条路，
     * 固件那条是过渡期的退路。 */
    {
        int ok = 0;
        if (!EFI_ERROR(t_read_file(TND_AUTOEXEC, buf, TND_FILE_CAP, &len)) && len > 0) {
            ok = 1;
        } else if (fat_ready()) {
            int fr = fat_read_text(TND_AUTOEXEC, buf, TND_FILE_CAP, &len);
            log_puts("[log] autoexec: fat fallback fr="); log_u64((UINT64)fr);
            log_puts(" len="); log_u64((UINT64)len); log_puts("\r\n");
            if (fr == 0 && len > 0) ok = 1;
        }
        if (ok) shell_exec_batch("autoexec.bat", buf, len);
        else {
            con_puts("\r\n  (no autoexec.bat, skipped)\r\n");
            log_kv("autoexec", "missing");
        }
    }

    con_puts("\r\n");
    for (;;) {
        char line[TND_MAX_LINE];

        con_cursor(1);
        show_prompt();
        read_line(line, TND_MAX_LINE);

        t_rtrim(line);
        hist_add(line);
        log_puts("[log] cmd: "); log_puts(line); log_puts("\r\n");
        if (!t_stricmp(t_skip_ws(line), "EXIT")) { con_puts("  Leaving the shell.\r\n"); return 1; }
        shell_exec_line(line);
    }
}
