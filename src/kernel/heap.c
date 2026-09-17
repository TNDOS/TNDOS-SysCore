/* ============================================================================
 * TNDDOS — 内核堆（M3）
 *
 * 建立在 PMM 之上。策略故意选最笨的那个：按地址排序的单链表 + 首次适配。
 * 分配时能切就切，释放时向前合并。
 *
 * 记账方式同样是故意的笨：gFreeBytes / gUsedBytes 每次操作后从链表重算一遍。
 * 增量维护省的那点时间是假的，账算错才是真的。等真有性能需求再换成边界标记。
 * ==========================================================================*/
#include "tnd.h"

#define HEAP_ALIGN     16
#define HEAP_MIN_CHUNK (64 * 1024)
#define HEAP_MAGIC     0x544E44484B424C4BULL   /* "TNDHBLK" */

typedef struct TND_BLK {
    UINT64 magic;
    UINTN  size;                 /* 数据区字节数（不含头部） */
    UINTN  free;
    struct TND_BLK *next;        /* 按地址升序 */
} TND_BLK;

#define HDR_SIZE ((UINTN)((sizeof(TND_BLK) + HEAP_ALIGN - 1) & ~(UINTN)(HEAP_ALIGN - 1)))
#define BLK_DATA(b) ((void *)((UINT8 *)(b) + HDR_SIZE))

static TND_BLK *gHead = 0;
static UINT64 gChunks = 0;
static UINT64 gUsedBytes = 0, gFreeBytes = 0, gPeakUsed = 0;
static UINT64 gAllocCount = 0, gFreeCount = 0;
static int    gReady = 0;

static void list_insert(TND_BLK *b) {
    if (!gHead || (UINTN)b < (UINTN)gHead) { b->next = gHead; gHead = b; return; }
    TND_BLK *p = gHead;
    while (p->next && (UINTN)p->next < (UINTN)b) p = p->next;
    b->next = p->next;
    p->next = b;
}

/* 合并相邻空闲块，然后按链表重算账目 */
static void list_merge_and_recount(void) {
    TND_BLK *b = gHead;
    while (b && b->next) {
        UINT8 *end = (UINT8 *)b + HDR_SIZE + b->size;
        if (b->free && b->next->free && end == (UINT8 *)b->next) {
            b->size += HDR_SIZE + b->next->size;
            b->next = b->next->next;
            continue;                      /* 不前进：可能还能继续往后合 */
        }
        b = b->next;
    }

    gFreeBytes = 0; gUsedBytes = 0;
    for (TND_BLK *q = gHead; q; q = q->next) {
        if (q->free) gFreeBytes += q->size;
        else         gUsedBytes += q->size;
    }
}

static int heap_grow(UINTN need) {
    UINTN bytes = need + HDR_SIZE + HEAP_ALIGN;
    UINTN pages, got;
    UINT64 addr;
    TND_BLK *b;

    if (bytes < HEAP_MIN_CHUNK) bytes = HEAP_MIN_CHUNK;
    pages = (bytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    addr = pmm_alloc_pages(pages);
    if (!addr) return 0;
    got = pages * (UINTN)EFI_PAGE_SIZE;
    if (got <= HDR_SIZE) return 0;

    b = (TND_BLK *)(UINTN)addr;
    b->magic = HEAP_MAGIC;
    b->size = got - HDR_SIZE;
    b->free = 1;
    b->next = 0;
    list_insert(b);
    gChunks++;
    list_merge_and_recount();
    log_puts("[log] heap: grew by "); log_u64((UINT64)pages);
    log_puts(" pages @ "); log_hex(addr); log_puts("\r\n");
    return 1;
}

const TND_MODULE_DEF gModHeap = { "heap", "kernel heap kmalloc / kfree", heap_init };

int heap_init(void) {
    gHead = 0; gChunks = 0;
    gUsedBytes = gFreeBytes = gPeakUsed = 0;
    gAllocCount = gFreeCount = 0;
    if (!heap_grow(HEAP_MIN_CHUNK)) { log_kv("heap", "initial grow failed"); return 0; }
    gReady = 1;
    log_kv_u64("heap.freeBytes", gFreeBytes);
    return 1;
}

void *kmalloc(UINTN size) {
    if (!gReady || !size) return 0;
    size = (size + HEAP_ALIGN - 1) & ~(UINTN)(HEAP_ALIGN - 1);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (TND_BLK *b = gHead; b; b = b->next) {
            if (!b->free || b->size < size) continue;

            /* 够大就切一块出来，剩下的留作空闲块 */
            if (b->size >= size + HDR_SIZE + HEAP_ALIGN) {
                TND_BLK *nb = (TND_BLK *)((UINT8 *)b + HDR_SIZE + size);
                nb->magic = HEAP_MAGIC;
                nb->size = b->size - size - HDR_SIZE;
                nb->free = 1;
                nb->next = b->next;
                b->next = nb;
                b->size = size;
            }
            b->free = 0;
            list_merge_and_recount();
            if (gUsedBytes > gPeakUsed) gPeakUsed = gUsedBytes;
            gAllocCount++;
            return BLK_DATA(b);
        }
        if (attempt == 0 && !heap_grow(size)) return 0;
    }
    return 0;
}

