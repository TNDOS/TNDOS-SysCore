/* ============================================================================
 * fat —— 文件系统层（**自己写的 FAT12/16/32 驱动**）
 *
 * 历史：原来是自写的 FAT16 读取器 -> 换成 FatFs R0.16 -> 现在又换回自己写。
 *
 * 为什么放弃 FatFs：用内存快照对比发现，一个 27 KB 的 TNX 程序从盘上读进来之后，
 * 缓冲区里的内容和磁盘上的文件对不上。而加载器自己的 src == dst（所有采样点一致），
 * 说明拷贝没问题 —— 错的是 fat_read_alloc 交出来的数据。
 * 小程序 6~7 KB 正常，大程序必崩：这就是「第 N 个程序之后才崩」「MORE 打不开文件」
 * 的总根源。
 *
 * 与其在一个第三方移植层里找，不如自己写 —— 这一层本来就只需要扇区读写。
 *
 * 支持 FAT12 / FAT16 / FAT32。目录项只解析 8.3 短名；LFN 项跳过不显示。
 * **对外接口保持原样**，上层（shell / vfs / api / conf / tnx）一行不用改。
 * ==========================================================================*/
#include "tnd.h"

/* ------------------------------------------------------------------ 盘上结构 */
typedef struct {
    UINT8  Name[11];
    UINT8  Attr;
    UINT8  NtRes;
    UINT8  CrtTenth;
    UINT16 CrtTime, CrtDate, LstAccDate;
    UINT16 FstClusHI;
    UINT16 WrtTime, WrtDate;
    UINT16 FstClusLO;
    UINT32 FileSize;
} __attribute__((packed)) FAT_DIRENT;      /* 恰好 32 字节 */

#define ATTR_RO      0x01u
#define ATTR_HIDDEN  0x02u
#define ATTR_SYSTEM  0x04u
#define ATTR_VOLUME  0x08u
#define ATTR_DIR     0x10u
#define ATTR_ARCH    0x20u
#define ATTR_LFN     0x0Fu

#define DIRENT_FREE  0xE5u
#define DIRENT_END   0x00u

/* ------------------------------------------------------------------ 卷状态 */
static struct {
    int     ok;
    int     blk;
    UINT64  base;
    UINT32  bps, spc, rsvd, nfats;
    UINT32  rootEnts;
    UINT32  totalSec, fatSec, rootSec;
    UINT32  rootClus;
    UINT64  fatLba, rootLba, dataLba;
    UINT32  nClusters;
    int     type;
} gV;

static int gMounted = 0;

static int fat_init(void) { gMounted = 0; gV.ok = 0; return 1; }

const TND_MODULE_DEF gModFat = { "fat", "native FAT12/16/32 over blk", fat_init };

int fat_ready(void) { return gMounted; }

/* ------------------------------------------------------------------ 小工具 */
static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 rd32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}
static UINT64 rd64(const UINT8 *p) { return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32); }
static void wr16(UINT8 *p, UINT16 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); }
static void wr32(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24);
}
static int eq16(const UINT8 *a, const UINT8 *b) {
    int i; for (i = 0; i < 16; i++) if (a[i] != b[i]) return 0; return 1;
}

/* 扇区缓冲：这一层不做并发，静态一个够用，省掉每次 kmalloc */
static UINT8 gSec[512] __attribute__((aligned(16)));

/* **blk 那一层的约定是"成功返回 0"，这一层内部统一用"成功返回 1"。**
 * 只在这里转一次，别让两种约定在文件里混着走 —— 混着走就会写出
 * `if (!blk_read(...)) return 0;` 这种"一成功就判失败"的代码。 */
static int sec_read(UINT64 relLba, void *buf) {
    if (!gV.ok) return 0;
    return blk_read(gV.blk, gV.base + relLba, 1, buf) == 0;
}
static int sec_write(UINT64 relLba, const void *buf) {
    if (!gV.ok) return 0;
    return blk_write(gV.blk, gV.base + relLba, 1, buf) == 0;
}

static UINT64 cl_to_lba(UINT32 cl) {
    return gV.dataLba + (UINT64)(cl - 2) * gV.spc;
}
static int cl_valid(UINT32 cl) {
    return cl >= 2 && cl < (2u + gV.nClusters);
}

/* ------------------------------------------------------------ FAT 表项 */
/* FAT12 是 12 位紧密打包的：两个表项占 3 字节，且**可能跨扇区边界**。
 * 这里不假设对齐，按字节偏移逐字节取，跨扇区就再读一个扇区。 */
static int fat_get(UINT32 cl, UINT32 *out) {
    UINT32 off, lba;

    if (!gV.ok || !out) return -1;

    if (gV.type == 12)      off = cl + (cl >> 1);
    else if (gV.type == 16) off = cl * 2u;
    else                    off = cl * 4u;

    lba = (UINT32)(gV.fatLba + (off / gV.bps));
    if (!sec_read(lba, gSec)) return -1;

    if (gV.type == 32) { *out = rd32(gSec + (off % gV.bps)) & 0x0FFFFFFFu; return 0; }
    if (gV.type == 16) { *out = rd16(gSec + (off % gV.bps)); return 0; }

    {
        UINT32 insec = off % gV.bps;
        UINT8  b[3];
        UINT32 i;
        for (i = 0; i < 3; i++) {
            UINT32 o = insec + i;
            if (o >= gV.bps) {
                if (!sec_read(lba + 1, gSec)) return -1;
                insec = 0;
                b[i] = gSec[0];
                continue;
            }
            b[i] = gSec[o];
        }
        {
            UINT32 v = (UINT32)b[0] | ((UINT32)b[1] << 8) | ((UINT32)b[2] << 16);
            *out = (cl & 1u) ? (v >> 4) : (v & 0x0FFFu);
        }
    }
    return 0;
}

static int fat_is_eoc(UINT32 v) {
    if (gV.type == 12) return v >= 0x0FF8u;
    if (gV.type == 16) return v >= 0xFFF8u;
    return v >= 0x0FFFFFF8u;
}

