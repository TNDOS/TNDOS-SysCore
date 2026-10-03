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
    con_puts("    CONSOLE [name]    list or switch console backend (uefi / fb)\r\n");
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
                con_puts("  now using: "); con_puts(con_current()); con_puts("\r\n");
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
        EFI_STATUS s = gEnv.ST->ConIn->ReadKeyStroke(gEnv.ST->ConIn, &k);
        if (s == EFI_NOT_READY || EFI_ERROR(s)) { gEnv.BS->Stall(20000); continue; }

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
