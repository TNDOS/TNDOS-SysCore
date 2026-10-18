/* ============================================================================
 * fat —— 文件系统层（**现在是 FatFs 的外壳**）
 *
 * 历史：这一层原来是自己写的 FAT16 读取器。
 * 它证明了"驱动读扇区 -> blk -> 文件系统"这条链是通的，但只认 FAT16 —
 * 而真机的 ESP 是 FAT32，所以一上真机就"目录全无"。
 *
 * 现在换成 FatFs R0.16（BSD 风格，见 docs/THIRD-PARTY.md）：
 *   · FAT12/16/32 全支持
 *   · 子目录和 f_chdir/f_getcwd 都有（用户要求 cd 不能被拦）
 *   · ff.c 一个字未改；接合层在 fatfs_glue.c
 *
 * **保留的**：找卷起点的那段（GPT / MBR / 探测）。
 * FatFs 挂的是"驱动器"，而我们挂的是 GPT 分区 ——
 * FatFs 的 FF_MULTI_PARTITION 只认 MBR，GPT 盘上它看到的是保护性 MBR，
 * 用不了。所以起点由这里找出来，写进 gFatVolBase，胶水层的 disk_read 会加上它。
 *
 * 对外接口不变：fat_ready / fat_mount / fat_report。上层一行都不用改。
 * ==========================================================================*/
#include "tnd.h"
#include "ff.h"
#include "fatfs_tnd.h"      /* 只为了 gFatVolBase 的声明 */

static FATFS gFs;
static int   gMounted = 0;

static int fat_init(void) { gMounted = 0; return 1; }

const TND_MODULE_DEF gModFat = { "fat", "FatFs R0.16 over blk (no firmware)", fat_init };

int fat_ready(void) { return gMounted; }

/* ------------------------------------------------------------ 卷起点
 *
 * 为什么不能靠猜：原来这里是 probe[] = {0, 2048, 63, 1}，
 * QEMU 里那个 esp.img 恰好从 2048 开始所以能挂上；
 * 真机上 ESP 的起点由分区表决定，猜不中就挂不上 ——
 * 而症状只是"根目录列不出来"，完全看不出是起点找错了。
 * 顺序：GPT -> MBR -> 最后才退回探测。 */
static UINT32 rd32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}
static UINT64 rd64(const UINT8 *p) {
    return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32);
}
static int eq16(const UINT8 *a, const UINT8 *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* GPT 磁盘上 EFI System Partition 的类型 GUID（磁盘字节序） */
static const UINT8 kEspGuid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static UINT64 find_esp_gpt(int blkIdx) {
    static UINT8 hdr[512];
    static UINT8 ents[512 * 8];
    UINT64 entLba;
    UINT32 n, entSize, total, sectors, i;

    if (!blk_read(blkIdx, 1, 1, hdr)) return 0;
    if (hdr[0] != 'E' || hdr[1] != 'F' || hdr[2] != 'I' || hdr[3] != ' ') return 0;
    if (hdr[4] != 'P' || hdr[5] != 'A' || hdr[6] != 'R' || hdr[7] != 'T') return 0;

    entLba  = rd64(hdr + 72);
    n       = rd32(hdr + 80);
    entSize = rd32(hdr + 84);
    if (entSize < 128 || entSize > 512 || n == 0) return 0;
    if (n > 128) n = 128;

    total = n * entSize;
    if (total > sizeof(ents)) total = sizeof(ents);
    sectors = (total + 511) / 512;
    if (!blk_read(blkIdx, entLba, sectors, ents)) return 0;

    for (i = 0; i < n; i++) {
        const UINT8 *e = ents + i * entSize;
        if (!eq16(e, kEspGuid)) continue;
        { UINT64 s = rd64(e + 32); if (s) return s; }
    }
    return 0;
}

static UINT64 find_fat_mbr(int blkIdx) {
    static UINT8 mbr[512];
    int i;
    if (!blk_read(blkIdx, 0, 1, mbr)) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (i = 0; i < 4; i++) {
        const UINT8 *e = mbr + 446 + i * 16;
        UINT8 t = e[4];
        UINT64 s;
        if (t != 0xEF && t != 0x0C && t != 0x0B && t != 0x07) continue;
        s = rd32(e + 8);
        if (s) return s;
    }
    return 0;
}

/* 试一个起点：设好卷基址，让 FatFs 挂一次试试 */
static int try_mount_at(int blkIdx, UINT64 lba, const char *how) {
    FRESULT fr;

    gFatVolBase = lba;
    fr = f_mount(&gFs, "0:", 1);
    if (fr != FR_OK) {
        f_mount(0, "0:", 0);
        return 0;
    }
    gMounted = 1;
    log_puts("[log] fat: mounted on blk "); log_u64((UINT64)blkIdx);
    log_puts(" at LBA "); log_u64(lba);
    log_puts(" (via "); log_puts(how); log_puts(")  FatFs\r\n");
    return 1;
}

int fat_mount(int blkIdx) {
    static const UINT64 probe[] = { 0, 2048, 63, 1 };
    UINT64 start;
    unsigned i;

    gMounted = 0;
    if (!blk_at(blkIdx)) return 0;

    start = find_esp_gpt(blkIdx);
    if (start) {
        log_puts("[log] fat: GPT says the ESP starts at LBA "); log_u64(start); log_puts("\r\n");
        if (try_mount_at(blkIdx, start, "GPT")) return 1;
    }
    start = find_fat_mbr(blkIdx);
    if (start) {
        log_puts("[log] fat: MBR says a FAT partition starts at LBA "); log_u64(start); log_puts("\r\n");
        if (try_mount_at(blkIdx, start, "MBR")) return 1;
    }
    for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        if (try_mount_at(blkIdx, probe[i], "probe")) return 1;
    }
    log_puts("[log] fat: no FAT volume found (no GPT, no MBR, no probe hit)\r\n");
    return 0;
}