/* ------------------------------------------------------------ 名字转换 */
static void name_from_dirent(const FAT_DIRENT *e, char *out, UINTN cap) {
    UINTN j = 0;
    int   i;
    for (i = 0; i < 8 && j + 1 < cap; i++) {
        if (e->Name[i] == ' ') break;
        out[j++] = (char)e->Name[i];
    }
    if (e->Name[8] != ' ' && j + 1 < cap) {
        out[j++] = '.';
        for (i = 8; i < 11 && j + 1 < cap; i++) {
            if (e->Name[i] == ' ') break;
            out[j++] = (char)e->Name[i];
        }
    }
    out[j] = 0;
}

static void name_to_dirent(const char *name, UINT8 out[11]) {
    int i = 0, j;
    for (j = 0; j < 11; j++) out[j] = ' ';
    j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[j++] = (UINT8)c;
    }
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        for (j = 8; j < 11 && name[i]; j++, i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[j] = (UINT8)c;
        }
    }
}

static int name_eq(const FAT_DIRENT *e, const char *name) {
    UINT8 want[11];
    int i;
    name_to_dirent(name, want);
    for (i = 0; i < 11; i++) if (e->Name[i] != want[i]) return 0;
    return 1;
}

static UINT32 dirent_cluster(const FAT_DIRENT *e) {
    return ((UINT32)e->FstClusHI << 16) | (UINT32)e->FstClusLO;
}
/* ------------------------------------------------------------ 挂载 */
/* 卷起点：GPT -> MBR -> 探测。这一段沿用原来的做法，它一直是对的。 */
static const UINT8 kEspGuid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static UINT64 find_esp_gpt(int blkIdx) {
    static UINT8 hdr[512];
    static UINT8 ents[512 * 8];
    UINT64 entLba;
    UINT32 n, entSize, total, sectors, i;

    if (blk_read(blkIdx, 1, 1, hdr) != 0) return 0;
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
    if (blk_read(blkIdx, entLba, sectors, ents) != 0) return 0;

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
    if (blk_read(blkIdx, 0, 1, mbr) != 0) return 0;
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

/* 解析 BPB。成功返回 1。 */
static int parse_bpb(int blkIdx, UINT64 base) {
    UINT8  bpb[512];
    UINT32 fatSz16, fatSz32, totalSec16, totalSec32;
    UINT32 rootSec, dataSec, clusters;

    if (blk_read(blkIdx, base, 1, bpb) != 0) { log_kv("fat.bpb", "read failed"); return 0; }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) { log_kv("fat.bpb", "no 55AA signature"); return 0; }

    gV.bps      = rd16(bpb + 11);
    gV.spc      = bpb[13];
    gV.rsvd     = rd16(bpb + 14);
    gV.nfats    = bpb[16];
    gV.rootEnts = rd16(bpb + 17);
    totalSec16  = rd16(bpb + 19);
    fatSz16     = rd16(bpb + 22);
    totalSec32  = rd32(bpb + 32);
    fatSz32     = rd32(bpb + 36);

    if (gV.bps != 512) { log_kv_u64("fat.bpb.bps", gV.bps); return 0; }
    if (gV.spc == 0 || (gV.spc & (gV.spc - 1))) { log_kv_u64("fat.bpb.spc", gV.spc); return 0; }
    if (gV.nfats == 0) { log_kv("fat.bpb", "nfats 0"); return 0; }

    gV.totalSec = totalSec16 ? totalSec16 : totalSec32;
    gV.fatSec   = fatSz16 ? fatSz16 : fatSz32;
    if (!gV.totalSec || !gV.fatSec) {
        log_kv_u64("fat.bpb.total", gV.totalSec);
        log_kv_u64("fat.bpb.fatsec", gV.fatSec);
        return 0;
    }

    rootSec = ((gV.rootEnts * 32u) + (gV.bps - 1)) / gV.bps;

    gV.fatLba  = gV.rsvd;
    gV.rootLba = gV.rsvd + (UINT64)gV.nfats * gV.fatSec;
    gV.dataLba = gV.rootLba + rootSec;

    if (gV.dataLba >= gV.totalSec) {
        log_kv_u64("fat.bpb.datalba", gV.dataLba);
        log_kv_u64("fat.bpb.total",   gV.totalSec);
        return 0;
    }
    dataSec  = gV.totalSec - (UINT32)gV.dataLba;
    clusters = dataSec / gV.spc;
    if (clusters == 0) { log_kv("fat.bpb", "zero clusters"); return 0; }

    gV.nClusters = clusters;
    gV.rootSec   = rootSec;

    if (clusters < 4085u)       gV.type = 12;
    else if (clusters < 65525u) gV.type = 16;
    else                        gV.type = 32;

    gV.rootClus = 0;
    if (gV.type == 32) {
        gV.rootClus = rd32(bpb + 44) & 0x0FFFFFFFu;
        if (!cl_valid(gV.rootClus)) { log_kv_u64("fat.bpb.rootclus", gV.rootClus); return 0; }
    }

    gV.blk  = blkIdx;
    gV.base = base;
    gV.ok   = 1;
    return 1;
}

int fat_mount(int blkIdx) {
    static const UINT64 probe[] = { 0, 2048, 63, 1 };
    UINT64 lba = 0;
    int i;

    gMounted = 0; gV.ok = 0;

    lba = find_esp_gpt(blkIdx);
    if (lba && parse_bpb(blkIdx, lba)) { gMounted = 1; goto done; }

    lba = find_fat_mbr(blkIdx);
    if (lba && parse_bpb(blkIdx, lba)) { gMounted = 1; goto done; }

    for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
        log_puts("[log] fat: try blk "); log_u64((UINT64)blkIdx);
        log_puts(" lba "); log_u64(probe[i]); log_puts("\r\n");
        if (parse_bpb(blkIdx, probe[i])) { gMounted = 1; lba = probe[i]; goto done; }
    }
    return 0;

done:
    log_puts("[log] fat: mounted on blk "); log_u64((UINT64)blkIdx);
    log_puts(" at LBA "); log_u64(lba);
    log_puts("  FAT"); log_u64((UINT64)gV.type);
    log_puts("  clusters "); log_u64((UINT64)gV.nClusters);
    log_puts("  spc "); log_u64((UINT64)gV.spc);
    log_puts("  rootEnts "); log_u64((UINT64)gV.rootEnts);
    log_puts("\r\n");
    return 1;
}

