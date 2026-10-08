/* ============================================================================
 * TNDDOS -- 单调时基
 *
 * **不需要 IDT。** 这是这一层的设计要点：ticks() 要的是"现在过去多久了"，
 * 那是一次**读时基**，不是"等一个中断"。读 TSC 是几条指令，不调用就零开销。
 *
 * 唯一费 CPU 的做法是**轮询循环**（空转问"到了吗"）—— 我们不那么做。
 * 周期性**中断**（PIT/APIC）才需要 IDT，那是后面的事。
 *
 * 时基用 TSC（x86-64 的时间戳计数器）。频率没法直接问，所以用固件的
 * Stall 校：它保证至少延时那么久，TSC 的增量除以时间就是频率。
 *
 * 开关：SET TNDDOS_TIMER=ON|OFF（写在 efidos.sys 或 config.sys 里）。
 * 关掉时 ticks() **老实返回 0**，不给假值 —— 假的时间比没有时间更坏。
 * ==========================================================================*/
#include "tnd.h"

static int     gOn        = 0;      /* 0 = 未初始化或已关闭 */
static UINT64  gTscPerMs  = 0;
static UINT64  gTscBase   = 0;
static int     gInitTried = 0;

#if defined(__x86_64__) || defined(_M_X64)
static UINT64 rdtsc_now(void) { return (UINT64)__builtin_ia32_rdtsc(); }
#else
static UINT64 rdtsc_now(void) { return 0; }      /* 别的架构接上时在这里加 */
#endif

int t_time_on(void) { return gOn; }

UINT64 t_time_ms(void) {
    if (!gInitTried) t_time_init();          /* 懒初始化：省掉模块初始化顺序的麻烦 */
    if (!gOn || !gTscPerMs) return 0;
    return (rdtsc_now() - gTscBase) / gTscPerMs;
}

void t_time_init(void) {
    const char *v;

    if (gInitTried) return;
    gInitTried = 1;

    v = env_get("TNDDOS_TIMER");
    if (v && (t_stricmp(v, "OFF") == 0 || t_stricmp(v, "0") == 0 || t_stricmp(v, "NO") == 0)) {
        log_puts("[log] timer: disabled by TNDDOS_TIMER=");
        log_puts(v); log_puts("\r\n");
        gOn = 0;
        return;
    }

#if !defined(__x86_64__) && !defined(_M_X64)
    log_puts("[log] timer: no time base on this architecture yet\r\n");
    return;
#endif

    if (!gEnv.BS || !gEnv.BS->Stall) {
        log_puts("[log] timer: no Boot Services Stall, cannot calibrate\r\n");
        return;
    }

    /* 校频：Stall 保证**至少**延时这么久，所以这里量到的是个上限估计。
     * 100ms 是长度和精度的折中 —— 再长会拖慢首次 ticks()。 */
    {
        UINT64 a = rdtsc_now();
        gEnv.BS->Stall(100000);
        UINT64 b = rdtsc_now();

        if (b <= a) { log_puts("[log] timer: TSC did not advance\r\n"); return; }
        gTscPerMs = (b - a) / 100;
        if (!gTscPerMs) { log_puts("[log] timer: TSC too slow to measure\r\n"); return; }
    }

    gTscBase = rdtsc_now();
    gOn = 1;

    log_puts("[log] timer: TSC "); log_u64(gTscPerMs);
    log_puts(" cycles/ms  (~"); log_u64(gTscPerMs / 1000); log_puts(" MHz)\r\n");
}
