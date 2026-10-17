/* ============================================================================
 * TNDDOS -- physical memory manager
 *
 * Take the UEFI memory map, build a bitmap covering all physical pages (1 = used),
 * mark every EfiConventionalMemory page free, then take back three things:
 *   1) the pages the bitmap itself occupies
 *   2) the low 1 MB (BIOS / real-mode structures still live there)
 *   3) pages the firmware refuses to hand over (see pmm_alloc_pages)
 *
 * Important: we have NOT called ExitBootServices, so the firmware still owns
 * this memory. Actually taking a page must therefore go through UEFI
 * AllocatePages(AllocateAddress) -- otherwise the firmware would hand the same
 * page out again. The bitmap chooses; UEFI reserves. Only after
 * ExitBootServices does the bitmap become the sole authority.
 * ==========================================================================*/
#include "tnd.h"

#define PMM_PAGE_SHIFT 12
#define PMM_PAGE_SIZE  (1ULL << PMM_PAGE_SHIFT)

static UINT8  *gBitmap = 0;
static UINT64  gBitmapBytes = 0;
static UINT64  gTotalPages = 0;
static UINT64  gManagedPages = 0;
static UINT64  gFreePages = 0;
static UINT64  gUsedPages = 0;
static UINT64  gReserveLowPages = 0;
static UINT64  gBitmapPages = 0;
static UINT64  gAllocCount = 0, gFreeCount = 0, gFailCount = 0;
static int     gReady = 0;
/* 两个 TNX 窗口到底从固件手里占住了没有。**加载器靠它决定敢不敢往里写。** */
static int     gWinProgOk = 0, gWinDrvOk = 0;

const TND_MODULE_DEF gModPmm = { "pmm", "physical memory manager (bitmap + UEFI reserve)", pmm_init };

#define BIT_SET(i) (gBitmap[(i) >> 3] = (UINT8)(gBitmap[(i) >> 3] |  (UINT8)(1u << ((i) & 7))))
#define BIT_CLR(i) (gBitmap[(i) >> 3] = (UINT8)(gBitmap[(i) >> 3] & (UINT8)~(1u << ((i) & 7))))
#define BIT_GET(i) ((gBitmap[(i) >> 3] >> ((i) & 7)) & 1u)

/* Which memory types count as real RAM. MMIO / reserved / unusable do not. */
static int pmm_is_ram_type(UINT32 t) {
    switch (t) {
        case EfiLoaderCode: case EfiLoaderData:
        case EfiBootServicesCode: case EfiBootServicesData:
        case EfiRuntimeServicesCode: case EfiRuntimeServicesData:
        case EfiConventionalMemory:
        case EfiPersistentMemory:
            return 1;
        default:
            return 0;
    }
}

static EFI_MEMORY_DESCRIPTOR *map_desc(EFI_MEMORY_DESCRIPTOR *base, UINTN i, UINTN dsize) {
    return (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)base + i * dsize);
}