/* ------------------------------------------------------------ 路径规范化 */
/* 统一成 "\\A\\B\\C" 的绝对形式，只认反斜杠。空路径 = 根目录。 */
void fat_resolve(const char *in, char *out, UINTN cap) {
    UINTN j = 0;
    char  full[TND_MAX_PATH];

    if (!out || !cap) return;
    if (!in) in = "";

    /* **相对路径要先接上当前目录。**
     *
     * 这一层是无状态的，当前目录由 vfs 维护（vfs_cwd）。不接的话，
     * "HELLO.TXT" 会被解析成 "\\HELLO.TXT"（根目录），而不是
     * "\\EFI\\TNDOS\\HELLO.TXT"。
     *
     * 症状很难看出来：root 下的文件读得到，子目录里的读不到 ——
     * 而且固件那条老路还会兜底一部分，于是表现为"有的程序能跑有的不能"。 */
    if (in[0] != 92 && in[0] != 47) {
        const char *cwd = vfs_cwd();
        UINTN k = 0;
        while (cwd && *cwd && k + 1 < sizeof(full)) full[k++] = *cwd++;
        if (k + 1 < sizeof(full) && k > 0 && full[k - 1] != 92) full[k++] = 92;
        while (*in && k + 1 < sizeof(full)) full[k++] = *in++;
        full[k] = 0;
        in = full;
    }

    if (j + 1 < cap) out[j++] = 92;

    while (*in && j + 1 < cap) {
        char  seg[16];
        UINTN k = 0;

        while (*in == 92 || *in == 47) in++;
        while (*in && *in != 92 && *in != 47 && k + 1 < sizeof(seg)) seg[k++] = *in++;
        seg[k] = 0;
        if (!k) break;

        if (k == 1 && seg[0] == '.') continue;
        if (k == 2 && seg[0] == '.' && seg[1] == '.') {
            UINTN t = j;
            if (t > 1 && out[t - 1] == 92) t--;
            while (t > 1 && out[t - 1] != 92) t--;
            j = (t > 1) ? t : 1;
            out[j] = 0;
            continue;
        }

        if (j > 1 && out[j - 1] != 92) out[j++] = 92;
        { UINTN m; for (m = 0; m < k && j + 1 < cap; m++) out[j++] = seg[m]; }
    }
    if (j > 1 && out[j - 1] == 92) j--;
    out[j] = 0;
}
/* ------------------------------------------------------------ 目录遍历 */
typedef int (*DIR_CB)(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx);

/* 走一个目录的所有项。isRoot 且非 FAT32 时走固定根目录区。
 * cb 返回非 0 -> 提前停。返回 1 = 正常走完，0 = 读扇区失败。 */
static int dir_walk(UINT32 dirClus, int isRoot, DIR_CB cb, void *ctx) {
    UINT32 cl = dirClus;
    UINT32 guard = 0;

    if (isRoot && gV.type != 32) {
        UINT32 s;
        for (s = 0; s < gV.rootSec; s++) {
            UINT32 o;
            if (!sec_read(gV.rootLba + s, gSec)) return 0;
            for (o = 0; o + 32 <= gV.bps; o += 32) {
                const FAT_DIRENT *e = (const FAT_DIRENT *)(gSec + o);
                if (cb(e, gV.rootLba + s, o, ctx)) return 1;
            }
        }
        return 1;
    }

    while (cl_valid(cl) && guard++ < gV.nClusters + 2) {
        UINT32 s;
        for (s = 0; s < gV.spc; s++) {
            UINT32 o;
            if (!sec_read(cl_to_lba(cl) + s, gSec)) return 0;
            for (o = 0; o + 32 <= gV.bps; o += 32) {
                const FAT_DIRENT *e = (const FAT_DIRENT *)(gSec + o);
                if (cb(e, cl_to_lba(cl) + s, o, ctx)) return 1;
            }
        }
        {
            UINT32 nx;
            if (fat_get(cl, &nx) != 0) return 0;
            if (fat_is_eoc(nx)) break;
            cl = nx;
        }
    }
    return 1;
}

typedef struct {
    const char *want;
    FAT_DIRENT *out;
    UINT64     *outLba;
    UINT32     *outOff;
    int         found;
} LOOKUP_CTX;

static int lookup_cb(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx) {
    LOOKUP_CTX *k = (LOOKUP_CTX *)ctx;
    if (e->Name[0] == DIRENT_END)  return 1;      /* 目录到此结束 */
    if (e->Name[0] == DIRENT_FREE) return 0;
    if (e->Attr == ATTR_LFN)       return 0;
    if (e->Attr & ATTR_VOLUME)     return 0;
    if (!name_eq(e, k->want))      return 0;
    if (k->out)    t_memcpy(k->out, e, sizeof(FAT_DIRENT));
    if (k->outLba) *k->outLba = lba;
    if (k->outOff) *k->outOff = off;
    k->found = 1;
    return 1;
}

static int dir_lookup_in(UINT32 dirClus, int isRoot, const char *name,
                         FAT_DIRENT *out, UINT64 *outLba, UINT32 *outOff) {
    LOOKUP_CTX k;
    k.want = name; k.out = out; k.outLba = outLba; k.outOff = outOff; k.found = 0;
    if (!dir_walk(dirClus, isRoot, lookup_cb, &k)) return 0;
    return k.found;
}

/* 解析整条路径。返回 1 = 找到。
 * outParentClus / outParentIsRoot 给出**父目录**的位置（写操作用）。 */
static int path_lookup(const char *path, FAT_DIRENT *out, UINT64 *outLba, UINT32 *outOff,
                       UINT32 *outParentClus, int *outParentIsRoot) {
    char  buf[TND_MAX_PATH];
    char  part[16];
    UINTN i = 0;
    UINT32 curClus;
    int    curIsRoot = 1;

    fat_resolve(path, buf, sizeof(buf));
    if (buf[0] == 92 && buf[1] == 0) return 0;    /* 根目录本身没有目录项 */

    curClus = (gV.type == 32) ? gV.rootClus : 0;

    while (buf[i]) {
        UINTN j = 0;
        FAT_DIRENT e;
        UINT64 elba = 0;
        UINT32 eoff = 0;

        while (buf[i] == 92) i++;
        while (buf[i] && buf[i] != 92 && j + 1 < sizeof(part)) part[j++] = buf[i++];
        part[j] = 0;
        if (!j) break;

        if (outParentClus)   *outParentClus = curClus;
        if (outParentIsRoot) *outParentIsRoot = curIsRoot;

        if (!dir_lookup_in(curClus, curIsRoot, part, &e, &elba, &eoff)) return 0;

        if (buf[i] == 0) {
            if (out)    *out = e;
            if (outLba) *outLba = elba;
            if (outOff) *outOff = eoff;
            return 1;
        }
        if (!(e.Attr & ATTR_DIR)) return 0;
        curClus = dirent_cluster(&e);
        curIsRoot = 0;
    }
    return 0;
}