/* ------------------------------------------------------------ 列目录
 *
 * 用 f_opendir / f_readdir。**先列子目录再列文件** —— DOS 的 dir 就是这么排的，
 * 而且这样一眼能看出层级。 */
static int list_dir(const char *path) {
    DIR dir;
    FILINFO fi;
    FRESULT fr;
    int nDir = 0, nFile = 0;
    UINT64 bytes = 0;

    fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        con_puts("  cannot open directory, FRESULT=");
        con_u64((UINT64)fr);
        con_puts("\r\n");
        return 0;
    }

    for (;;) {
        UINT64 sz;
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK || fi.fname[0] == 0) break;
        if (!t_strcmp(fi.fname, ".") || !t_strcmp(fi.fname, "..")) continue;

        con_puts("  ");
        con_puts(fi.fname);
        if (fi.fattrib & AM_DIR) {
            con_puts("   <DIR>\r\n");
            nDir++;
        } else {
            /* 右对齐到第 34 列，像 DOS 那样好读 */
            UINTN len = t_strlen(fi.fname);
            UINTN col = 2 + len;
            while (col < 34) { con_putc(' '); col++; }
            sz = (UINT64)fi.fsize;
            con_u64(sz);
            con_puts("\r\n");
            nFile++;
            bytes += sz;
        }
    }
    f_closedir(&dir);

    con_puts("\r\n  ");
    con_u64((UINT64)nFile);
    con_puts(" file(s)   ");
    con_u64(bytes);
    con_puts(" bytes   ");
    con_u64((UINT64)nDir);
    con_puts(" dir(s)\r\n");
    return 1;
}

/* ============================================================================
 * fat_selftest —— 读写自检
 *
 * 建文件 -> 写 -> 关 -> 重开读回 -> 逐字节比对 -> 再列一次目录。
 *
 * 为什么要有这一条：读通路（dir 能列）和写通路（命令能下去）各自"看起来通了"
 * 不等于拼起来能用。只有"写进去、读回来、一模一样"才算数。
 *
 * **只写这一个文件名**（/TNDTEST.TXT），而且每次都 CREATE_ALWAYS ——
 * 万一在真机上误跑，也只有一个文件会被覆盖。 */
