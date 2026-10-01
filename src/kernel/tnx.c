/* ============================================================================
 * TNDDOS -- TNX 加载器
 *
 * 做四件事：读文件 -> 严格校验 -> 搬进固定映像窗口 -> 调入口。
 *
 * 关于"为什么现在还需要加载器"：
 *   PE 今天是免费的，因为 LoadImage 是固件替我们写的加载器。
 *   TNX 的不可替代价值在于：**当我们不能再调 LoadImage 的时候，我们还能加载程序。**
 *   也就是说这不是格式项目，是独立项目。
 *
 * v1 的已知边界（写清楚，不含糊）：
 *   - 没有分页，物理地址 == 虚拟地址，所以**一次只能装一个程序**，没有隔离。
 *     这不是缺陷 —— DOS 就是这样：固定段、一次一个、没有保护。
 *   - 程序跑在内核栈上、ring 0。它崩了系统就崩了。
 *     真正的答案需要异常处理，而异常处理需要自己的 IDT，而 IDT 需要 ExitBootServices。
 *   - 窗口每次加载都整体清零重写，不保留任何上一个程序的状态。
 * ==========================================================================*/
#include "tnd.h"
#include "tnd_api.h"

const TND_MODULE_DEF gModTnx = { "tnx", "TNX executable loader (fixed base, no relocs)", tnx_init };

static UINT64 gLoads = 0, gFails = 0;
static UINT64 gLastSize = 0;
static int    gLastCode = 0;

/* API 表本身不住在这里 —— 见 src/kernel/api.c（程序运行环境：句柄表 + argv）。
 * 本文件只管格式和加载：校验 -> 清零窗口 -> 逐段搬运 -> 调入口。 */

/* 入口也是 SysV：程序在 ELF 那边是默认 ABI，内核这边必须显式对齐 */
typedef int (TND_ABI *TNX_ENTRY)(const TND_API_TABLE *api);

/* 本次要跑的程序参数。v1 一次只有一个程序，所以放文件级静态即可；
 * 将来有多进程时它会变成"每进程一份"。 */
static int   gRunArgc = 0;
static char *gRunArgv[8];

/* ---------------------------------------------------------------- 初始化 */
int tnx_init(void) {
    gLoads = gFails = gLastSize = 0;
    log_kv_u64("tnx.imageBase", TNX_IMAGE_BASE);
    log_kv_u64("tnx.windowSize", TNX_WINDOW_SIZE);
    log_kv_u64("tnx.headerSize", TNX_HEADER_SIZE);
    log_kv_u64("tnx.sectionSize", (UINT64)sizeof(TNX_SECTION));
    return 1;
}

static const char *tag_name(UINT32 t) {
    switch (t) {
        case TNX_TAG_CODE: return "CODE";
        case TNX_TAG_DATA: return "DATA";
        case TNX_TAG_RODT: return "RODT";
        case TNX_TAG_RSRC: return "RSRC";
        case TNX_TAG_SIGN: return "SIGN";
        default:           return "????";
    }
}

static void sec_flags_str(UINT32 f, char *out) {
    out[0] = (f & TNX_SEC_R) ? 'R' : '-';
    out[1] = (f & TNX_SEC_W) ? 'W' : '-';
    out[2] = (f & TNX_SEC_X) ? 'X' : '-';
    out[3] = (f & TNX_SEC_ZERO) ? 'Z' : '-';
    out[4] = 0;
}

/* 把整个文件读进 kmalloc 缓冲。调用方负责 kfree。 */
static EFI_STATUS read_whole(const char *path, void **out, UINTN *outSize) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    UINTN size = 0, got = 0;
    void *buf = 0;

    *out = 0; *outSize = 0;
    s = vfs_open(path, 0, &f);
    if (EFI_ERROR(s) || !f) return s;

    {
        UINT8 info[512];
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
        UINTN isz = sizeof(info);
        s = f->GetInfo(f, &gEfiFileInfoGuid, &isz, info);
        if (EFI_ERROR(s)) { f->Close(f); return s; }
        size = (UINTN)fi->FileSize;
    }
    if (size == 0 || size > (UINTN)TNX_WINDOW_SIZE) {
        f->Close(f);
        log_kv_u64("tnx.read.badSize", size);
        return EFI_LOAD_ERROR;
    }

    buf = kmalloc(size);
    if (!buf) { f->Close(f); log_kv("tnx", "kmalloc for image failed"); return EFI_OUT_OF_RESOURCES; }

    while (got < size) {
        UINTN want = size - got;
        s = f->Read(f, &want, (UINT8 *)buf + got);
        if (EFI_ERROR(s) || want == 0) break;
        got += want;
    }
    f->Close(f);

    if (got != size) {
        log_kv_u64("tnx.read.want", size);
        log_kv_u64("tnx.read.got", got);
        kfree(buf);
        return EFI_LOAD_ERROR;
    }
    *out = buf; *outSize = size;
    return EFI_SUCCESS;
}