/* ------------------------------------------------------------ 读文件 */
/* 沿簇链把 want 字节读进 dst。返回 1 = 读满。 */
static int read_chain(UINT32 firstClus, UINT8 *dst, UINT32 want, UINT32 *gotOut) {
    UINT32 cl = firstClus;
    UINT32 done = 0, guard = 0;
    UINT32 clBytes = gV.spc * gV.bps;

    while (done < want && cl_valid(cl) && guard++ < gV.nClusters + 2) {
        UINT32 s;
        UINT32 take = want - done;
        if (take > clBytes) take = clBytes;
        for (s = 0; s < gV.spc && (s * gV.bps) < take; s++) {
            UINT32 n = take - s * gV.bps;
            if (n > gV.bps) n = gV.bps;
            if (!sec_read(cl_to_lba(cl) + s, gSec)) { if (gotOut) *gotOut = done; return 0; }
            t_memcpy(dst + done + s * gV.bps, gSec, n);
        }
        done += take;
        if (done >= want) break;
        {
            UINT32 nx;
            if (fat_get(cl, &nx) != 0) break;
            if (fat_is_eoc(nx)) break;
            cl = nx;
        }
    }
    if (gotOut) *gotOut = done;
    return done == want;
}

int fat_read_alloc(const char *path, void **out, UINTN *outLen) {
    FAT_DIRENT e;
    void  *buf;
    UINT32 got = 0;

    if (out) *out = 0;
    if (outLen) *outLen = 0;
    if (!gMounted || !out || !outLen) return -1;
    if (!path_lookup(path, &e, 0, 0, 0, 0)) {
        log_puts("[log] fat: lookup failed  "); log_puts(path); log_puts("\r\n");
        return -2;
    }
    if (e.Attr & ATTR_DIR)   { log_puts("[log] fat: is a dir  "); log_puts(path); log_puts("\r\n"); return -2; }
    if (e.FileSize == 0)     { log_puts("[log] fat: zero size  "); log_puts(path); log_puts("\r\n"); return -2; }
    if (e.FileSize > (UINT32)TNX_WINDOW_SIZE) { log_kv_u64("fat: too big", e.FileSize); return -2; }

    buf = kmalloc((UINTN)e.FileSize);
    if (!buf) return -3;

    if (!read_chain(dirent_cluster(&e), (UINT8 *)buf, e.FileSize, &got)) {
        kfree(buf);
        log_kv_u64("fat.read.want", e.FileSize);
        log_kv_u64("fat.read.got",  got);
        return -4;
    }
    *out = buf;
    *outLen = (UINTN)got;
    return 0;
}

int fat_read_text(const char *path, char *buf, UINTN cap, UINTN *outLen) {
    FAT_DIRENT e;
    UINT32 want, got = 0;

    if (outLen) *outLen = 0;
    if (!gMounted || !buf || !cap) return -1;
    if (!path_lookup(path, &e, 0, 0, 0, 0)) return -2;
    if (e.Attr & ATTR_DIR) return -2;

    want = e.FileSize;
    if (want > cap - 1) want = (UINT32)(cap - 1);
    if (!read_chain(dirent_cluster(&e), (UINT8 *)buf, want, &got)) return -4;
    buf[got] = 0;
    if (outLen) *outLen = (UINTN)got;
    return 0;
}

int fat_exists(const char *path) {
    char buf[TND_MAX_PATH];
    FAT_DIRENT e;
    fat_resolve(path, buf, sizeof(buf));
    if (buf[0] == 92 && buf[1] == 0) return 1;
    return path_lookup(path, &e, 0, 0, 0, 0) ? 1 : 0;
}
/* ------------------------------------------------------------ 打印 */
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

/* 剩余空间：数 FAT 里的空表项。第一次算完就缓存 —— 每次 dir 都扫一遍 FAT
 * 在 63K 簇的盘上要读 500 个扇区，没必要。 */
static UINT64 gFreeClusters = 0;
static int    gFreeValid = 0;

static UINT64 count_free_clusters(void) {
    UINT32 cl;
    UINT64 freeN = 0;
    if (gFreeValid) return gFreeClusters;
    for (cl = 2; cl < 2u + gV.nClusters; cl++) {
        UINT32 v = 0;
        if (fat_get(cl, &v) != 0) break;
        if (v == 0) freeN++;
    }
    gFreeClusters = freeN;
    gFreeValid = 1;
    return freeN;
}

/* 卷标探测用的回调（放在外面是因为 C 没有闭包） */
static char *gLabelOut = 0;
static UINTN gLabelCap = 0;
static int label_cb(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx) {
    (void)lba; (void)off; (void)ctx;
    if (e->Name[0] == DIRENT_END)  return 1;
    if (e->Name[0] == DIRENT_FREE) return 0;
    if (!(e->Attr & ATTR_VOLUME))  return 0;
    if (!gLabelOut || !gLabelCap)  return 1;
    {
        UINTN j = 0;
        int i;
        for (i = 0; i < 11 && j + 1 < gLabelCap; i++) {
            if (e->Name[i] == ' ') break;
            gLabelOut[j++] = (char)e->Name[i];
        }
        gLabelOut[j] = 0;
    }
    return 1;
}

static void get_label(char *out, UINTN cap) {
    out[0] = 0;
    gLabelOut = out; gLabelCap = cap;
    dir_walk((gV.type == 32) ? gV.rootClus : 0, 1, label_cb, 0);
    gLabelOut = 0; gLabelCap = 0;
}