int pmm_init(void) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN size = 0, key = 0, dsize = 0;
    UINT32 dver = 0;
    EFI_STATUS s;
    UINT64 i, highest = 0;

    if (!gEnv.BS) return 0;

    s = gEnv.BS->GetMemoryMap(&size, 0, &key, &dsize, &dver);
    if (!dsize) { log_kv("pmm", "descriptor size = 0, giving up"); return 0; }
    size += 8192;                                   /* the map can grow between calls */
    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, size, (void **)&map)) || !map) {
        log_kv("pmm", "memory map buffer allocation failed"); return 0;
    }
    s = gEnv.BS->GetMemoryMap(&size, map, &key, &dsize, &dver);
    if (EFI_ERROR(s)) { log_kv("pmm", "GetMemoryMap failed"); gEnv.BS->FreePool(map); return 0; }

    UINTN count = size / dsize;

    /* Only count types that are actually RAM.
     * Lesson: MMIO / reserved regions sit at very high physical addresses (TB
     * range under QEMU/OVMF). Sizing the bitmap by the highest physical address
     * blows it up to tens of MB and makes every statistic meaningless. */
    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = map_desc(map, i, dsize);
        UINT64 end;
        if (!pmm_is_ram_type(d->Type)) continue;
        end = d->PhysicalStart + (d->NumberOfPages << PMM_PAGE_SHIFT);
        if (end > highest) highest = end;
    }
    gTotalPages = highest >> PMM_PAGE_SHIFT;

    /* Defensive: clamp absurd values instead of allocating an absurd bitmap */
    if (gTotalPages > (16ULL * 1024 * 1024 * 1024 / PMM_PAGE_SIZE)) {
        log_kv_u64("pmm.!! page count clamped from", gTotalPages);
        gTotalPages = 16ULL * 1024 * 1024 * 1024 / PMM_PAGE_SIZE;
    }
    gBitmapBytes = (gTotalPages + 7) / 8;

    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, gBitmapBytes, (void **)&gBitmap)) || !gBitmap) {
        log_kv("pmm", "bitmap allocation failed"); gEnv.BS->FreePool(map); return 0;
    }
    for (i = 0; i < gBitmapBytes; i++) gBitmap[i] = 0xFF;   /* start with everything used */

    /* EfiConventionalMemory is what we can actually hand out */
    gManagedPages = 0;
    for (i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = map_desc(map, i, dsize);
        if (d->Type != EfiConventionalMemory) continue;
        UINT64 first = d->PhysicalStart >> PMM_PAGE_SHIFT;
        for (UINT64 k = 0; k < d->NumberOfPages; k++) {
            UINT64 pg = first + k;
            if (pg < gTotalPages && BIT_GET(pg)) { BIT_CLR(pg); gManagedPages++; }
        }
    }

    /* take back the bitmap's own pages */
    UINT64 bmFirst = ((UINT64)(UINTN)gBitmap) >> PMM_PAGE_SHIFT;
    gBitmapPages = (gBitmapBytes + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT;
    for (i = 0; i < gBitmapPages; i++) {
        UINT64 pg = bmFirst + i;
        if (pg < gTotalPages && !BIT_GET(pg)) BIT_SET(pg);
    }

    /* take back the low 1 MB */
    gReserveLowPages = (1024ULL * 1024) >> PMM_PAGE_SHIFT;
    for (i = 0; i < gReserveLowPages && i < gTotalPages; i++) {
        if (!BIT_GET(i)) BIT_SET(i);
    }

    /* 预留 TNX 程序映像窗口。
     * 必须在这里就占住：否则内核堆的首次适配会先把它吃掉，
     * 之后每次加载 TNX 都会失败 —— 而且失败原因看起来跟"内存不够"一模一样。
     *
     * 注意 gReady 必须先置位：pmm_reserve 开头有 gReady 检查，
     * 放在它后面的话这次预留会**静默地什么都不做** ——
     * 位图上看不出来，只有等到堆长到那个地址才炸。 */
    gReady = 1;
    /* **两个窗口都要占住。** 只占程序窗口的话，驱动窗口会被堆吃掉，
     * 症状是「驱动加载失败」而且看起来像内存不够 —— 极难查。 */
    /* **返回值必须看。** 申请失败意味着固件还拥有那块内存 —— 我们再往里写
     * TNX 就是在毁固件的数据，而症状是 ExitBootServices 里固件断言死循环。
     * 失败的那一个窗口，加载器会拒绝使用。 */
    gWinProgOk = pmm_reserve(TNX_PROG_BASE,   (UINTN)((TNX_WINDOW_SIZE + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT));
    gWinDrvOk  = pmm_reserve(TNX_DRIVER_BASE, (UINTN)((TNX_WINDOW_SIZE + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT));
    if (!gWinProgOk) log_puts("[log] pmm: ** program window NOT ours -- TNX programs will be refused **\r\n");
    if (!gWinDrvOk)  log_puts("[log] pmm: ** driver window NOT ours -- TNX drivers will be refused **\r\n");

    gFreePages = 0; gUsedPages = 0;
    for (i = 0; i < gTotalPages; i++) { if (BIT_GET(i)) gUsedPages++; else gFreePages++; }

    gEnv.BS->FreePool(map);

    log_kv_u64("pmm.totalPages", gTotalPages);
    log_kv_u64("pmm.managedPages", gManagedPages);
    log_kv_u64("pmm.freePages", gFreePages);
    log_kv_u64("pmm.bitmapPages", gBitmapPages);
    return 1;
}

void pmm_stats(PMM_STATS *o) {
    if (!o) return;
    o->TotalPages = gTotalPages;
    o->ManagedPages = gManagedPages;
    o->FreePages = gFreePages;
    o->UsedPages = gUsedPages;
    o->ReserveLowPages = gReserveLowPages;
    o->BitmapBytes = gBitmapBytes;
    o->AllocCount = gAllocCount;
    o->FreeCount = gFreeCount;
    o->FailCount = gFailCount;
}

static UINT64 find_free_run(UINTN count) {
    UINT64 run = 0;
    for (UINT64 i = gReserveLowPages; i < gTotalPages; i++) {
        if (!BIT_GET(i)) {
            run++;
            if (run >= count) return i + 1 - count;
        } else {
            run = 0;
        }
    }
    return (UINT64)-1;
}

UINT64 pmm_alloc_pages(UINTN count) {
    if (!gReady || count == 0) return 0;
    if ((UINT64)count > gFreePages) { gFailCount++; return 0; }

    for (int attempt = 0; attempt < 32; attempt++) {
        UINT64 idx = find_free_run(count);
        if (idx == (UINT64)-1) { gFailCount++; return 0; }

        EFI_PHYSICAL_ADDRESS want = (EFI_PHYSICAL_ADDRESS)(idx << PMM_PAGE_SHIFT);
        EFI_PHYSICAL_ADDRESS got = want;
        /* **ExitBootServices 之后 AllocatePages 就不存在了。**
         * 那时候位图是唯一的真相，直接从它发出去。
         * 退出前必须问固件 —— 位图以为是空闲的页，固件可能自己占着。 */
        EFI_STATUS s = gBsGone ? EFI_SUCCESS
                               : gEnv.BS->AllocatePages(AllocateAddress, EfiLoaderData, count, &got);

        /* Mark used whether or not it succeeded: a failure means the firmware
         * does not own up to this range (bitmap thought it was free but someone
         * else holds it), so never hand it out again. Losing a few pages beats
         * handing out memory twice. */
        for (UINTN k = 0; k < count; k++) {
            UINT64 pg = idx + k;
            if (pg < gTotalPages && !BIT_GET(pg)) { BIT_SET(pg); gFreePages--; gUsedPages++; }
        }

        if (!EFI_ERROR(s) && got == want) { gAllocCount++; return (UINT64)want; }

        log_puts("[log] pmm: AllocateAddress refused at ");
        log_hex((UINT64)want);
        log_puts(", trying next run\r\n");
    }
    gFailCount++;
    return 0;
}

UINT64 pmm_alloc_page(void) { return pmm_alloc_pages(1); }

void pmm_free_pages(UINT64 addr, UINTN count) {
    if (!gReady || !addr || !count) return;
    UINT64 idx = addr >> PMM_PAGE_SHIFT;
    if (!gBsGone) gEnv.BS->FreePages((EFI_PHYSICAL_ADDRESS)addr, count);
    for (UINTN k = 0; k < count; k++) {
        UINT64 pg = idx + k;
        if (pg < gTotalPages && pg >= gReserveLowPages && BIT_GET(pg)) {
            BIT_CLR(pg); gFreePages++; gUsedPages--;
        }
    }
    gFreeCount++;
}

void pmm_free_page(UINT64 addr) { pmm_free_pages(addr, 1); }

/* **这一页在我们位图里是"空闲"吗。**
 *
 * 给 fb_set_wc 做安全检查用：MTRR 只允许盖在 MMIO 上。
 * 如果帧缓冲那个范围里有一页在位图里是空闲的，说明它是普通内存 ——
 * 把它设成 write-combining 会造成数据损坏，必须拒绝。 */
/* 这个窗口真的从固件手里拿到了吗。**加载器用它决定敢不敢往那儿写。**
 * 0x1000000 那个程序窗口在 QEMU+OVMF 上实测是**拿不到**的，
 * 而原来的代码写着「固件拒绝也没关系」，照样往里写 —— 那就是在踩固件的内存。 */
int pmm_window_ok(UINT64 base) {
    if (base == TNX_PROG_BASE)   return gWinProgOk;
    if (base == TNX_DRIVER_BASE) return gWinDrvOk;
    return 0;
}

int pmm_page_is_free(UINT64 addr) {
    UINT64 pg = addr >> PMM_PAGE_SHIFT;
    if (!gReady || pg >= gTotalPages) return 0;      /* 位图覆盖不到 —— 当作不是普通内存 */
    return BIT_GET(pg) ? 0 : 1;
}

/* ============================================================================
 * 把一段物理区间永久占住 **并返回到底占住了没有**
 *
 * **"固件拒绝也没关系"这句话是错的。** 原注释就是这么写的，而后果极难查：
 *
 *   [log] pmm: reserve 0x0000000001000000 via AllocatePages failed, bitmap only
 *
 * 也就是说 16 MiB 的程序窗口**根本没拿到**，但我们照样往里写 TNX 程序 ——
 * 覆盖的是固件自己的内存。于是 ExitBootServices 里固件摸到一块已经被写成
 * 我们代码的内存 -> 断言 -> CpuDeadLoop：
 *
 *     RIP=0x493e8  RFL=0x47 (**IF=0**)  HLT=0     <- 关中断空转，典型的死循环
 *
 * 屏幕上看起来就是"卡在 calling ExitBootServices..."，和"内存被踩"完全对不上。
 *
 * 所以现在返回 1/0，**调用方必须看**。位图那边仍然标死（我们自己不会重复发放），
 * 但"固件那边也让出来了"是另一件事，不能混为一谈。 */
int pmm_reserve(UINT64 base, UINTN pages) {
    int claimed = 0;
    if (!gReady) return 0;
    {
        EFI_PHYSICAL_ADDRESS at = (EFI_PHYSICAL_ADDRESS)base;
        EFI_STATUS s = gBsGone ? EFI_SUCCESS
                               : gEnv.BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &at);
        if (EFI_ERROR(s) || at != (EFI_PHYSICAL_ADDRESS)base) {
            log_puts("[log] pmm: reserve "); log_hex(base);
            log_puts(" ** NOT CLAIMED ** -- the firmware still owns it\r\n");
        } else {
            claimed = 1;
        }
    }
    UINT64 first = base >> PMM_PAGE_SHIFT;
    for (UINTN k = 0; k < pages; k++) {
        UINT64 pg = first + k;
        if (pg < gTotalPages && !BIT_GET(pg)) { BIT_SET(pg); gFreePages--; gUsedPages++; }
    }
    log_puts("[log] pmm: reserved "); log_u64((UINT64)pages);
    log_puts(" pages @ "); log_hex(base);
    log_puts(claimed ? "  (claimed from the firmware)\r\n" : "  (** bitmap only **)\r\n");
    return claimed;
}

/* Self-test: really take pages, fill with a pattern, read back, give them back.
 * Also checks the returned address is page aligned. */
int pmm_selftest(void) {
    const UINTN n = 8;
    UINT64 a = pmm_alloc_pages(n);
    volatile UINT8 *p;
    int ok = 1;

    if (!a) { log_kv("pmm.selftest", "allocation failed"); return 0; }
    if (a & (PMM_PAGE_SIZE - 1)) { log_kv("pmm.selftest", "returned address not page aligned"); ok = 0; }

    p = (volatile UINT8 *)(UINTN)a;
    for (UINTN i = 0; i < n * PMM_PAGE_SIZE; i++) p[i] = (UINT8)((i * 31 + 7) & 0xFF);
    for (UINTN i = 0; i < n * PMM_PAGE_SIZE; i++) {
        if (p[i] != (UINT8)((i * 31 + 7) & 0xFF)) { ok = 0; break; }
    }
    log_puts("[log] pmm.selftest: "); log_hex(a); log_puts(" x ");
    log_u64((UINT64)n); log_puts(" pages ");
    log_puts(ok ? "read/write verified\r\n" : "read/write MISMATCH\r\n");

    pmm_free_pages(a, n);
    return ok;
}

void pmm_report(void) {
    PMM_STATS s;
    pmm_stats(&s);
    con_kv("bitmap", s.BitmapBytes / 1024 ? s.BitmapBytes / 1024 : 1, "KB");
    con_puts("  covers       : "); con_u64(s.TotalPages);
    con_puts(" pages ("); con_u64(s.TotalPages * 4 / 1024); con_puts(" MB)\r\n");
    con_kv("allocatable", s.ManagedPages, "pages");
    con_kv("low reserved", s.ReserveLowPages, "pages (1MB)");
    con_puts("  free         : "); con_u64(s.FreePages);
    con_puts(" pages ("); con_u64(s.FreePages * 4 / 1024); con_puts(" MB usable)\r\n");
    con_kv("used", s.UsedPages, "pages");
    con_puts("  (used = firmware + bitmap itself + TNDDOS allocations)\r\n");
    con_puts("  ---- totals: alloc "); con_u64(s.AllocCount);
    con_puts(", free "); con_u64(s.FreeCount);
    con_puts(", fail "); con_u64(s.FailCount); con_puts("\r\n");
}
