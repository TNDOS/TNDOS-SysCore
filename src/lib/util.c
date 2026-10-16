/* ============================================================================
 * TNDDOS — 最小字符串/内存工具
 * 内核不依赖 CRT，所有这一切都自己来。故意写得很笨，胜在能看懂。
 * ==========================================================================*/
#include "tnd.h"

UINTN t_strlen(const char *s) { UINTN n = 0; while (s && s[n]) n++; return n; }

int t_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int t_strncmp(const char *a, const char *b, UINTN n) {
    UINTN i = 0;
    for (; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int t_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int t_stricmp(const char *a, const char *b) {
    while (*a && t_toupper((unsigned char)*a) == t_toupper((unsigned char)*b)) { a++; b++; }
    return t_toupper((unsigned char)*a) - t_toupper((unsigned char)*b);
}

int t_strnicmp(const char *a, const char *b, UINTN n) {
    UINTN i = 0;
    for (; i < n; i++) {
        int ca = t_toupper((unsigned char)a[i]), cb = t_toupper((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!a[i]) return 0;
    }
    return 0;
}

void t_strcpy(char *d, const char *s) { while ((*d++ = *s++) != 0) { } }

void t_strncpy(char *d, const char *s, UINTN cap) {
    UINTN i = 0;
    if (!cap) return;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* 按机器字搬，不是逐字节。
 *
 * 逐字节在 -O0 下每字节要五到八条指令，而 -fno-builtin 让编译器不会把它
 * 识别成 memcpy 去做块搬移。帧缓冲滚动一次要搬 4 MB —— 每次滚动上千万条
 * 指令，QEMU 没开 KVM 时肉眼可见地卡。
 *
 * 两边都对到 8 字节才走字循环：错位访问在 x86 上只是慢，在 LoongArch 那类
 * 架构上会直接抛对齐异常 —— 移植时这是个真坑，所以这里不做假设。 */
/* ============================================================================
 * 关掉固件的看门狗定时器
 *
 * **UEFI 固件在启动时给自己装了一个看门狗并开始倒数。**
 * 引导程序如果不主动关它，时间一到**就强制复位**：
 *
 *   真机  -> 复位看起来像"自己关机"
 *   QEMU  -> 复位就是自动重启
 *
 * 而这个倒数是**从开机就开始的**，和我们当时在做什么没有因果关系 ——
 * 所以症状很迷惑：随便做点什么，机器突然就没了。我们就是被这个咬了：
 * 屏幕上刚出现一行信息就断电，一直以为是驱动的问题。
 *
 * **标准做法就是开机第一件事关掉它**（SetWatchdogTimer(0,0,0,NULL)）。
 * 之后要保活是操作系统自己的事 —— 那是我们以后想加再说的事，
 * 但**绝不能留着固件那个**：它的语义、时长、重启行为都不是我们能控的。
 * ==========================================================================*/
void t_disable_watchdog(void) {
    EFI_STATUS s;
    if (!gEnv.BS || !gEnv.BS->SetWatchdogTimer) {
        log_puts("[log] watchdog: no SetWatchdogTimer\r\n");
        return;
    }
    s = gEnv.BS->SetWatchdogTimer(0, 0, 0, 0);
    if (EFI_ERROR(s)) { log_puts("[log] watchdog: disable FAILED\r\n"); return; }
    log_puts("[log] watchdog: disabled\r\n");
}

void t_memzero(void *d, UINTN n) {
    unsigned char *p = (unsigned char *)d;
    while (n && ((UINTN)p & 7)) { *p++ = 0; n--; }
    while (n >= 8) { *(UINT64 *)p = 0; p += 8; n -= 8; }
    while (n--) *p++ = 0;
}

void t_memcpy(void *d, const void *s, UINTN n) {
    unsigned char *dp = (unsigned char *)d; const unsigned char *sp = (const unsigned char *)s;
    while (n && ((((UINTN)dp | (UINTN)sp) & 7))) { *dp++ = *sp++; n--; }
    while (n >= 8) { *(UINT64 *)dp = *(const UINT64 *)sp; dp += 8; sp += 8; n -= 8; }
    while (n--) *dp++ = *sp++;
}

int t_is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void t_upper(char *s) { for (; s && *s; s++) *s = (char)t_toupper((unsigned char)*s); }

char *t_skip_ws(char *s) { while (*s && (*s == ' ' || *s == '\t')) s++; return s; }

void t_rtrim(char *s) {
    UINTN n = t_strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

void t_utoa(UINT64 v, char *out) {
    char b[24]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int j = 0;
    while (i) out[j++] = b[--i];
    out[j] = 0;
}

/* t_ascii_to_u16 / t_utf8_to_u16 已挪到 src/lib/utf8.c —— 驱动也要用，
 * 那边没有内核的依赖，可以被 BOOTX64 / kernel / 各驱动分别链接。 */