static void print_volume_header(const char *path, UINT64 *freeBytes) {
    UINT8  sec[512];
    UINT32 serial = 0;
    char   label[24];

    get_label(label, sizeof(label));
    if (sec_read(0, sec))
        serial = rd32(sec + 39);

    *freeBytes = count_free_clusters() * (UINT64)gV.spc * gV.bps;

    con_puts("\r\n Volume in drive C");
    if (label[0]) { con_puts(" is "); con_puts(label); }
    con_puts("\r\n Volume Serial Number is ");
    {
        static const char hx[] = "0123456789ABCDEF";
        UINTN g;
        for (g = 0; g < 4; g++) con_putc(hx[(serial >> (12 - g * 4)) & 0xF]);
        con_putc('-');
        for (g = 0; g < 4; g++) con_putc(hx[(serial >> (12 - g * 4)) & 0xF]);
    }
    con_puts("\r\n");
    (void)path;
}

static void pad2(UINTN v) { if (v < 10) con_putc('0'); { char b[8]; t_utoa((UINT64)v, b); con_puts(b); } }

static void print_entry(const FAT_DIRENT *e, const char *name, int isDir) {
    UINT16 fdate = e->WrtDate, ftime = e->WrtTime;
    UINTN  y, m, d, hh, mm, len;

    y  = 1980 + ((fdate >> 9) & 0x7F);
    m  = (fdate >> 5) & 0x0F;
    d  = fdate & 0x1F;
    hh = (ftime >> 11) & 0x1F;
    mm = (ftime >> 5) & 0x3F;
    if (m  < 1) m = 1;  if (m  > 12) m = 12;
    if (d  < 1) d = 1;  if (d  > 31) d = 31;
    if (hh > 23) hh = 23;
    if (mm > 59) mm = 59;

    { char b[8]; t_utoa(y, b); len = t_strlen(b); while (len++ < 4) con_putc('0'); con_puts(b); }
    con_putc('/');  pad2(m);
    con_putc('/');  pad2(d);
    con_puts("  "); pad2(hh);
    con_putc(':');  pad2(mm);
    con_puts("   ");

    if (isDir) {
        con_puts("<DIR>          ");
    } else {
        char b[32];
        UINTN vis;
        t_utoa((UINT64)e->FileSize, b);
        len = t_strlen(b);
        vis = len + (len - 1) / 3;
        while (vis++ < 14) con_putc(' ');
        print_commas((UINT64)e->FileSize);
        con_putc(' ');
    }
    con_puts(name);
    con_puts("\r\n");
}

/* 列目录：先子目录、再文件 —— cmd 的 dir 就是这个顺序。
 * dir_walk 走两遍，用一个静态计数器分流。 */
static int  gWantDirs = 1;
static int  gNDir = 0, gNFile = 0;
static UINT64 gBytes = 0;

static int list_cb(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx) {
    char name[16];
    (void)lba; (void)off; (void)ctx;
    if (e->Name[0] == DIRENT_END)  return 1;
    if (e->Name[0] == DIRENT_FREE) return 0;
    if (e->Attr == ATTR_LFN)       return 0;
    if (e->Attr & ATTR_VOLUME)     return 0;
    name_from_dirent(e, name, sizeof(name));
    if (!t_strcmp(name, ".") || !t_strcmp(name, "..")) return 0;
    if (gWantDirs) {
        if (!(e->Attr & ATTR_DIR)) return 0;
        print_entry(e, name, 1);
        gNDir++;
    } else {
        if (e->Attr & ATTR_DIR) return 0;
        print_entry(e, name, 0);
        gNFile++;
        gBytes += (UINT64)e->FileSize;
    }
    return 0;
}

static void list_one(const char *path) {
    char   buf[TND_MAX_PATH];
    FAT_DIRENT e;
    UINT32 dirClus;
    int    isRoot;
    UINT64 freeBytes = 0;

    if (!path || !*path) fat_resolve("", buf, sizeof(buf));
    else                 fat_resolve(path, buf, sizeof(buf));

    /* 找到目标目录的起始簇 */
    if (buf[0] == 92 && buf[1] == 0) {
        dirClus = (gV.type == 32) ? gV.rootClus : 0;
        isRoot = 1;
    } else {
        if (!path_lookup(buf, &e, 0, 0, 0, 0) || !(e.Attr & ATTR_DIR)) {
            con_puts("  cannot open directory\r\n");
            return;
        }
        dirClus = dirent_cluster(&e);
        isRoot = 0;
    }

    print_volume_header(buf, &freeBytes);

    con_puts(" Directory of C:");
    con_puts(buf);
    con_puts("\r\n\r\n");

    gNDir = gNFile = 0; gBytes = 0;

    gWantDirs = 1;
    if (!dir_walk(dirClus, isRoot, list_cb, 0)) { con_puts("  read error\r\n"); return; }
    gWantDirs = 0;
    if (!dir_walk(dirClus, isRoot, list_cb, 0)) { con_puts("  read error\r\n"); return; }

    {
        char nb[32];
        UINTN l;
        t_utoa((UINT64)gNFile, nb);
        con_puts("\r\n");
        l = t_strlen(nb);
        while (l++ < 16) con_putc(' ');
        con_puts(nb); con_puts(" File(s)");
        t_utoa(gBytes, nb);
        l = t_strlen(nb) + (t_strlen(nb) - 1) / 3;
        while (l++ < 16) con_putc(' ');
        print_commas(gBytes); con_puts(" bytes\r\n");

        t_utoa((UINT64)gNDir, nb);
        l = t_strlen(nb);
        while (l++ < 16) con_putc(' ');
        con_puts(nb); con_puts(" Dir(s)");
        t_utoa(freeBytes, nb);
        l = t_strlen(nb) + (t_strlen(nb) - 1) / 3;
        while (l++ < 16) con_putc(' ');
        print_commas(freeBytes); con_puts(" bytes free\r\n\r\n");
    }
}

void fat_report(void) {
    if (gMounted) list_one(0);
    else con_puts("  no FAT volume mounted\r\n");
}

