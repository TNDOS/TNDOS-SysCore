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
    con_puts("    MEM               memory overview (UEFI map + PMM + heap)\r\n");
    con_puts("    MEMTEST           run memory self-tests\r\n");
    con_puts("    MODULES           list kernel modules and status\r\n");
    con_puts("    DRIVERS           list loaded drivers\r\n");
    con_puts("    LOAD <file>       load a UEFI image (~= load fs0:\\<file>)\r\n");
    con_puts("    <prog>            run a TNX program by name (extension optional)\r\n");
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
    if (!t_stricmp(cmd, "LOAD")) {
        if (!*arg) { con_puts("  usage: LOAD <driver file>\r\n"); return; }
        if (EFI_ERROR(drv_load(arg))) con_puts("  load failed\r\n");
        else { con_puts("  OK\r\n"); drv_report(); }
        return;
    }
    if (!t_stricmp(cmd, "VOL"))     { con_puts("\r\n"); vfs_report(); return; }

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

    /* 不是内建命令 —— 按 DOS 的规矩当成程序名去找、去跑。
     * 找的顺序：当前目录 -> PATH 每一项；名字没扩展名就补 .TNX。 */
    {
        char prog[TND_MAX_PATH];
        if (tnx_find(cmd, prog, sizeof(prog))) {
            if (*arg) con_puts("  (note: command-line arguments are not passed to TNX programs yet)\r\n");
            tnx_load(prog, 0, 0);      /* verbose = 0：只留程序自己的输出 */
            return;
        }
    }

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

int shell_start(void) {
    static char buf[TND_FILE_CAP];
    UINTN len = 0;

    con_puts("\r\n");
    con_puts("  " TND_NAME " Shell  (" TND_VERSION ")   type HELP for commands, EXIT to return\r\n");

    /* last step of the boot chain: run autoexec.bat */
    if (!EFI_ERROR(t_read_file(TND_AUTOEXEC, buf, TND_FILE_CAP, &len)) && len > 0) {
        shell_exec_batch("autoexec.bat", buf, len);
    } else {
        con_puts("\r\n  (no autoexec.bat, skipped)\r\n");
        log_kv("autoexec", "missing");
    }

    con_puts("\r\n");
    for (;;) {
        char line[TND_MAX_LINE];
        UINTN n = 0;
        show_prompt();
        for (;;) {
            EFI_INPUT_KEY k;
            EFI_STATUS s = gEnv.ST->ConIn->ReadKeyStroke(gEnv.ST->ConIn, &k);
            if (s == EFI_NOT_READY || EFI_ERROR(s)) { gEnv.BS->Stall(20000); continue; }
            if (k.UnicodeChar == 0) continue;
            if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n') { con_puts("\r\n"); break; }
            if (k.UnicodeChar == 0x08) { if (n) { n--; con_puts("\b \b"); } continue; }
            if (k.UnicodeChar >= 0x20 && n + 1 < TND_MAX_LINE) {
                line[n++] = (char)k.UnicodeChar;
                con_putc((char)k.UnicodeChar);
            }
        }
        line[n] = 0;
        t_rtrim(line);
        log_puts("[log] cmd: "); log_puts(line); log_puts("\r\n");
        if (!t_stricmp(t_skip_ws(line), "EXIT")) { con_puts("  Leaving the shell.\r\n"); return 1; }
        shell_exec_line(line);
    }
}
