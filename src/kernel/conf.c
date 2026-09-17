/* ============================================================================
 * TNDDOS — efidos.sys / config.sys 解析 与 环境变量
 *
 *   efidos.sys : 系统配置，安装期部署，先加载    （≈ DOS7 的 MSDOS.SYS 的位置）
 *   config.sys : 用户配置，完全交给用户          （传统 config.sys 语义）
 * 两者语法相同。
 * ==========================================================================*/
#include "tnd.h"

static int conf_module_init(void) { return 1; }   /* 实际解析由 kernel 显式调用，好控制输出顺序 */

const TND_MODULE_DEF gModConf = { "conf", "efidos.sys / config.sys parsing + environment", conf_module_init };

static char sEnv[TND_MAX_ENV][96];
static int  sEnvCount = 0;

const char *env_get(const char *name) {
    UINTN nl = t_strlen(name);
    for (int i = 0; i < sEnvCount; i++) {
        if (!t_strnicmp(sEnv[i], name, nl) && sEnv[i][nl] == '=') return sEnv[i] + nl + 1;
    }
    return 0;
}

/* 传入 "NAME=VALUE" 或 "NAME VALUE" 或单独的 "NAME" */
int env_set(const char *kv) {
    char tmp[96];
    t_strncpy(tmp, kv, sizeof(tmp));
    for (char *p = tmp; *p; p++) if (*p == ' ') *p = '=';
    if (!tmp[0]) return 0;
    if (!env_get(tmp)) {
        if (sEnvCount >= TND_MAX_ENV) return 0;
        t_strncpy(sEnv[sEnvCount], tmp, 96);
        sEnvCount++;
    } else {
        /* 覆盖：找到名字相同的那条 */
        UINTN nl = 0; while (tmp[nl] && tmp[nl] != '=') nl++;
        for (int i = 0; i < sEnvCount; i++)
            if (!t_strnicmp(sEnv[i], tmp, nl) && sEnv[i][nl] == '=') { t_strncpy(sEnv[i], tmp, 96); break; }
    }
    return 1;
}

void env_dump(void) {
    con_puts("  Environment variables ("); con_u64((UINT64)sEnvCount); con_puts(")\r\n");
    for (int i = 0; i < sEnvCount; i++) { con_puts("    "); con_puts(sEnv[i]); con_puts("\r\n"); }
}

static void show_item(const char *k, const char *v) {
    con_puts("    ");
    con_puts(k);
    for (UINTN i = t_strlen(k); i < 10; i++) con_puts(" ");
    con_puts("= ");
    con_puts(v);
    con_puts("\r\n");
}

/* 解析一份配置文件。isSystem=1 表示 efidos.sys */
void conf_parse(const char *path, int isSystem) {
    static char buf[TND_FILE_CAP];
    UINTN len = 0;
    int lines = 0, applied = 0, comments = 0, unknown = 0;

    con_puts("\r\n");
    con_puts(isSystem ? "-- efidos.sys   system config (install-time, loaded first)\r\n"
                      : "-- config.sys   user config\r\n");

    if (EFI_ERROR(t_read_file(path, buf, TND_FILE_CAP, &len)) || len == 0) {
        con_puts("   (missing or empty, skipped)\r\n");
        log_kv("conf", "missing");
        return;
    }

    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char save = *eol;
        *eol = 0;

        char line[TND_MAX_LINE];
        t_strncpy(line, p, sizeof(line));
        t_rtrim(line);

        char *q = t_skip_ws(line);
        if (*q) {
            lines++;
            if (*q == ';' || *q == '#' || !t_strnicmp(q, "REM", 3)) {
                comments++;
            } else if (!t_strnicmp(q, "SET ", 4)) {
                char *arg = t_skip_ws(q + 4);
                if (env_set(arg)) { applied++; show_item("SET", arg); }
                else unknown++;
            } else if (!t_strnicmp(q, "DEVICE", 6) && (q[6] == '=' || q[6] == ' ')) {
                char *arg = t_skip_ws(q + 6);
                if (*arg == '=') arg = t_skip_ws(arg + 1);
                applied++;
                show_item("DEVICE", arg);
                /* DEVICE=<文件>  ≈  load fs0:\<文件>：
                 * 读进来 -> LoadImage -> StartImage。失败只记账，不中断启动。 */
                drv_load(arg);
            } else if (!t_strnicmp(q, "SHELL", 5) && (q[5] == '=' || q[5] == ' ')) {
                char *arg = t_skip_ws(q + 5);
                if (*arg == '=') arg = t_skip_ws(arg + 1);
                applied++; show_item("SHELL", arg);
            } else if (!t_strnicmp(q, "FILES", 5) || !t_strnicmp(q, "BUFFERS", 7)
                    || !t_strnicmp(q, "LASTDRIVE", 9)) {
                char *eq = q; while (*eq && *eq != '=') eq++;
                if (*eq == '=') { applied++; show_item("PARAM", q); }
                else unknown++;
            } else {
                unknown++;
                con_puts("    ? unknown directive: "); con_puts(q); con_puts("\r\n");
                log_puts("[log] unknown directive in "); log_puts(path); log_puts(": "); log_puts(q); log_puts("\r\n");
            }
        }

        if (!save) break;
        p = eol + 1;
    }

    con_puts("    "); con_u64((UINT64)lines);
    con_puts(" lines -- "); con_u64((UINT64)applied);
    con_puts(" applied, "); con_u64((UINT64)comments);
    con_puts(" comments, "); con_u64((UINT64)unknown); con_puts(" unknown\r\n");
    log_kv_u64("conf.lines", lines);
    log_kv_u64("conf.applied", applied);
}