void fat_list(const char *path) {
    if (!gMounted) { con_puts("  no FAT volume mounted\r\n"); return; }
    list_one(path);
}
/* ------------------------------------------------------------ 写 FAT 表项 */
static int fat_set(UINT32 cl, UINT32 val) {
    UINT32 off, lba;

    if (!gV.ok) return -1;
    if (gV.type == 12)      off = cl + (cl >> 1);
    else if (gV.type == 16) off = cl * 2u;
    else                    off = cl * 4u;

    lba = (UINT32)(gV.fatLba + (off / gV.bps));
    if (!sec_read(lba, gSec)) return -1;

    if (gV.type == 32) {
        /* 高 4 位是保留位，必须原样保留 */
        UINT32 old = rd32(gSec + (off % gV.bps));
        wr32(gSec + (off % gV.bps), (old & 0xF0000000u) | (val & 0x0FFFFFFFu));
        return sec_write(lba, gSec) ? 0 : -1;
    }
    if (gV.type == 16) {
        wr16(gSec + (off % gV.bps), (UINT16)val);
        return sec_write(lba, gSec) ? 0 : -1;
    }
    /* FAT12：两个表项共享 3 字节，写的时候必须保住邻居那半个 */
    {
        UINT32 insec = off % gV.bps;
        UINT8  b[3];
        UINT32 i;
        for (i = 0; i < 3; i++) {
            UINT32 o = insec + i;
            if (o >= gV.bps) {
                /* 跨扇区：先把当前扇区写回去，再读下一扇区 */
                if (!sec_write(lba, gSec)) return -1;
                if (!sec_read(lba + 1, gSec)) return -1;
                insec = 0;
                b[i] = gSec[0];
                continue;
            }
            b[i] = gSec[o];
        }
        {
            UINT32 v = (UINT32)b[0] | ((UINT32)b[1] << 8) | ((UINT32)b[2] << 16);
            if (cl & 1u) v = (v & 0x00000Fu) | ((val & 0x0FFFu) << 4);
            else         v = (v & 0xFFF000u) | (val & 0x0FFFu);
            b[0] = (UINT8)v; b[1] = (UINT8)(v >> 8); b[2] = (UINT8)(v >> 16);
        }
        /* 重新定位再写一遍（跨扇区时上面已经推进过 gSec） */
        lba = (UINT32)(gV.fatLba + (off / gV.bps));
        insec = off % gV.bps;
        for (i = 0; i < 3; i++) {
            UINT32 o = insec + i;
            if (o >= gV.bps) {
                if (!sec_write(lba, gSec)) return -1;
                if (!sec_read(lba + 1, gSec)) return -1;
                insec = 0;
                gSec[0] = b[i];
                continue;
            }
            gSec[o] = b[i];
        }
        return sec_write(lba, gSec) ? 0 : -1;
    }
}

static UINT32 cl_eoc(void) {
    if (gV.type == 12) return 0x0FFFu;
    if (gV.type == 16) return 0xFFFFu;
    return 0x0FFFFFFFu;
}

/* 找一个空簇并标成链尾。返回 0 = 没有空间。 */
static UINT32 alloc_cluster(void) {
    UINT32 cl;
    for (cl = 2; cl < 2u + gV.nClusters; cl++) {
        UINT32 v = 0;
        if (fat_get(cl, &v) != 0) return 0;
        if (v != 0) continue;
        if (fat_set(cl, cl_eoc()) != 0) return 0;
        gFreeValid = 0;
        return cl;
    }
    return 0;
}

static void free_chain(UINT32 firstClus) {
    UINT32 cl = firstClus, guard = 0;
    while (cl_valid(cl) && guard++ < gV.nClusters + 2) {
        UINT32 nx = 0;
        if (fat_get(cl, &nx) != 0) return;
        fat_set(cl, 0);
        gFreeValid = 0;
        if (fat_is_eoc(nx) || nx == 0) return;
        cl = nx;
    }
}

/* 把 firstClus 起的链延长一格（用在写文件时顺序追加） */
static UINT32 chain_append(UINT32 lastClus) {
    UINT32 n = alloc_cluster();
    if (!n) return 0;
    if (lastClus && fat_set(lastClus, n) != 0) return 0;
    return n;
}

/* 目录里找一个空位（优先复用已删除项；没有就接在 END 处） */
typedef struct { UINT64 lba; UINT32 off; int found; } SLOT;
static SLOT gSlot;
static UINT32 gSlotDirClus;
static int    gSlotIsRoot;

static int slot_cb(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx) {
    (void)ctx;
    if (e->Name[0] == DIRENT_FREE) { gSlot.lba = lba; gSlot.off = off; gSlot.found = 1; return 1; }
    if (e->Name[0] == DIRENT_END)  { gSlot.lba = lba; gSlot.off = off; gSlot.found = 1; return 1; }
    return 0;
}

static int dir_find_slot(UINT32 dirClus, int isRoot, UINT64 *lba, UINT32 *off) {
    gSlot.lba = 0; gSlot.off = 0; gSlot.found = 0;
    gSlotDirClus = dirClus; gSlotIsRoot = isRoot;
    dir_walk(dirClus, isRoot, slot_cb, 0);
    if (!gSlot.found) {
        /* 目录满了：FAT32 可以给它挂新簇，FAT12/16 根目录是固定的、不能扩 */
        if (!(isRoot && gV.type != 32)) {
            UINT32 last = dirClus, nx = 0, guard = 0;
            while (guard++ < gV.nClusters + 2) {
                if (fat_get(last, &nx) != 0) return 0;
                if (fat_is_eoc(nx)) break;
                last = nx;
            }
            {
                UINT32 n = chain_append(last);
                if (!n) return 0;
                {
                    UINT32 s;
                    for (s = 0; s < gV.spc; s++) {
                        t_memzero(gSec, 512);
                        if (!sec_write(cl_to_lba(n) + s, gSec)) return 0;
                    }
                }
                gSlot.lba = cl_to_lba(n);
                gSlot.off = 0;
                gSlot.found = 1;
            }
        } else return 0;
    }
    *lba = gSlot.lba;
    *off = gSlot.off;
    return 1;
}