void kfree(void *p) {
    TND_BLK *b;
    if (!gReady || !p) return;
    b = (TND_BLK *)((UINT8 *)p - HDR_SIZE);
    if (b->magic != HEAP_MAGIC) { log_kv("heap", "kfree: bad magic, refused"); return; }
    if (b->free) { log_kv("heap", "kfree: double free, ignored"); return; }
    b->free = 1;
    gFreeCount++;
    list_merge_and_recount();
}

void heap_stats(HEAP_STATS *o) {
    if (!o) return;
    o->Chunks = gChunks;
    o->BlockCount = 0; o->FreeBlocks = 0;
    for (TND_BLK *q = gHead; q; q = q->next) { o->BlockCount++; if (q->free) o->FreeBlocks++; }
    o->UsedBytes = gUsedBytes;
    o->FreeBytes = gFreeBytes;
    o->PeakUsed = gPeakUsed;
    o->AllocCount = gAllocCount;
    o->FreeCount = gFreeCount;
}

/* 自检：分配三块 -> 各写各的模式 -> 互相不串 -> 释放 -> 空闲量回得去 */
int heap_selftest(void) {
    HEAP_STATS before, after;
    UINT8 *a, *b, *c;
    int ok = 1;

    heap_stats(&before);
    a = (UINT8 *)kmalloc(100);
    b = (UINT8 *)kmalloc(4000);
    c = (UINT8 *)kmalloc(64);
    if (!a || !b || !c) { log_kv("heap.selftest", "allocation failed"); return 0; }

    for (int i = 0; i < 100;  i++) a[i] = 0xAA;
    for (int i = 0; i < 4000; i++) b[i] = 0xBB;
    for (int i = 0; i < 64;   i++) c[i] = 0xCC;
    for (int i = 0; i < 100;  i++) if (a[i] != 0xAA) ok = 0;
    for (int i = 0; i < 4000; i++) if (b[i] != 0xBB) ok = 0;
    for (int i = 0; i < 64;   i++) if (c[i] != 0xCC) ok = 0;

    if (((UINTN)a & (HEAP_ALIGN - 1)) || ((UINTN)b & (HEAP_ALIGN - 1)) || ((UINTN)c & (HEAP_ALIGN - 1))) {
        log_kv("heap.selftest", "returned pointer not aligned"); ok = 0;
    }

    kfree(b); kfree(a); kfree(c);
    heap_stats(&after);

    if (before.FreeBytes && after.FreeBytes < before.FreeBytes) {
        log_kv("heap.selftest", "free bytes decreased after free"); ok = 0;
    }

    log_puts("[log] heap.selftest: three blocks 100/4000/64 bytes, ");
    log_puts(ok ? "write-read-free all passed\r\n" : "verification FAILED\r\n");
    return ok;
}

void heap_report(void) {
    HEAP_STATS s;
    heap_stats(&s);
    con_kv("grow count", s.Chunks, "times");
    con_kv("blocks", s.BlockCount, "blocks");
    con_kv("free blocks", s.FreeBlocks, "blocks");
    con_puts("  used bytes    : "); con_u64(s.UsedBytes); con_puts("\r\n");
    con_puts("  free bytes    : "); con_u64(s.FreeBytes);
    con_puts("  ("); con_u64(s.FreeBytes / 1024); con_puts(" KB)\r\n");
    con_puts("  peak used     : "); con_u64(s.PeakUsed); con_puts("\r\n");
    con_puts("  ---- totals: kmalloc "); con_u64(s.AllocCount);
    con_puts(" calls, kfree "); con_u64(s.FreeCount); con_puts(" calls\r\n");
}
