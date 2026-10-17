/* ============================================================================
 * fat —— FAT16 读取（**完全不碰固件**）
 *
 * 在这之前，内核里一切「文件」都是固件的 SimpleFileSystem 给的。
 * ExitBootServices 之后它没了，于是 DIR / TYPE / 跑程序全都没了。
 *
 * 这一层加上 blk 层，就是把那条路接回来：**驱动读扇区 -> blk -> fat**。
 *
 * 现在只做 **FAT16**，因为 ESP 就是 mkfat.ps1 生成的 FAT16。
 * FAT32 的结构差在根目录（一条簇链而不是固定区）和 FAT 项宽度，
 * 后面补的时候是加分支，不是重写。
 *
 * 挂载点靠**探测**：esp.img 里的文件系统不在 LBA 0，前面 1 MiB 是空的
 * （文件系统从 LBA 2048 开始）。发一个真实 GPT/MBR 分区表才行，
 * 但那是另一件事 —— 现在依次试几个常见起点，认得出就认。
 * ==========================================================================*/
#include "tnd.h"

#define FAT16 16

typedef struct {
    int    blk;
    UINT64 base;        /* 卷起始 LBA */
    UINT32 spc;         /* 每簇扇区数 */
    UINT32 rsvd;        /* 保留扇区 */
    UINT32 nfats;
    UINT32 rootEnt;     /* 根目录项数 */
    UINT32 fatsz;       /* 每 FAT 的扇区数 */
    UINT32 rootSecs;    /* 根目录占几个扇区 */
    UINT32 total;       /* 总扇区数 */
    UINT64 fatLba;
    UINT64 rootLba;
    UINT64 dataLba;
} FAT_VOL;

static FAT_VOL gV;
static int     gOk;
static UINT8   gSec[512];
static int     fat_init(void) { gOk = 0; return 1; }

const TND_MODULE_DEF gModFat = { "fat", "FAT16 reader over blk (no firmware)", fat_init };

int fat_ready(void) { return gOk; }

static UINT32 u16(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8); }
static UINT32 u32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

/* 试一个起点。成功返回 1。 */
static int try_mount(int blk, UINT64 base) {
    const UINT8 *b = gSec;
    UINT32 rootSecs;

    if (blk_read(blk, base, 1, gSec) != 0) return 0;

    /* 引导扇区的硬指纹：跳转指令 + 结束标记 55 AA */
    if (!(b[0] == 0xEB || b[0] == 0xE9)) return 0;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (u16(b + 11) != 512) return 0;                 /* 每扇区字节数 */
    if (b[13] == 0 || b[16] == 0) return 0;           /* 每簇扇区数 / FAT 个数 */

    gV.blk     = blk;
    gV.base    = base;
    gV.spc     = b[13];
    gV.rsvd    = u16(b + 14);
    gV.nfats   = b[16];
    gV.rootEnt = u16(b + 17);
    gV.total   = u16(b + 19);
    if (!gV.total) gV.total = u32(b + 32);
    gV.fatsz   = u16(b + 22);
    if (!gV.fatsz) return 0;                          /* 0 就是 FAT32 —— 这一版不认 */

    rootSecs   = (gV.rootEnt * 32 + 511) / 512;
    gV.rootSecs = rootSecs;
    gV.fatLba   = base + gV.rsvd;
    gV.rootLba  = gV.fatLba + (UINT64)gV.nfats * gV.fatsz;
    gV.dataLba  = gV.rootLba + rootSecs;

    /* 合理性：数据区得在卷内 */
    if (gV.dataLba >= base + gV.total) return 0;
    return 1;
}

/* 在某个块设备上找 FAT16 卷。起点依次试：0 / 2048 / 63 / 1。 */
int fat_mount(int blkIdx) {
    static const UINT64 probe[] = { 0, 2048, 63, 1 };
    int i;

    gOk = 0;
    if (!blk_at(blkIdx)) return 0;

    for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
        if (!try_mount(blkIdx, probe[i])) continue;
        gOk = 1;
        log_puts("[log] fat: mounted on blk "); log_u64((UINT64)blkIdx);
        log_puts(" at LBA "); log_u64(gV.base);
        log_puts("  clusters spc="); log_u64(gV.spc);
        log_puts("\r\n");
        return 1;
    }
    log_puts("[log] fat: no FAT16 volume found\r\n");
    return 0;
}

/* 8.3 名字打出来：8 字节主名（去尾空格）+ '.' + 3 字节扩展名 */
static void print_name(const UINT8 *e) {
    int i, last = -1;
    for (i = 0; i < 8; i++) if (e[i] != ' ') last = i;
    for (i = 0; i <= last; i++) con_putc((char)e[i]);
    last = -1;
    for (i = 8; i < 11; i++) if (e[i] != ' ') last = i;
    if (last >= 8) { con_putc('.'); for (i = 8; i <= last; i++) con_putc((char)e[i]); }
}

/* 把根目录列出来。**这一条走的是 blk + fat，一颗固件服务都不碰。** */
void fat_report(void) {
    UINT32 s, i;
    int n = 0;

    if (!gOk) { con_puts("  no FAT volume mounted\r\n"); return; }

    con_puts("\r\n  === root directory, read through blk + fat (no firmware) ===\r\n");
    con_puts("  volume starts at LBA ");
    con_u64(gV.base);
    con_puts(",   ");
    con_u64(gV.total);
    con_puts(" sectors\r\n\r\n");

    for (s = 0; s < gV.rootSecs; s++) {
        if (blk_read(gV.blk, gV.rootLba + s, 1, gSec) != 0) { con_puts("  read error\r\n"); return; }
        for (i = 0; i < 16; i++) {
            const UINT8 *e = gSec + i * 32;
            if (e[0] == 0x00) goto done;              /* 后面全空 */
            if (e[0] == 0xE5) continue;               /* 已删除 */
            if (e[11] == 0x0F) continue;              /* 长文件名项，先跳过 */
            if (e[11] & 0x08) continue;               /* 卷标 */

            con_puts("  ");
            print_name(e);
            if (e[11] & 0x10) con_puts("   <DIR>");
            else              { con_puts("   "); con_u64(u32(e + 28)); }
            con_puts("\r\n");
            n++;
        }
    }
done:
    con_puts("\r\n  ");
    con_u64((UINT64)n);
    con_puts(" entr");
    con_puts(n == 1 ? "y" : "ies");
    con_puts(", read with our own driver stack\r\n");
}