/* 在父目录里加一个条目 */
static int dir_add(UINT32 parentClus, int parentIsRoot, const char *name,
                   UINT8 attr, UINT32 firstClus, UINT32 size) {
    UINT64 lba; UINT32 off;
    UINT8  sec[512];

    if (!dir_find_slot(parentClus, parentIsRoot, &lba, &off)) return 0;
    if (!sec_read(lba, sec)) return 0;

    {
        FAT_DIRENT *e = (FAT_DIRENT *)(sec + off);
        UINT8 nm[11];
        t_memzero(e, sizeof(FAT_DIRENT));
        name_to_dirent(name, nm);
        t_memcpy(e->Name, nm, 11);
        e->Attr       = attr;
        e->FstClusHI  = (UINT16)(firstClus >> 16);
        e->FstClusLO  = (UINT16)(firstClus & 0xFFFF);
        e->FileSize   = size;
        e->WrtDate    = 0x21;      /* 1980-01-01，没有 RTC 就给个确定值 */
        e->WrtTime    = 0;
    }
    if (!sec_write(lba, sec)) return 0;
    return 1;
}

/* ------------------------------------------------------------ 写操作 */
int fat_mkdir(const char *path) {
    char  buf[TND_MAX_PATH];
    char  name[16];
    UINTN i, lastSep = 0, len;
    UINT32 parentClus = 0;
    int    parentIsRoot = 1;
    FAT_DIRENT e;
    UINT32 cl;
    UINT8  sec[512];

    if (!gMounted) return -1;
    fat_resolve(path, buf, sizeof(buf));
    if (buf[0] == 92 && buf[1] == 0) return -1;
    if (path_lookup(buf, &e, 0, 0, 0, 0)) return -1;    /* 已存在 */

    /* 拆出父目录和最后一段名字 */
    len = t_strlen(buf);
    for (i = 0; i < len; i++) if (buf[i] == 92) lastSep = i;
    if (!lastSep) return -1;
    {
        UINTN j = 0;
        for (i = lastSep + 1; buf[i] && j + 1 < sizeof(name); i++) name[j++] = buf[i];
        name[j] = 0;
    }
    if (lastSep == 0) { parentClus = (gV.type == 32) ? gV.rootClus : 0; parentIsRoot = 1; }
    else {
        buf[lastSep] = 0;
        if (!path_lookup(buf, &e, 0, 0, 0, 0) || !(e.Attr & ATTR_DIR)) return -1;
        parentClus = dirent_cluster(&e);
        parentIsRoot = 0;
    }

    cl = alloc_cluster();
    if (!cl) return -1;

    /* 新目录的两项：. 和 .. */
    t_memzero(sec, 512);
    { FAT_DIRENT *d = (FAT_DIRENT *)sec;
      t_memcpy(d->Name, ".          ", 11); d->Attr = ATTR_DIR;
      d->FstClusLO = (UINT16)cl; d->FstClusHI = 0;
      d = (FAT_DIRENT *)(sec + 32);
      t_memcpy(d->Name, "..         ", 11); d->Attr = ATTR_DIR;
      d->FstClusLO = (UINT16)(parentIsRoot ? 0 : parentClus); d->FstClusHI = 0; }
    if (!sec_write(cl_to_lba(cl), sec)) { free_chain(cl); return -1; }
    {
        UINT32 s;
        for (s = 1; s < gV.spc; s++) {
            t_memzero(sec, 512);
            if (!sec_write(cl_to_lba(cl) + s, sec)) { free_chain(cl); return -1; }
        }
    }

    if (!dir_add(parentClus, parentIsRoot, name, ATTR_DIR, cl, 0)) { free_chain(cl); return -1; }
    gFreeValid = 0;
    return 0;
}

int fat_unlink(const char *path) {
    FAT_DIRENT e;
    UINT64 lba;
    UINT32 off;
    UINT8  sec[512];

    if (!gMounted) return -1;
    if (!path_lookup(path, &e, &lba, &off, 0, 0)) return -1;
    if (e.Attr & ATTR_DIR) return -1;

    if (!sec_read(lba, sec)) return -1;
    sec[off] = DIRENT_FREE;
    if (!sec_write(lba, sec)) return -1;

    free_chain(dirent_cluster(&e));
    gFreeValid = 0;
    return 0;
}

int fat_rename(const char *from, const char *to) {
    char  buf[TND_MAX_PATH];
    char  name[16];
    UINTN i, lastSep = 0, len;
    FAT_DIRENT e;
    UINT64 lba;
    UINT32 off;
    UINT8  sec[512];

    if (!gMounted) return -1;
    if (!path_lookup(from, &e, &lba, &off, 0, 0)) return -1;

    fat_resolve(to, buf, sizeof(buf));
    len = t_strlen(buf);
    for (i = 0; i < len; i++) if (buf[i] == 92) lastSep = i;
    {
        UINTN j = 0;
        for (i = lastSep + 1; buf[i] && j + 1 < sizeof(name); i++) name[j++] = buf[i];
        name[j] = 0;
    }
    if (!name[0]) return -1;

    if (!sec_read(lba, sec)) return -1;
    {
        UINT8 nm[11];
        name_to_dirent(name, nm);
        t_memcpy(sec + off, nm, 11);
    }
    return sec_write(lba, sec) ? 0 : -1;
}

int fat_copy(const char *from, const char *to) {
    void  *src = 0;
    UINTN  len = 0;
    UINT32 cl, first = 0, last = 0, done = 0;
    char   buf[TND_MAX_PATH];
    char   name[16];
    UINTN  i, lastSep = 0, l;
    UINT32 parentClus = 0;
    int    parentIsRoot = 1;
    FAT_DIRENT e;
    UINT32 clBytes;

    if (!gMounted) return -1;
    if (fat_read_alloc(from, &src, &len) != 0) return -1;
    if (path_lookup(to, &e, 0, 0, 0, 0)) { kfree(src); return -1; }

    fat_resolve(to, buf, sizeof(buf));
    l = t_strlen(buf);
    for (i = 0; i < l; i++) if (buf[i] == 92) lastSep = i;
    {
        UINTN j = 0;
        for (i = lastSep + 1; buf[i] && j + 1 < sizeof(name); i++) name[j++] = buf[i];
        name[j] = 0;
    }
    if (lastSep == 0) { parentClus = (gV.type == 32) ? gV.rootClus : 0; parentIsRoot = 1; }
    else {
        buf[lastSep] = 0;
        if (!path_lookup(buf, &e, 0, 0, 0, 0) || !(e.Attr & ATTR_DIR)) { kfree(src); return -1; }
        parentClus = dirent_cluster(&e);
        parentIsRoot = 0;
    }

    clBytes = gV.spc * gV.bps;
    while (done < (UINT32)len) {
        UINT32 take = (UINT32)len - done;
        UINT32 s;
        if (take > clBytes) take = clBytes;
        cl = chain_append(last);
        if (!cl) { if (first) free_chain(first); kfree(src); return -1; }
        if (!first) first = cl;
        last = cl;
        for (s = 0; s < gV.spc && (s * gV.bps) < take; s++) {
            UINT32 n = take - s * gV.bps;
            if (n > gV.bps) n = gV.bps;
            t_memzero(gSec, 512);
            t_memcpy(gSec, (UINT8 *)src + done + s * gV.bps, n);
            if (!sec_write(cl_to_lba(cl) + s, gSec)) { free_chain(first); kfree(src); return -1; }
        }
        done += take;
    }

    if (!dir_add(parentClus, parentIsRoot, name, ATTR_ARCH, first, (UINT32)len)) {
        if (first) free_chain(first);
        kfree(src);
        return -1;
    }
    kfree(src);
    gFreeValid = 0;
    return 0;
}

