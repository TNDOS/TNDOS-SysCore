/* ============================================================================
 * 帧缓冲的 write-combining（MTRR）—— **只有内核构建里有这个文件**
 *
 * 放在内核而不是 con_fb.c，是因为它要用 pmm 的位图做安全检查，
 * 而 pmm 只在 kernel.efi 里（BOOTX64.EFI 没有内存管理器）。
 *
 * **真机的帧缓冲是 UC（非缓存）内存。** 每个像素写都是一次独立的总线事务，
 * CPU 的写合并完全用不上。后果在 2K 屏上很具体（帧缓冲 14.7 MB）：
 *
 *   con_fb.c 的 fb_scroll 里那一句 t_memcpy 搬的就是帧缓冲本体
 *   -> 14.7 MB 的非缓存写。按 10~50M 次写/秒算，**一次滚动 0.3~1.5 秒**；
 *   启动输出一百来行要滚五十来次 —— 屏幕上就是「一帧一帧地刷」。
 *
 * MTRR 能把这个范围标成 WC：CPU 把写攒成整块再发出去，快一到两个数量级。
 * **和分页无关** —— MTRR 是纯 MSR 操作，我们本来也没有分页。
 *
 * **危险在哪**：MTRR 范围设错（盖到普通内存上），那块 RAM 会变成 WC，
 * 缓存语义就错了 —— **那是数据损坏级的后果**。所以这里有三道闸：
 *
 *   1  范围必须在 2 的幂边界上对齐。不满足就**直接放弃**，不硬凑。
 *   2  **范围里不能有任何位图标为空闲的页** —— 那就是普通内存。
 *   3  没有空闲的 MTRR 槽就放弃。
 *
 * 而且它是**显式命令**（shell 的 FBWC），开机不做。
 * MTRR 掉电即复位，试错了重启就回来。
 * ==========================================================================*/
#include "tnd.h"

#define FB_MSR_MTRRCAP    0x000000FEu
#define FB_MSR_MTRR_DEF   0x000002FFu
#define FB_MSR_MTRR_BASE  0x00000200u

static int gWcSlot = -1;      /* 我们设的槽号；-1 = 没设 */

static UINT64 fb_rdmsr(UINT32 msr) {
    UINT32 lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | (UINT64)lo;
}
static void fb_wrmsr(UINT32 msr, UINT64 v) {
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"((UINT32)v), "d"((UINT32)(v >> 32)) : "memory");
}

int fb_set_wc(int on) {
    UINT64 cap, base, size, mask;
    int n, i, slot = -1;

    if (!fb_info(&base, &size)) return -1;        /* 没有帧缓冲 */

    cap = fb_rdmsr(FB_MSR_MTRRCAP);
    n   = (int)(cap & 0xFF);
    if (!(cap & (1ull << 8))) return -2;          /* CPU 不支持 WC */
    if (n <= 0) return -3;                        /* 没有可变 MTRR */
    if (size < 4096) return -4;

    if (!on) {
        if (gWcSlot < 0) return 0;
        fb_wrmsr(FB_MSR_MTRR_BASE + (UINT32)gWcSlot * 2, 0);
        fb_wrmsr(FB_MSR_MTRR_BASE + (UINT32)gWcSlot * 2 + 1, 0);
        gWcSlot = -1;
        return 0;
    }
    if (gWcSlot >= 0) return 0;                   /* 已经开着 */

    { UINT64 s = 1; while (s < size) s <<= 1; size = s; }   /* 上取到 2 的幂 */
    if (base & (size - 1)) return -5;             /* 闸 1：对不齐 —— 放弃，不硬凑 */

    {   /* 闸 2：范围里不能有普通内存 */
        UINT64 p;
        for (p = base; p < base + size; p += 4096)
            if (pmm_page_is_free(p)) return -6;
    }

    for (i = 0; i < n; i++)                       /* 闸 3：要有空槽 */
        if (!(fb_rdmsr(FB_MSR_MTRR_BASE + (UINT32)i * 2) & (1ull << 11))) { slot = i; break; }
    if (slot < 0) return -7;

    mask = ~(size - 1);
    fb_wrmsr(FB_MSR_MTRR_BASE + (UINT32)slot * 2,
             (base & ~0xFFFull) | 1ull /* WC */ | (1ull << 11) /* valid */);
    fb_wrmsr(FB_MSR_MTRR_BASE + (UINT32)slot * 2 + 1,
             (mask & ~0xFFFull) | (1ull << 11));

    {                                             /* MTRR 总开关 */
        UINT64 d = fb_rdmsr(FB_MSR_MTRR_DEF);
        if (!(d & (1ull << 11))) fb_wrmsr(FB_MSR_MTRR_DEF, d | (1ull << 11));
    }

    gWcSlot = slot;
    return 0;
}
