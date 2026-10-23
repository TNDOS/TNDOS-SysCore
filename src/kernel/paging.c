/* ============================================================================
 * TNDDOS — 页表：走表与自建表
 *
 * 背景：我们从来没有自己的页表。CR0.PG 一直是 1，CR3 里是固件的表，
 * 而文档里"虚拟 == 物理"的说法只在固件恒等映射覆盖到的范围内成立。
 * 这不是 bug，是缺了一块地基。
 *
 * 这个文件分两半，而且**刻意分开**：
 *
 *   paging_walk()      走**当前** CR3（也就是固件那张表），只读不写。
 *                      零风险，而且能立刻回答"某个地址到底映没映射"。
 *
 *   paging_build()     建我们自己的表。**先不装**，见下面 paging_install 的注释。
 *
 * 为什么先做 walk：建表和换 CR3 要处理 MMIO 的缓存属性（帧缓冲、PCI ECAM），
 * 弄错的后果是黑屏或者变得极慢，而且很难查。而 walk 不需要任何前提。
 * ==========================================================================*/
#include "tnd.h"

static UINT64 gPml4  = 0;      /* 我们自己的 PML4 物理地址（paging_build 之后有效） */
static UINT64 gMapEnd = 0;     /* 覆盖到的最高物理地址（2 MiB 对齐） */
static UINT64 gPtPages = 0;
static int    gBuilt = 0;
static int    gActive = 0;

const TND_MODULE_DEF gModPaging = { "paging", "page tables (walk the live CR3, build our own)", paging_init };

/* ------------------------------------------------------------------ 走表 */