/* ------------------------------------------------------------ 自检 */
/* 建 / 写 / 读回 / 比对 / 删掉。没有 RTC 也没关系，这里不依赖时间。 */
int fat_selftest(void) {
    static const char kText[] = "TNDDOS fat self-test payload. 0123456789";
    static const char kPath[] = "\\TNDFAT.TMP";
    char  rb[128];
    UINTN n = 0;
    int   ok = 1;

    if (!gMounted) return 0;

    fat_unlink(kPath);

    /* 直接用 fat_copy 的写路径造一个文件：先创建一个空的，再写 */
    {
        UINT32 cl = alloc_cluster();
        UINT8  sec[512];
        UINT32 parentClus = (gV.type == 32) ? gV.rootClus : 0;
        if (!cl) return 0;
        t_memzero(sec, 512);
        t_memcpy(sec, kText, sizeof(kText) - 1);
        if (!sec_write(cl_to_lba(cl), sec)) { free_chain(cl); return 0; }
        if (!dir_add(parentClus, 1, "TNDFAT.TMP", ATTR_ARCH, cl, (UINT32)(sizeof(kText) - 1))) {
            free_chain(cl);
            return 0;
        }
    }

    if (fat_read_text(kPath, rb, sizeof(rb), &n) != 0) ok = 0;
    if (ok && (n != sizeof(kText) - 1 || t_strcmp(rb, kText) != 0)) ok = 0;

    fat_unlink(kPath);
    if (fat_exists(kPath)) ok = 0;

    log_kv_u64("fat.selftest", (UINT64)(ok ? 1 : 0));
    return ok;
}
/* ------------------------------------------------------------ 目录枚举 */
/* 给 api.c 的 findfirst/findnext 用。按序号取目录里的第 n 个条目。
 * 每次调用走一遍目录 —— O(n^2)，但一次扫描最多 256 项，代价可以忽略，
 * 换来的是不需要维护任何句柄状态。 */
typedef struct {
    int     want;
    int     idx;
    char   *name;
    UINTN   cap;
    UINT32  attr;
    UINT32  size;
    int     hit;
} ENUM_CTX;

static int enum_cb(const FAT_DIRENT *e, UINT64 lba, UINT32 off, void *ctx) {
    ENUM_CTX *k = (ENUM_CTX *)ctx;
    (void)lba; (void)off;
    if (e->Name[0] == DIRENT_END)  return 1;
    if (e->Name[0] == DIRENT_FREE) return 0;
    if (e->Attr == ATTR_LFN)       return 0;
    if (e->Attr & ATTR_VOLUME)     return 0;
    if (k->idx++ != k->want) return 0;
    name_from_dirent(e, k->name, k->cap);
    k->attr = (e->Attr & ATTR_DIR) ? TND_ATTR_DIR : 0;
    k->size = e->FileSize;
    k->hit  = 1;
    return 1;
}

/* 返回 1 = 第 index 项存在。name 是 8.3 名字，attr 里 TND_ATTR_DIR 表示目录。 */
int fat_enum(const char *dirPath, int index, char *name, UINTN nameCap,
             UINT32 *attrOut, UINT32 *sizeOut) {
    char     buf[TND_MAX_PATH];
    FAT_DIRENT e;
    UINT32   dirClus;
    int      isRoot;
    ENUM_CTX k;

    if (!gMounted || !name || nameCap < 2 || index < 0) return 0;
    name[0] = 0;

    if (!dirPath || !*dirPath) fat_resolve("", buf, sizeof(buf));
    else                       fat_resolve(dirPath, buf, sizeof(buf));

    if (buf[0] == 92 && buf[1] == 0) {
        dirClus = (gV.type == 32) ? gV.rootClus : 0;
        isRoot = 1;
    } else {
        if (!path_lookup(buf, &e, 0, 0, 0, 0) || !(e.Attr & ATTR_DIR)) return 0;
        dirClus = dirent_cluster(&e);
        isRoot = 0;
    }

    k.want = index; k.idx = 0; k.name = name; k.cap = nameCap;
    k.attr = 0; k.size = 0; k.hit = 0;
    if (!dir_walk(dirClus, isRoot, enum_cb, &k)) return 0;
    if (!k.hit) return 0;
    if (attrOut) *attrOut = k.attr;
    if (sizeOut) *sizeOut = k.size;
    return 1;
}
/* ------------------------------------------------------------ 按需挂载
 *
 * **为什么需要它**：块设备是驱动注册的，而驱动是 TNX 程序 —— 它们比模块
 * 初始化晚得多。vfs 第一次问"有没有 FAT 卷"的时候 blk_count() 还是 0，
 * 于是挂载失败；而懒挂载只看 gMounted，失败过一次就再也不试了。
 * 结果：整台机器退回固件那条路，程序读不到、autoexec 找不到。
 *
 * 所以把"找一个能挂的块设备"做成一个幂等入口，谁发现没挂就调它一次。
 * 代价只是几次扇区读。 */
int fat_mount_any(void) {
    int bi, bn;
    if (gMounted) return 1;
    bn = blk_count();
    for (bi = 0; bi < bn && !gMounted; bi++) fat_mount(bi);
    return gMounted;
}