int fat_selftest(void) {
    FIL   f;
    FRESULT fr;
    UINT  bw = 0, br = 0;
    static const char kMsg[] = "TNDDOS write test: hello from our own driver stack.\r\n";
    char  rd[128];
    UINTN i, n;
    int   same = 1;

    if (!gMounted) { con_puts("  FATWRITE: no volume mounted\r\n"); return 0; }

    con_puts("\r\n  [FATWRITE] create + write + read back + compare\r\n");

    fr = f_open(&f, "/TNDTEST.TXT", FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { con_puts("  f_open(create) FRESULT="); con_u64((UINT64)fr); con_puts("\r\n"); return 0; }
    con_puts("  1 create              OK\r\n");

    n = (UINTN)t_strlen(kMsg);
    fr = f_write(&f, kMsg, (UINT)n, &bw);
    if (fr != FR_OK || bw != n) {
        con_puts("  f_write FRESULT="); con_u64((UINT64)fr);
        con_puts(" wrote="); con_u64((UINT64)bw); con_puts("/"); con_u64((UINT64)n); con_puts("\r\n");
        f_close(&f);
        return 0;
    }
    con_puts("  2 write               "); con_u64((UINT64)bw); con_puts(" bytes\r\n");

    fr = f_close(&f);
    if (fr != FR_OK) { con_puts("  f_close FRESULT="); con_u64((UINT64)fr); con_puts("\r\n"); return 0; }
    con_puts("  3 close               OK\r\n");

    fr = f_open(&f, "/TNDTEST.TXT", FA_READ);
    if (fr != FR_OK) { con_puts("  f_open(read) FRESULT="); con_u64((UINT64)fr); con_puts("\r\n"); return 0; }
    fr = f_read(&f, rd, sizeof(rd) - 1, &br);
    f_close(&f);
    if (fr != FR_OK) { con_puts("  f_read FRESULT="); con_u64((UINT64)fr); con_puts("\r\n"); return 0; }
    con_puts("  4 read back           "); con_u64((UINT64)br); con_puts(" bytes\r\n");

    if (br != n) same = 0;
    else for (i = 0; i < n; i++) if (rd[i] != kMsg[i]) { same = 0; break; }

    con_puts("  5 compare             ");
    con_puts(same ? "** MATCH **\r\n" : "** MISMATCH **\r\n");

    /* 最后再列一次目录 —— 确认这个文件真的进了文件系统，而不是只在缓存里 */
    con_puts("  6 directory now:\r\n");
    (void)list_dir("/");

    return same;
}

/* 列目录的输出格式 —— 按 DOS 的样子来，不要中间那些调试信息。
 *
 * 原来的开头是：
 *   === directory listing, read through blk + FatFs (no firmware) ===
 *   volume starts at LBA 0
 * 那两行对我们有用，对看屏幕的人没用。DOS 的 dir 第一行是卷标和目录名，
 * 最后一行是统计。**把调试信息让到串口去，屏幕上只留人要看的东西。** */
/* 列一个目录。path == 0 表示**当前目录**（"."）。
 *
 * 这里踩过一个难看的错：原来写死 f_opendir("/")，于是不管你在哪个目录，
 * dir 列出来的永远是根目录 —— 而表头上还写着 "Directory of C:\"，
 * 看起来像是对的。**命令行列出错目录比报错危险得多**：报错你知道有问题，
 * 列错目录你会照着它做决定。 */
/* ---------------------------------------------------------------- dir 的格式
 *
 * 照 cmd 的 dir 来。之前的版本只有"名字 + 大小"，可读但敷衍。
 * 该有的：卷标、卷序列号、日期时间、千分位、对齐、剩余空间。
 * 这些都来自 BPB 和 FILINFO，读一次就有，不是造出来的。 */

static void print_commas(UINT64 v) {
    char b[32];
    UINTN n, i;
    t_utoa(v, b);
    n = t_strlen(b);
    for (i = 0; i < n; i++) {
        if (i && ((n - i) % 3) == 0) con_putc(',');
        con_putc(b[i]);
    }
}

/* 卷标 + 序列号 + 剩余空间。序列号在 BPB 偏移 39（4 字节）。 */
static void print_volume_header(char *label, UINTN cap, UINT32 *serial, UINT64 *freeBytes) {
    static UINT8 sec[512];
    FATFS *fs = 0;
    DWORD  nfree = 0;
    UINTN  i = 0;

    label[0] = 0;
    (void)f_getlabel("", label, (DWORD *)&i);
    *serial = 0;
    if (blk_read(0, gFatVolBase + 39 / 512, 1, sec) == 0) {
        UINTN off = 39 % 512;
        *serial = (UINT32)sec[off] | ((UINT32)sec[off + 1] << 8)
                | ((UINT32)sec[off + 2] << 16) | ((UINT32)sec[off + 3] << 24);
    }
    *freeBytes = 0;
    if (f_getfree("", &nfree, &fs) == FR_OK && fs) {
        *freeBytes = (UINT64)nfree * fs->csize * 512ULL;
    }

    con_puts("\r\n Volume in drive C");
    if (label[0]) { con_puts(" is "); con_puts(label); }
    con_puts("\r\n Volume Serial Number is ");
    {
        static const char hx[] = "0123456789ABCDEF";
        UINTN g;
        for (g = 0; g < 4; g++) con_putc(hx[(*serial >> (12 - g * 4)) & 0xF]);
        con_putc('-');
        for (g = 0; g < 4; g++) con_putc(hx[(*serial >> (12 - g * 4)) & 0xF]);
    }
    con_puts("\r\n");
}

/* 一个条目：日期 时间  大小/<DIR>  名字 */
static void print_entry(const FILINFO *fi, int isDir) {
    UINTN y, m, d, hh, mm, len;

    y = 1980 + ((fi->fdate >> 9) & 0x7F);
    m = (fi->fdate >> 5) & 0x0F;
    d = fi->fdate & 0x1F;
    hh = (fi->ftime >> 11) & 0x1F;
    mm = (fi->ftime >> 5) & 0x3F;

    if (m  < 1) m = 1;  if (m  > 12) m = 12;
    if (d  < 1) d = 1;  if (d  > 31) d = 31;
    if (hh > 23) hh = 23;
    if (mm > 59) mm = 59;

    { char b[8]; t_utoa(y, b); while (t_strlen(b) < 4) { /* 补零 */ break; }
      /* 年补到 4 位 */
      if (t_strlen(b) < 4) con_putc('0');
      con_puts(b); }
    con_putc('/');
    if (m < 10) con_putc('0'); { char b[8]; t_utoa(m, b); con_puts(b); }
    con_putc('/');
    if (d < 10) con_putc('0'); { char b[8]; t_utoa(d, b); con_puts(b); }
    con_puts("  ");
    if (hh < 10) con_putc('0'); { char b[8]; t_utoa(hh, b); con_puts(b); }
    con_putc(':');
    if (mm < 10) con_putc('0'); { char b[8]; t_utoa(mm, b); con_puts(b); }
    con_puts("   ");

    if (isDir) {
        con_puts("<DIR>          ");
    } else {
        char b[32];
        t_utoa((UINT64)fi->fsize, b);
        len = t_strlen(b);
        { UINTN commas = (len - 1) / 3; UINTN vis = len + commas; while (vis++ < 14) con_putc(' '); }
        print_commas((UINT64)fi->fsize);
        con_putc(' ');
    }
    con_puts(fi->fname);
    con_puts("\r\n");
}

static void list_one(const char *path) {
    DIR   dir;
    FILINFO fi;
    FRESULT fr;
    int   nDir = 0, nFile = 0;
    UINT64 bytes = 0;
    char   cwd[TND_MAX_PATH];
    UINT64 freeBytes = 0;
    char   label[24];
    UINT32 serial = 0;

    if (!path) {
        if (f_getcwd(cwd, (UINT)sizeof(cwd)) != FR_OK) t_strncpy(cwd, "\\", sizeof(cwd));
        path = cwd;
    }

    print_volume_header(label, sizeof(label), &serial, &freeBytes);

    con_puts(" Directory of C:");
    con_puts(path);
    con_puts("\r\n\r\n");

    fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        con_puts("  cannot open directory, FRESULT=");
        con_u64((UINT64)fr);
        con_puts("\r\n\r\n");
        return;
    }

    /* **先子目录、再文件** —— cmd 的 dir 就是这个顺序 */
    for (;;) {
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK || fi.fname[0] == 0) break;
        if (!t_strcmp(fi.fname, ".") || !t_strcmp(fi.fname, "..")) continue;
        if (!(fi.fattrib & AM_DIR)) continue;
        print_entry(&fi, 1);
        nDir++;
    }
    f_closedir(&dir);

    fr = f_opendir(&dir, path);
    if (fr == FR_OK) {
        for (;;) {
            fr = f_readdir(&dir, &fi);
            if (fr != FR_OK || fi.fname[0] == 0) break;
            if (!t_strcmp(fi.fname, ".") || !t_strcmp(fi.fname, "..")) continue;
            if (fi.fattrib & AM_DIR) continue;
            print_entry(&fi, 0);
            nFile++;
            bytes += (UINT64)fi.fsize;
        }
        f_closedir(&dir);
    }

    /* 统计 —— 和 cmd 一样，右对齐到第 30 列左右 */
    {
        char nb[32];
        UINTN l;
        t_utoa((UINT64)nFile, nb);
        con_puts("\r\n");
        l = t_strlen(nb);
        while (l++ < 16) con_putc(' ');
        con_puts(nb); con_puts(" File(s)");
        l = 0; { char b2[32]; t_utoa(bytes, b2); l = t_strlen(b2); }
        while (l++ < 16) con_putc(' ');
        print_commas(bytes); con_puts(" bytes\r\n");

        { char b3[32]; t_utoa((UINT64)nDir, b3); l = t_strlen(b3); }
        while (l++ < 16) con_putc(' ');
        { char b4[32]; t_utoa((UINT64)nDir, b4); con_puts(b4); }
        con_puts(" Dir(s)");
        l = 0; { char b5[32]; t_utoa(freeBytes, b5); l = t_strlen(b5); }
        while (l++ < 16) con_putc(' ');
        print_commas(freeBytes); con_puts(" bytes free\r\n\r\n");
    }
}

/* 对外两个入口：fat_report 列当前目录，fat_list 列指定目录。
 * **不带参数的那个也要走"当前目录"** —— 见 list_one 上面那段注释。 */
void fat_report(void) { if (gMounted) list_one(0); else con_puts("  no FAT volume mounted\r\n"); }
void fat_list(const char *path) { if (gMounted) list_one(path); else con_puts("  no FAT volume mounted\r\n"); }