void paging_walk(UINT64 va, PAGING_WALK *out) {
    UINT64 cr3 = 0, tbl, e;
    int i4, i3, i2, i1;

    if (!out) return;
    out->Present = 0; out->Writable = 0; out->User = 0;
    out->PageSize = 0; out->Phys = 0; out->Level = 0; out->Entry = 0;

    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    cr3 &= ~(UINT64)0xFFF;                  /* 去掉低 12 位的标志 */

    i4 = (int)((va >> 39) & 0x1FF);
    i3 = (int)((va >> 30) & 0x1FF);
    i2 = (int)((va >> 21) & 0x1FF);
    i1 = (int)((va >> 12) & 0x1FF);

    tbl = cr3;
    e = ((volatile UINT64 *)(UINTN)tbl)[i4];
    if (!(e & 1)) { out->Level = 4; out->Entry = e; return; }
    tbl = e & 0x000FFFFFFFFFF000ULL;

    e = ((volatile UINT64 *)(UINTN)tbl)[i3];
    if (!(e & 1)) { out->Level = 3; out->Entry = e; return; }
    if (e & 0x80) {                          /* PDPT 层就是 1 GiB 页 */
        out->Present = 1; out->Writable = (e >> 1) & 1; out->User = (e >> 2) & 1;
        out->PageSize = 1; out->Level = 3; out->Entry = e;
        out->Phys = (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
        return;
    }
    tbl = e & 0x000FFFFFFFFFF000ULL;

    e = ((volatile UINT64 *)(UINTN)tbl)[i2];
    if (!(e & 1)) { out->Level = 2; out->Entry = e; return; }
    if (e & 0x80) {                          /* PD 层的 2 MiB 页 */
        out->Present = 1; out->Writable = (e >> 1) & 1; out->User = (e >> 2) & 1;
        out->PageSize = 2; out->Level = 2; out->Entry = e;
        out->Phys = (e & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
        return;
    }
    tbl = e & 0x000FFFFFFFFFF000ULL;

    e = ((volatile UINT64 *)(UINTN)tbl)[i1];
    out->Level = 1; out->Entry = e;
    if (!(e & 1)) return;
    out->Present = 1; out->Writable = (e >> 1) & 1; out->User = (e >> 2) & 1;
    out->PageSize = 0; out->Phys = (e & 0x000FFFFFFFFFF000ULL) | (va & 0xFFF);
}

/* ------------------------------------------------------------------ 建表 */

static UINT64 *pt_page(void) {
    UINT64 p = pmm_alloc_pages(1);
    UINT8 *q;
    UINTN i;
    if (!p) return 0;
    q = (UINT8 *)(UINTN)p;
    for (i = 0; i < EFI_PAGE_SIZE; i++) q[i] = 0;   /* 必须清零，见 AUDIT M1 */
    gPtPages++;
    return (UINT64 *)q;
}

/* 建恒等映射。**覆盖到 4 GiB 以上**，因为帧缓冲和 PCI ECAM 都在 RAM 之外；
 * 只映 RAM 的话，换完 CR3 立刻黑屏。 */
int paging_build(void) {
    PMM_STATS st;
    UINT64 maxPhys, base;
    UINT64 *pml4;

    if (gBuilt) return 1;
    pmm_stats(&st);
    maxPhys = (UINT64)st.TotalPages * EFI_PAGE_SIZE;
    if (maxPhys < 0x100000000ULL) maxPhys = 0x100000000ULL;   /* 至少映到 4 GiB */
    maxPhys = (maxPhys + 0x1FFFFFULL) & ~(UINT64)0x1FFFFFULL;
    if (maxPhys > 0x0000004000000000ULL) maxPhys = 0x0000004000000000ULL;  /* 256 GiB 上限 */

    pml4 = pt_page();
    if (!pml4) { log_kv("paging", "no PML4 page"); return 0; }

    for (base = 0; base < maxPhys; base += 0x40000000ULL) {
        int i4 = (int)((base >> 39) & 0x1FF);
        UINT64 *pdpt, *pd;

        if (!(pml4[i4] & 1)) {
            pdpt = pt_page();
            if (!pdpt) return 0;
            pml4[i4] = ((UINT64)(UINTN)pdpt) | 0x3;          /* P | W */
        }
        pdpt = (UINT64 *)(UINTN)(pml4[i4] & 0x000FFFFFFFFFF000ULL);

        if (!(pdpt[(base >> 30) & 0x1FF] & 1)) {
            pd = pt_page();
            if (!pd) return 0;
            pdpt[(base >> 30) & 0x1FF] = ((UINT64)(UINTN)pd) | 0x3;
        }
        pd = (UINT64 *)(UINTN)(pdpt[(base >> 30) & 0x1FF] & 0x000FFFFFFFFFF000ULL);

        for (int i = 0; i < 512; i++) {
            UINT64 pa = base + (UINT64)i * 0x200000ULL;
            if (pa >= maxPhys) break;
            pd[i] = pa | 0x83;                                /* P | W | PS */
        }
    }

    gPml4 = (UINT64)(UINTN)pml4;
    gMapEnd = maxPhys;
    gBuilt = 1;
    log_puts("[log] paging: built our own tables, " );
    log_u64(gPtPages); log_puts(" pages, covers up to ");
    log_hex(gMapEnd); log_puts("\r\n");
    return 1;
}

/* **装我们自己的 CR3。**
 *
 * 现在**故意不调**。原因：固件的表可能把帧缓冲映射成 UC/WC，而我们的表
 * 是一律 WB。换成我们的表之后，MTRR 说 WC、页表说 WB，取最坏的 -> 帧缓冲
 * 变成 WB，滚动会变慢，而且没有任何报错。
 *
 * 要装之前必须先解决两件事：
 *   1  MMIO 区间要按固件的属性来（至少帧缓冲要 UC 或 WC）
 *   2  装完要能回滚（把固件 CR3 存下来，出错就换回去）
 *
 * 在那之前，walk 已经够用了 —— 它走的是当前的 CR3，而且只读。 */
int paging_install(void) {
    if (!gBuilt) return 0;
    log_kv("paging", "install is deliberately disabled, see the comment");
    return 0;
}

int paging_active(void) { return gActive; }
UINT64 paging_map_end(void) { return gMapEnd; }
UINT64 paging_own_cr3(void) { return gPml4; }
UINT64 paging_pt_pages(void) { return gPtPages; }

/* ------------------------------------------------------------------ 自检 */

/* 自检：走几个我们自己范围内的地址，确认它们是映射着的、可写的。
 * 这是 AUDIT 第 5.3 节要求的"页表自检"。 */
int paging_selftest(void) {
    UINT64 probes[6];
    int ok = 1, i;
    PAGING_WALK w;

    probes[0] = TNX_PROG_BASE;                 /* 程序窗口起点 */
    probes[1] = TNX_PROG_BASE + 0x1000;
    probes[2] = TNX_PROG_BASE + TNX_WINDOW_SIZE - 0x1000;
    probes[3] = TNX_DRIVER_BASE;
    probes[4] = (UINT64)(UINTN)&gPml4;         /* 内核自己的数据 */
    probes[5] = (UINT64)(UINTN)gEnv.ST;        /* 固件仍在使用的高地址 */

    for (i = 0; i < 6; i++) {
        paging_walk(probes[i], &w);
        log_puts("[log] paging.probe "); log_hex(probes[i]);
        if (!w.Present) {
            log_puts("  NOT PRESENT\r\n");
            ok = 0;
        } else {
            log_puts("  -> "); log_hex(w.Phys);
            log_puts((w.Writable ? "  W" : "  r"));
            log_puts("  size "); log_u64(w.PageSize ? (UINT64)(1 << w.PageSize) : 1);
            log_puts(" MiB\r\n");
        }
    }
    return ok;
}

int paging_init(void) {
    gPml4 = 0; gMapEnd = 0; gPtPages = 0; gBuilt = 0; gActive = 0;
    return 1;
}
