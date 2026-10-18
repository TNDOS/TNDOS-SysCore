/* ============================================================================
 * blk —— 块设备层
 *
 * **这一层的存在理由只有一个**：让文件系统不再依赖固件。
 *
 * 在那之前，内核的 VFS 一直走固件的 SimpleFileSystem —— ExitBootServices
 * 之后它连同整块代码一起消失，于是 DIR 在退出后要么炸要么报错。
 *
 * 而 AHCI / NVMe 驱动**早就能读扇区了**，只是没地方交。这一层就是那个地方：
 * 驱动通过 API 的 blk_register 把「读扇区」交进来，文件系统从这里取。
 *
 * **驱动只管读扇区，不管文件系统**；文件系统只认扇区，不关心底下是 AHCI
 * 还是 NVMe。谁换谁都不影响对方。
 * ==========================================================================*/
#include "tnd.h"

static const TND_BLOCK *gBlk[TND_MAX_BLK];
static int gBlkCount;
static int blk_init(void) { gBlkCount = 0; return 1; }

const TND_MODULE_DEF gModBlk = { "blk", "block devices handed over by drivers", blk_init };

int blk_register(const TND_BLOCK *b) {
    int i;
    if (!b || !b->Read) return -1;
    /* **扇区大小现在恒为 512。** 不接受别的值 —— 悄悄按别的算会读错地方，
     * 而那比直接拒绝难查得多。 */
    if (b->SectorSize != 512) return -1;
    if (gBlkCount >= TND_MAX_BLK) return -2;
    for (i = 0; i < gBlkCount; i++)
        if (gBlk[i]->Name && b->Name && t_stricmp(gBlk[i]->Name, b->Name) == 0) return -3;

    gBlk[gBlkCount++] = b;
    log_puts("[log] blk: registered "); log_puts(b->Name ? b->Name : "?");
    log_puts("  sectors="); log_u64(b->Sectors); log_puts("\r\n");
    return 0;
}

int blk_count(void) { return gBlkCount; }

const TND_BLOCK *blk_at(int i) { return (i >= 0 && i < gBlkCount) ? gBlk[i] : 0; }

int blk_write(int idx, UINT64 lba, UINT32 count, const void *buf) {
    const TND_BLOCK *b = blk_at(idx);
    if (!b || !buf || !count) return -1;
    if (!b->Write) return -4;                                        /* 只读设备 */
    if (b->Sectors && lba + (UINT64)count > b->Sectors) return -2;
    return b->Write(lba, count, buf) == 0 ? 0 : -3;
}

int blk_read(int idx, UINT64 lba, UINT32 count, void *buf) {
    const TND_BLOCK *b = blk_at(idx);
    if (!b || !buf || !count) return -1;
    if (b->Sectors && lba + (UINT64)count > b->Sectors) return -2;   /* 出界不发 */
    return b->Read(lba, count, buf) == 0 ? 0 : -3;
}