/* ---------------------------------------------------------------- 校验 */
/* 校验是全篇最该较真的地方：这个加载器将来要吃的是"别人的程序"。
 * 每一条失败都有具体理由，绝不静默继续 —— 静默是 bug 的温床。 */
static EFI_STATUS validate(const TNX_HEADER *h, UINTN fileSize, const char **why) {
    if (h->Magic != TNX_MAGIC)          { *why = "bad magic (not a TNX)"; return EFI_LOAD_ERROR; }
    if (h->Version != TNX_VERSION)      { *why = "unsupported version"; return EFI_LOAD_ERROR; }
    if (h->HeaderSize < TNX_HEADER_SIZE){ *why = "HeaderSize below 48"; return EFI_LOAD_ERROR; }
    if (h->HeaderSize > fileSize)       { *why = "HeaderSize beyond file"; return EFI_LOAD_ERROR; }
    if (h->Flags & ~TNX_KNOWN_FLAGS)    { *why = "unknown flag bits set"; return EFI_UNSUPPORTED; }
    if (h->SectionCount == 0)           { *why = "no sections"; return EFI_LOAD_ERROR; }
    if (h->SectionCount > 16)           { *why = "too many sections (max 16)"; return EFI_LOAD_ERROR; }
    if (h->ImageBase != TNX_IMAGE_BASE) { *why = "ImageBase does not match loader window"; return EFI_UNSUPPORTED; }
    if (h->ImageSize == 0)              { *why = "ImageSize is 0"; return EFI_LOAD_ERROR; }
    if (h->ImageSize > TNX_WINDOW_SIZE) { *why = "image larger than window"; return EFI_OUT_OF_RESOURCES; }
    if (h->EntryRVA >= h->ImageSize)    { *why = "entry point outside image"; return EFI_LOAD_ERROR; }
    if ((UINT64)h->HeaderSize + (UINT64)h->SectionCount * sizeof(TNX_SECTION) > (UINT64)fileSize) {
        *why = "section table runs past end of file"; return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

/* ---------------------------------------------------------------- 加载 */
/* 按 DOS 的规矩找一个可执行文件：
 *   1) 当前目录优先
 *   2) 然后依次查 PATH 的每一项（分号分隔）
 *   3) 名字里没有 '.' 就补上 .TNX —— TNDDOS 的程序就是 TNX
 * 找到就把解析后的路径写进 out，返回 1。 */
int tnx_find(const char *name, char *out, UINTN cap) {
    char dirs[8][TND_MAX_PATH];
    int  ndirs = 0;
    int  hasDot = 0;
    char file[TND_MAX_PATH];

    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) if (*p == '.') { hasDot = 1; break; }

    t_strncpy(file, name, sizeof(file));
    if (!hasDot) t_strncpy(file + t_strlen(file), ".TNX", sizeof(file) - t_strlen(file));

    /* 当前目录用空串：这样返回的路径就是裸文件名，不带 ".\" 前缀 */
    dirs[ndirs][0] = 0; ndirs++;
    {
        const char *path = env_get("PATH");
        while (path && *path && ndirs < 8) {
            char one[TND_MAX_PATH]; UINTN k = 0;
            while (*path && *path != ';' && k + 1 < sizeof(one)) one[k++] = *path++;
            one[k] = 0;
            if (*path == ';') path++;
            if (one[0]) t_strncpy(dirs[ndirs++], one, TND_MAX_PATH);
        }
    }

    for (int d = 0; d < ndirs; d++) {
        char cand[TND_MAX_PATH];
        UINTN l = t_strlen(dirs[d]);
        t_strncpy(cand, dirs[d], sizeof(cand));
        if (l && cand[l - 1] != '\\' && l + 1 < sizeof(cand)) { cand[l] = '\\'; cand[l + 1] = 0; }
        t_strncpy(cand + t_strlen(cand), file, sizeof(cand) - t_strlen(cand));
        if (vfs_exists(cand)) { t_strncpy(out, cand, cap); return 1; }
    }
    return 0;
}

/* 带命令行参数跑一个 TNX 程序。
 * Shell 解析完命令行之后走这条路径；tnx_load 自己不带参数是有意的 ——
 * 查看/调试的调用方不关心 argv。 */
EFI_STATUS tnx_run(const char *path, int argc, char **argv, int *outCode) {
    int i;
    gRunArgc = (argc > 8) ? 8 : (argc < 0 ? 0 : argc);
    for (i = 0; i < gRunArgc; i++) gRunArgv[i] = (argv && argv[i]) ? argv[i] : "";
    return tnx_load(path, outCode, 0);
}

EFI_STATUS tnx_load(const char *path, int *outCode, int verbose) {
    void *file = 0;
    UINTN fsize = 0;
    TNX_HEADER *h;
    TNX_SECTION *sec;
    const char *why = 0;
    EFI_STATUS s;
    UINT64 entryRva, imageSize;
    int rc = -1;

    if (outCode) *outCode = -1;

    if (verbose) { con_puts("\r\n  [TNX] "); con_puts(path); con_puts("\r\n"); }

    s = read_whole(path, &file, &fsize);
    if (EFI_ERROR(s) || !file) {
        con_puts("  cannot read "); con_puts(path);
        con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
        gFails++;
        return s;
    }

    h = (TNX_HEADER *)file;
    s = validate(h, fsize, &why);
    if (EFI_ERROR(s)) {
        con_puts("        REJECTED: "); con_puts(why); con_puts("\r\n");
        log_puts("[log] tnx reject: "); log_puts(why); log_puts("\r\n");
        kfree(file);
        gFails++;
        return s;
    }

    if (verbose) {
        con_puts("        image "); con_u64(h->ImageSize);
        con_puts(" bytes @ "); con_hex(h->ImageBase);
        con_puts("\r\n        entry RVA "); con_u64(h->EntryRVA);
        con_puts("   sections "); con_u64((UINT64)h->SectionCount);
        con_puts("\r\n");
    }

    /* 段表 */
    sec = (TNX_SECTION *)((UINT8 *)file + h->HeaderSize);
    for (UINT32 i = 0; i < h->SectionCount; i++) {
        if (verbose) {
            char fs[5]; sec_flags_str(sec[i].Flags, fs);
            con_puts("        #"); con_u64(i);
            con_puts("  "); con_puts(tag_name(sec[i].Tag));
            con_puts("  "); con_puts(fs);
            con_puts("   file "); con_u64(sec[i].FileSize);
            con_puts("  mem "); con_u64(sec[i].MemSize);
            con_puts("  rva "); con_hex(sec[i].RVA);
            con_puts("\r\n");
        }

        /* 逐段校验 —— 任何一条不过就整体拒绝，不做部分加载 */
        if (sec[i].Tag != TNX_TAG_CODE && sec[i].Tag != TNX_TAG_DATA &&
            sec[i].Tag != TNX_TAG_RODT && sec[i].Tag != TNX_TAG_RSRC &&
            sec[i].Tag != TNX_TAG_SIGN) {
            con_puts("        REJECTED: unknown section tag\r\n"); goto reject;
        }
        if (sec[i].FileSize > sec[i].MemSize) {
            con_puts("        REJECTED: FileSize > MemSize\r\n"); goto reject;
        }
        if ((sec[i].Flags & TNX_SEC_ZERO) && sec[i].FileSize != 0) {
            con_puts("        REJECTED: ZERO section carries file data\r\n"); goto reject;
        }
        if (sec[i].FileOffset + sec[i].FileSize > (UINT64)fsize) {
            con_puts("        REJECTED: section data past end of file\r\n"); goto reject;
        }
        if (sec[i].RVA + sec[i].MemSize > h->ImageSize) {
            con_puts("        REJECTED: section runs past image end\r\n"); goto reject;
        }
        if (sec[i].RVA & (TNX_SECTION_ALIGN - 1)) {
            con_puts("        REJECTED: section RVA not 16-byte aligned\r\n"); goto reject;
        }
    }

    /* --- 清零整个映像窗口，再逐段搬 --- */
    entryRva  = h->EntryRVA;
    imageSize = h->ImageSize;
    t_memzero((void *)(UINTN)TNX_IMAGE_BASE, (UINTN)imageSize);

    for (UINT32 i = 0; i < h->SectionCount; i++) {
        UINT8 *dst = (UINT8 *)(UINTN)(TNX_IMAGE_BASE + sec[i].RVA);
        if (sec[i].FileSize) t_memcpy(dst, (UINT8 *)file + sec[i].FileOffset, (UINTN)sec[i].FileSize);
        if (sec[i].MemSize > sec[i].FileSize)
            t_memzero(dst + sec[i].FileSize, (UINTN)(sec[i].MemSize - sec[i].FileSize));
    }

    kfree(file);
    file = 0;

    log_kv_u64("tnx.load.imageSize", imageSize);
    log_kv_u64("tnx.load.entryRva", entryRva);

    /* --- 跑 ---
     * 直接敲程序名执行时（verbose = 0）不打印加载器自己的东西 ——
     * DOS 跑一个程序也不会先跟你汇报段表。只有返回值非 0 才吭一声。 */
    if (verbose) con_puts("        running ...\r\n");
    {
        TNX_ENTRY entry = (TNX_ENTRY)(UINTN)(TNX_IMAGE_BASE + entryRva);
        api_setup(gRunArgc, gRunArgv);      /* 装好 fd 0/1/2 与 argv */
        rc = entry(api_get_table());
        api_teardown();                     /* 关掉程序忘了关的句柄 */
    }
    if (verbose || rc != 0) {
        con_puts("        returned "); con_u64((UINT64)(INT64)rc);
        con_puts("\r\n");
    }

    gLoads++; gLastSize = imageSize; gLastCode = rc;
    if (outCode) *outCode = rc;
    return EFI_SUCCESS;

reject:
    kfree(file);
    gFails++;
    return EFI_LOAD_ERROR;
}

/* ---------------------------------------------------------------- 只看不跑 */
int tnx_info(const char *path) {
    void *file = 0;
    UINTN fsize = 0;
    TNX_HEADER *h;
    TNX_SECTION *sec;
    EFI_STATUS s = read_whole(path, &file, &fsize);

    if (EFI_ERROR(s) || !file) {
        con_puts("  cannot read "); con_puts(path); con_puts("\r\n");
        return 0;
    }
    h = (TNX_HEADER *)file;
    con_puts("\r\n  TNX: "); con_puts(path);
    con_puts("   ("); con_u64((UINT64)fsize); con_puts(" bytes on disk)\r\n");

    if (h->Magic != TNX_MAGIC) {
        con_puts("        REJECTED: bad magic, not a TNX file\r\n");
        kfree(file);
        return 0;
    }
    con_puts("        version  "); con_u64(h->Version >> 16); con_puts(".");
    con_u64((h->Version >> 8) & 0xFF); con_puts(".");
    con_u64(h->Version & 0xFF); con_puts("\r\n");
    con_puts("        header   "); con_u64(h->HeaderSize); con_puts(" bytes\r\n");
    con_puts("        flags    "); con_hex(h->Flags); con_puts("\r\n");
    con_puts("        image    "); con_u64(h->ImageSize); con_puts(" bytes, entry RVA ");
    con_u64(h->EntryRVA); con_puts("\r\n");
    con_puts("        sections "); con_u64((UINT64)h->SectionCount); con_puts("\r\n");

    sec = (TNX_SECTION *)((UINT8 *)file + h->HeaderSize);
    for (UINT32 i = 0; i < h->SectionCount && i < 32; i++) {
        char fs[5]; sec_flags_str(sec[i].Flags, fs);
        con_puts("          "); con_puts(tag_name(sec[i].Tag));
        con_puts("  "); con_puts(fs);
        con_puts("   file "); con_u64(sec[i].FileSize);
        con_puts(" -> mem "); con_u64(sec[i].MemSize);
        con_puts("   rva "); con_u64(sec[i].RVA);
        con_puts("\r\n");
    }
    kfree(file);
    return 1;
}

void tnx_report(void) {
    con_puts("\r\n  TNX: ");
    con_u64(gLoads); con_puts(" run, "); con_u64(gFails); con_puts(" rejected");
    if (gLoads) {
        con_puts("   last: "); con_u64(gLastSize); con_puts(" bytes, returned ");
        con_u64((UINT64)(INT64)gLastCode);
    }
    con_puts("\r\n");
    con_puts("       image window "); con_u64(TNX_IMAGE_BASE);
    con_puts(" .. "); con_u64(TNX_IMAGE_BASE + TNX_WINDOW_SIZE); con_puts("\r\n");
    log_kv_u64("tnx.loads", gLoads);
    log_kv_u64("tnx.fails", gFails);
}
