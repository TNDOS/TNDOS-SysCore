/* ============================================================================
 * TNDDOS -- 程序运行环境（TNX API v2 的实现）
 *
 * 这个文件把"DOS 的骨架"给到 TNX 程序：句柄表、标准输入输出、argv、目录遍历。
 *
 * v1 那张表只有 puts/alloc/free/ticks —— 什么都写不了。v2 之后
 * EXECOM / MORE / FIND / TREE / ATTRIB 这些工具才有得写。
 *
 * 设计上照抄 DOS 的句柄模型：
 *   0 = stdin, 1 = stdout, 2 = stderr（v1 都指向控制台）
 *   重定向 = 把某个 fd 换成文件，程序完全不知情
 *   将来加管道也只是把 fd 换成一个环缓冲，程序侧一个字节都不用改
 *
 * v1 一次只有一个程序，所以句柄表是全局的。放进结构体里是为了将来能变成"每进程一份"。
 * ==========================================================================*/
#include "tnd.h"
#include "tnd_api.h"
/* 目录遍历现在走自己的 FAT 驱动（fat_enum），不再包含 FatFs。 */

#define TND_MAX_FDS 16

typedef enum { FD_FREE = 0, FD_CONSOLE, FD_FILE, FD_DIR } FD_KIND;

typedef struct {
    int                kind;
    EFI_FILE_PROTOCOL *f;
    char               pattern[TND_NAME_MAX];   /* FD_DIR：匹配模式 */
    UINT8             *infoBuf;                 /* FD_DIR：目录项缓冲 */
    UINTN              infoSize;

    /* **内存落地的文件。**
     *
     * FAT 那条路上的文件打开时整个读进堆，之后 read/seek 都在内存里做。
     * 为什么不按簇流式读：那要维护"当前簇 + 簇内偏移 + 链上位置"三个状态，
     * 而 DOS 程序绝大多数是"打开读完关掉"。用一个缓冲换掉那个状态机，
     * 代码少一半，出错机会也少一半。
     *
     * 代价是大文件占内存 —— 对 TNX 程序和配置文件这个量级完全可接受。 */
    UINT8             *mem;
    UINTN              memLen;
    UINTN              memPos;
} TND_FD;

static TND_FD gFd[TND_MAX_FDS];

static int  gArgc = 0;
static char gArgv[8][TND_PATH_MAX];

/* ----------------------------------------------------------------- 小工具 */
static int fd_alloc(void) {
    for (int i = 3; i < TND_MAX_FDS; i++) if (gFd[i].kind == FD_FREE) return i;
    return -1;
}

static void fd_reset(int fd) {
    if (fd < 0 || fd >= TND_MAX_FDS) return;
    if (gFd[fd].f && gFd[fd].kind != FD_CONSOLE) { gFd[fd].f->Close(gFd[fd].f); gFd[fd].f = 0; }
    if (gFd[fd].infoBuf) { gEnv.BS->FreePool(gFd[fd].infoBuf); gFd[fd].infoBuf = 0; }
    if (gFd[fd].mem) { kfree(gFd[fd].mem); gFd[fd].mem = 0; }
    gFd[fd].memLen = 0;
    gFd[fd].memPos = 0;
    gFd[fd].kind = FD_FREE;
    gFd[fd].infoSize = 0;
    gFd[fd].pattern[0] = 0;
}

/* 只支持 * 和 ?，大小写不敏感 —— 够 DOS 工具用了 */
static int wild_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (const char *s = name; ; s++) {
                if (wild_match(pat, s)) return 1;
                if (!*s) return 0;
            }
        } else if (*pat == '?' ||
                   (char)t_toupper((unsigned char)*pat) == (char)t_toupper((unsigned char)*name)) {
            if (!*name) return 0;
            pat++; name++;
        } else {
            return 0;
        }
    }
    return *name == 0;
}

/* 阻塞读一个键。
 * **走 in_getkey()，不直连固件** —— 否则 PS/2 驱动注册了也没人用。 */
static int con_read_key(void) {
    for (;;) {
        int k = in_getkey();
        if (k >> 16) continue;                 /* 方向键之类先丢掉 */
        return (int)(k & 0xFFFF);
    }
}

/* ------------------------------------------------------------------ 控制台 */
static TND_ABI void api_puts(const char *s) { con_puts(s ? s : "(null)"); }
static TND_ABI void api_putc(int c) { con_putc((char)c); }

static TND_ABI void api_putu(tnd_u64 v) {
    char b[24]; int i = 0;
    if (!v) { con_putc('0'); return; }
    while (v && i < 23) { b[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) con_putc(b[i]);
}

static TND_ABI void api_putx(tnd_u64 v) {
    static const char *h = "0123456789ABCDEF";
    char b[16]; int i;
    for (i = 0; i < 16; i++) b[i] = h[(v >> ((15 - i) * 4)) & 0xF];
    con_puts("0x");
    for (i = 0; i < 16; i++) con_putc(b[i]);
}

/* -------------------------------------------------------------- 程序环境 */
static TND_ABI int api_argc(void) { return gArgc; }

static TND_ABI const char *api_argv(int i) {
    if (i < 0 || i >= gArgc) return "";
    return gArgv[i];
}

static TND_ABI const char *api_env(const char *name) {
    const char *v = name ? env_get(name) : 0;
    return v ? v : "";
}

/* ------------------------------------------------------------------ 句柄 */
static TND_ABI int api_open(const char *path, int flags) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    int fd;

    if (!path || !*path) return -1;
    fd = fd_alloc();
    if (fd < 0) return -1;

    /* **能走 FAT 就走 FAT。**
     * 固件那个 SimpleFileSystem 在 ExitBootServices 之后就不存在了，
     * 而 TNX 程序全是在那之后跑的 —— 所以这条必须优先。 */
    /* 判只读不能写成 flags & (WRONLY|RDWR)：TND_O_RDWR 是 0x0003，
     * 它包含 TND_O_RDONLY 的 0x0001，于是 flags=RDONLY 时掩码非零，
     * 条件永远不成立，FAT 分支一次都进不去。
     * 低两位才是模式（1=只读 2=写 3=读写），所以拿它整体比。 */
    if (fat_mount_any() &&
        (flags & TND_O_RDWR) == TND_O_RDONLY &&
        !(flags & TND_O_CREATE)) {
        void *mem = 0;
        UINTN len = 0;
        if (fat_read_alloc(path, &mem, &len) == 0) {
            gFd[fd].kind   = FD_FILE;
            gFd[fd].f      = 0;
            gFd[fd].mem    = (UINT8 *)mem;
            gFd[fd].memLen = len;
            gFd[fd].memPos = 0;
            return fd;
        }
    }

    if (flags & TND_O_CREATE) {
        s = vfs_create(path, &f);
        if (!EFI_ERROR(s) && f && (flags & TND_O_TRUNC)) vfs_truncate(f);
    } else {
        s = vfs_open(path, (flags & (TND_O_WRONLY | TND_O_RDWR)) ? 1 : 0, &f);
    }
    if (EFI_ERROR(s) || !f) { fd_reset(fd); return -1; }

    gFd[fd].kind = FD_FILE;
    gFd[fd].f = f;
    return fd;
}

static TND_ABI int api_close(int fd) {
    if (fd < 0 || fd >= TND_MAX_FDS) return -1;
    if (fd < 3) return 0;                       /* 标准句柄不关 */
    if (gFd[fd].kind == FD_FREE) return -1;
    fd_reset(fd);
    return 0;
}

static TND_ABI tnd_i64 api_read(int fd, void *buf, tnd_i64 count) {
    TND_FD *e;
    UINTN want;
    EFI_STATUS s;

    if (fd < 0 || fd >= TND_MAX_FDS || !buf || count <= 0) return -1;
    e = &gFd[fd];

    if (e->kind == FD_CONSOLE) {
        int c = con_read_key();
        if (c < 0) return 0;
        *(char *)buf = (char)c;
        return 1;
    }
    if (e->kind != FD_FILE) return -1;

    /* 内存落地的文件 */
    if (e->mem) {
        UINTN n;
        if (e->memPos >= e->memLen) return 0;
        n = e->memLen - e->memPos;
        if (n > (UINTN)count) n = (UINTN)count;
        t_memcpy(buf, e->mem + e->memPos, n);
        e->memPos += n;
        return (tnd_i64)n;
    }

    if (!e->f) return -1;
    want = (UINTN)count;
    s = e->f->Read(e->f, &want, buf);
    if (EFI_ERROR(s)) return -1;
    return (tnd_i64)want;
}

static TND_ABI tnd_i64 api_write(int fd, const void *buf, tnd_i64 count) {
    TND_FD *e;
    UINTN want;
    EFI_STATUS s;

    if (fd < 0 || fd >= TND_MAX_FDS || !buf || count <= 0) return -1;
    e = &gFd[fd];

    if (e->kind == FD_CONSOLE) { con_write((const char *)buf, (UINTN)count); return count; }
    if (e->kind != FD_FILE || !e->f) return -1;

    want = (UINTN)count;
    s = e->f->Write(e->f, &want, (void *)buf);
    if (EFI_ERROR(s)) return -1;
    return (tnd_i64)want;
}

static TND_ABI tnd_i64 api_seek(int fd, tnd_i64 offset, int whence) {
    TND_FD *e;
    UINT64 pos = 0;
    EFI_STATUS s;

    if (fd < 0 || fd >= TND_MAX_FDS) return -1;
    e = &gFd[fd];
    if (e->kind != FD_FILE) return -1;

    if (e->mem) {
        UINT64 np = 0;
        if (whence == TND_SEEK_SET)      np = (UINT64)offset;
        else if (whence == TND_SEEK_CUR) np = (UINT64)e->memPos + (UINT64)offset;
        else                             np = (UINT64)e->memLen + (UINT64)offset;
        if (np > (UINT64)e->memLen) np = (UINT64)e->memLen;
        e->memPos = (UINTN)np;
        return (tnd_i64)np;
    }

    if (!e->f) return -1;

    if (whence == TND_SEEK_SET) {
        pos = (UINT64)offset;
    } else if (whence == TND_SEEK_CUR) {
        UINT64 cur = 0;
        if (EFI_ERROR(e->f->GetPosition(e->f, &cur))) return -1;
        pos = cur + (UINT64)offset;
    } else {
        pos = 0xFFFFFFFFFFFFFFFFULL;            /* UEFI：这个值 = 定位到末尾 */
        if (offset) {
            static UINT8 info[512];
            UINTN sz = sizeof(info);
            if (EFI_ERROR(e->f->GetInfo(e->f, &gEfiFileInfoGuid, &sz, info))) return -1;
            pos = ((EFI_FILE_INFO *)info)->FileSize + (UINT64)offset;
        }
    }
    s = e->f->SetPosition(e->f, pos);
    if (EFI_ERROR(s)) return -1;
    return (tnd_i64)pos;
}

/* ------------------------------------------------------------ 文件系统 */
static TND_ABI int api_unlink(const char *path) {
    return vfs_unlink(path) ? 0 : -1;
}
/* 注意 vfs_mkdir/rmdir/rename/unlink 返回的是 int（非 0 = 成功），
 * 不是 EFI_STATUS —— 套 EFI_ERROR() 会把结果判断反。 */
static TND_ABI int api_mkdir(const char *path)  { return vfs_mkdir(path)  ? 0 : -1; }
static TND_ABI int api_rmdir(const char *path)  { return vfs_rmdir(path)  ? 0 : -1; }
static TND_ABI int api_rename(const char *a, const char *b) { return vfs_rename(a, b) ? 0 : -1; }

static TND_ABI int api_stat(const char *path, TND_STAT *st) {
    EFI_FILE_PROTOCOL *f = 0;
    static UINT8 info[512];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    UINTN sz = sizeof(info);

    if (!st) return -1;
    t_memzero(st, sizeof(*st));

    /* **先走 FAT。** 固件那个 SFS 在退出之后没了，而这个函数是被
     * TNX 程序调的 —— 那时候早就退出了。 */
    if (fat_mount_any()) {
        UINT32 sz2 = 0, at2 = 0;
        if (fat_stat(path, &sz2, &at2) == 0) {
            st->Size = (tnd_u32)sz2;
            st->Attr = at2;
            return 0;
        }
    }

    if (EFI_ERROR(vfs_open(path, 0, &f)) || !f) return -1;
    if (EFI_ERROR(f->GetInfo(f, &gEfiFileInfoGuid, &sz, info))) { f->Close(f); return -1; }
    f->Close(f);

    st->Size   = (tnd_u32)fi->FileSize;
    st->Attr   = (fi->Attribute & EFI_FILE_DIRECTORY) ? TND_ATTR_DIR : 0;
    if (fi->Attribute & EFI_FILE_READ_ONLY) st->Attr |= TND_ATTR_RDONLY;
    st->Year   = fi->ModificationTime.Year;
    st->Month  = fi->ModificationTime.Month;
    st->Day    = fi->ModificationTime.Day;
    st->Hour   = fi->ModificationTime.Hour;
    st->Minute = fi->ModificationTime.Minute;
    return 0;
}

/* --------------------------------------------------------- 目录遍历 */
/* **FatFs 挂上之后，一次性把整个目录读进内存，再从内存里发。**
 *
 * 原来这条路上每一步都问固件的 EFI_FILE_PROTOCOL，而退出 Boot Services
 * 之后固件已经没了 —— 症状是 tree 能跑起来却打印
 *   Directory tree: .
 *   0 dir(s), 0 file(s)
 * 也就是"命令成功、结果为空"，最难看出来的一种坏法。
 *
 * 一次读完的好处：不需要维护一个 DIR 句柄的生命周期，
 * 也不会出现"列到一半状态跑了"。上限 256 条，够用。 */
#define FIND_LIST_MAX 256
static TND_FIND gFindList[FIND_LIST_MAX];
static int      gFindCount = 0;
static int      gFindIdx   = 0;
static int      gFindFd    = -1;

static TND_ABI int api_findnext(int fh, TND_FIND *out);

static TND_ABI int api_findfirst(const char *pattern, TND_FIND *out) {
    char dir[TND_PATH_MAX], pat[TND_NAME_MAX];
    const char *p, *last = 0;
    EFI_FILE_PROTOCOL *f = 0;
    int fd;

    if (!pattern || !out) return -1;

    for (p = pattern; *p; p++) if (*p == '\\' || *p == '/') last = p;
    if (last) {
        UINTN n = (UINTN)(last - pattern);
        if (n >= sizeof(dir)) n = sizeof(dir) - 1;
        t_memcpy(dir, pattern, n); dir[n] = 0;
        if (!dir[0]) { dir[0] = '\\'; dir[1] = 0; }
        t_strncpy(pat, last + 1, sizeof(pat));
    } else {
        t_strncpy(dir, ".", sizeof(dir));
        t_strncpy(pat, pattern, sizeof(pat));
    }
    if (!pat[0]) t_strncpy(pat, "*", sizeof(pat));

    /* **FatFs 优先。** 见上面那段注释。 */
    if (fat_ready()) {
        /* **走自己的 FAT 驱动**，不再经过 FatFs。
         * fat_enum 按序号取第 n 项，所以这里带一个递增的 idx ——
         * 名字被通配符过滤掉时序号照样前进，不会漏项。 */
        char    nm[TND_NAME_MAX];
        UINT32  at = 0, sz = 0;
        int     n = 0, idx = 0;

        while (n < FIND_LIST_MAX) {
            UINTN i;
            if (!fat_enum(dir, idx, nm, sizeof(nm), &at, &sz)) break;
            idx++;
            if (!t_strcmp(nm, ".") || !t_strcmp(nm, "..")) continue;
            for (i = 0; nm[i] && i < TND_NAME_MAX - 1; i++) {
                char c = nm[i];
                gFindList[n].Name[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            gFindList[n].Name[i] = 0;
            if (!wild_match(pat, gFindList[n].Name)) continue;
            gFindList[n].Size = (tnd_u32)sz;
            gFindList[n].Attr = at;
            gFindList[n].Reserved = 0;
            n++;
        }
        gFindCount = n;
        gFindIdx = 0;
        gFindFd = fd_alloc();
        if (gFindFd < 0) return -1;
        gFd[gFindFd].kind = FD_DIR;
        gFd[gFindFd].f = 0;              /* 没有 EFI 句柄，靠 gFindList */
        if (api_findnext(gFindFd, out) != 0) { fd_reset(gFindFd); gFindFd = -1; return -1; }
        return gFindFd;
    }

    if (EFI_ERROR(vfs_open(dir, 0, &f)) || !f) return -1;

    fd = fd_alloc();
    if (fd < 0) { f->Close(f); return -1; }

    gFd[fd].kind = FD_DIR;
    gFd[fd].f = f;
    t_strncpy(gFd[fd].pattern, pat, TND_NAME_MAX);

    /* 目录句柄的位置在别处用过之后可能停在末尾，先复位 */
    f->SetPosition(f, 0);

    if (api_findnext(fd, out) != 0) { fd_reset(fd); return -1; }
    return fd;
}

static TND_ABI int api_findnext(int fh, TND_FIND *out) {
    TND_FD *e;
    if (fh < 0 || fh >= TND_MAX_FDS || !out) return -1;
    e = &gFd[fh];
    if (e->kind != FD_DIR) return -1;

    /* FatFs 那条路：条目在 gFindList 里，没有 EFI 句柄 */
    if (!e->f) {
        if (fh != gFindFd || gFindIdx >= gFindCount) return -1;
        *out = gFindList[gFindIdx++];
        return 0;
    }

    for (;;) {
        EFI_FILE_INFO *fi;
        UINTN sz;
        char name[TND_NAME_MAX];
        EFI_STATUS s;

        if (!e->infoBuf) {
            e->infoSize = sizeof(EFI_FILE_INFO) + 512;
            if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, e->infoSize, (void **)&e->infoBuf)) || !e->infoBuf) return -1;
        }
        sz = e->infoSize;
        s = e->f->Read(e->f, &sz, e->infoBuf);
        if (EFI_ERROR(s) || sz == 0) return -1;

        fi = (EFI_FILE_INFO *)e->infoBuf;
        if (!fi->FileName[0]) continue;

        /* UTF-16 -> ASCII */
        {
            UINTN i;
            for (i = 0; fi->FileName[i] && i < TND_NAME_MAX - 1; i++) {
                CHAR16 c = fi->FileName[i];
                name[i] = (c < 128) ? (char)t_toupper((unsigned char)c) : '?';
            }
            name[i] = 0;
        }
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        if (!wild_match(e->pattern, name)) continue;

        t_strncpy(out->Name, name, TND_NAME_MAX);
        out->Size = (tnd_u32)fi->FileSize;
        out->Attr = (fi->Attribute & EFI_FILE_DIRECTORY) ? TND_ATTR_DIR : 0;
        out->Reserved = 0;
        return 0;
    }
}

static TND_ABI int api_findclose(int fh) {
    if (fh < 0 || fh >= TND_MAX_FDS) return -1;
    if (gFd[fh].kind != FD_DIR) return -1;
    fd_reset(fh);
    return 0;
}

/* ------------------------------------------------------------ 内存 / 时间 */
static TND_ABI void *api_alloc(tnd_size n) { return kmalloc((UINTN)n); }
static TND_ABI void  api_free(void *p)     { kfree(p); }
/* 自启动以来的**毫秒**。定时器关掉（SET TNDDOS_TIMER=OFF）时返回 0 ——
 * 老实返回 0，不给假值：假的时间比没有时间更坏。 */
static TND_ABI tnd_u64 api_ticks(void)     { return t_time_ms(); }

/* ------------------------------------------------------------ 屏幕与键盘 */
/* **这一组必须走控制台服务层，不能直接碰 ConOut。**
 *
 * 它们原来直连 ConOut，切到 fb 后端之后就全错了：
 *   cls    清的是 UEFI 控制台，fb 根本不理会
 *   gotoxy 移动的是 UEFI 光标，不是我们的
 *   cols   返回 100x31（固件的网格），而实际是 160x50
 *   rows   同上
 * 症状就是 EDIT：屏幕不清、从当前光标位置往下画、而且按 100 列排版。
 *
 * 这跟之前 api_getattr 读 ConOut->Mode->Attribute 是同一个坑 ——
 * **凡是"当前屏幕状态"的问题，答案只能来自当前后端。** */
static TND_ABI void api_cls(void) { con_clear(); }

static TND_ABI void api_gotoxy(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    con_gotoxy((UINTN)x, (UINTN)y);
}

/* 阻塞读键，**保留扫描码** —— 方向键的 UnicodeChar 是 0，
 * 只看它等于什么都没读到。这是写全屏程序的前提。 */
static TND_ABI int api_getkey(void) { return in_getkey(); }

static TND_ABI int api_cols(void) { return (int)con_cols(); }
static TND_ABI int api_rows(void) { return (int)con_rows(); }

static TND_ABI void api_setattr(int attr) { con_set_attr((UINTN)(attr & 0xFF)); }

static TND_ABI void api_cursor(int visible) { con_cursor(visible); }

static TND_ABI int api_getattr(void) {
    /* **不要读 ConOut->Mode->Attribute。** 切到 fb 之后那是过期值 ——
     * 和 shell 的光标位置是同一个坑。属性由控制台路由层记账，问它才对。 */
    return (int)con_get_attr();
}

/* ---------------------------------------------------------- 批量原语（v2.4） */
static TND_ABI void api_writecells(int x, int y, int w, int h, const TND_CELL *cells, int stride) {
    if (w <= 0 || h <= 0 || !cells || stride < w) return;
    con_write_cells((UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h, cells, (UINTN)stride);
}

static TND_ABI void api_fill(int x, int y, int w, int h, int ch, int attr) {
    if (w <= 0 || h <= 0) return;
    con_fill((UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h, (UINTN)(ch & 0xFF), (UINTN)(attr & 0xFF));
}

static TND_ABI void api_scroll(int x, int y, int w, int h, int dy, int ch, int attr) {
    if (w <= 0 || h <= 0) return;
    con_scroll((UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h, dy, (UINTN)(ch & 0xFF), (UINTN)(attr & 0xFF));
}

static TND_ABI void api_screen(TND_SCREEN *out) {
    UINTN cx = 0, cy = 0;
    if (!out) return;
    con_getxy(&cx, &cy);
    out->StructSize    = (tnd_u32)sizeof(TND_SCREEN);
    out->X             = (int)cx;
    out->Y             = (int)cy;
    out->Cols          = (int)con_cols();
    out->Rows          = (int)con_rows();
    out->Attr          = (int)con_get_attr();
    out->CursorVisible = 1;
    out->Reserved      = 0;
}

/* ------------------------------------------------------------- 输入后端（v2.5）
 * 驱动注册一个输入后端。逻辑在 log.c 的路由层，这里只做 ABI 适配。 */
static TND_ABI int api_in_register(const TND_INPUT *in) {
    if (!in || !in->GetKey) return -1;
    in_register(in);
    return 0;
}

/* --------------------------------------------------------------- 驱动（v2.5）
 * 驱动调这一个函数登记自己。真正的逻辑在 drv.c ——
 * 这里只做 ABI 适配，不重复实现。 */
static TND_ABI int api_drv_register(const TND_DRIVER_DESC *desc) {
    return drv_register(desc);
}

/* ------------------------------------------------------- PCI 设备归属（v2.6）
 *
 * **把一个 PCI 设备从固件的驱动手里摘下来。**
 *
 * 为什么需要它：Boot Services 还活着的时候，固件自己的驱动仍然拥有设备 ——
 * OVMF 的 Ps2KeyboardDxe 还在轮询 8042，XhciDxe 还在用 USB 端口。
 * 我们插进去之后两边抢同一个硬件，症状千奇百怪：
 *   键盘「一串里只有最后一个到」、USB 完成事件被顶掉、控制器转不出去……
 *
 * UEFI 的标准做法是 DisconnectController —— 把挂在设备 handle 上的驱动卸掉。
 * 内核有 gBS，驱动（TNX）拿不到固件服务，所以只能由内核代做。
 *
 * **只对调用者自己那个设备做。** GOP 挂在 00:01.0 上，把它的驱动摘了屏幕就黑。
 * 返回被摘掉的驱动个数（0 表示没找到那个设备，或它本来就没驱动）。 */
static TND_ABI int api_pci_own(int bus, int dev, int fun) {
    int n = pci_disown(bus, dev, fun);      /* 真正的实现在 pci.c */
    log_puts("[log] pci_own: bdf=");
    log_u64((UINT64)((bus << 16) | (dev << 8) | fun));
    log_puts(" -> "); log_u64((UINT64)(n < 0 ? 0 : n)); log_puts(" driver(s)\r\n");
    return n;
}

/* ------------------------------------------------------- 块设备（v2.7）
 * 驱动把「读扇区」交进来。实现全在 blk.c。 */
static TND_ABI int api_blk_register(const TND_BLOCK *b) {
    return blk_register(b);
}

/* ------------------------------------------------------------------ 表 */
static const TND_API_TABLE gApi = {
    sizeof(TND_API_TABLE), TND_API_VERSION,
    api_puts, api_putc, api_putu, api_putx,
    api_argc, api_argv, api_env,
    api_open, api_close, api_read, api_write, api_seek,
    api_unlink, api_mkdir, api_rmdir, api_rename, api_stat,
    api_findfirst, api_findnext, api_findclose,
    api_alloc, api_free, api_ticks,
    api_cls, api_gotoxy, api_getkey, api_cols, api_rows,
    api_setattr, api_getattr,
    api_cursor,
    api_writecells, api_fill, api_scroll, api_screen,
    api_drv_register,
    api_in_register,
    api_pci_own,
    api_blk_register
};

const TND_API_TABLE *api_get_table(void) { return &gApi; }

/* ------------------------------------------------------- 生命周期 / 重定向 */
void api_setup(int argc, char **argv) {
    int i;
    for (i = 0; i < TND_MAX_FDS; i++) {
        gFd[i].kind = FD_FREE; gFd[i].f = 0; gFd[i].infoBuf = 0;
        gFd[i].infoSize = 0;   gFd[i].pattern[0] = 0;
    }
    gArgc = (argc > 8) ? 8 : (argc < 0 ? 0 : argc);
    for (i = 0; i < gArgc; i++) t_strncpy(gArgv[i], argv[i] ? argv[i] : "", TND_PATH_MAX);

    gFd[TND_STDIN].kind  = FD_CONSOLE;
    gFd[TND_STDOUT].kind = FD_CONSOLE;
    gFd[TND_STDERR].kind = FD_CONSOLE;
}

void api_teardown(void) {
    for (int i = 3; i < TND_MAX_FDS; i++) fd_reset(i);
    for (int i = 0; i < 3; i++) fd_reset(i);
    gFd[TND_STDIN].kind  = FD_CONSOLE;
    gFd[TND_STDOUT].kind = FD_CONSOLE;
    gFd[TND_STDERR].kind = FD_CONSOLE;
}

/* 把 fd 换成文件。程序完全不知情 —— 这就是句柄模型的好处。 */
int api_redirect_stdout(const char *path, int append) {
    EFI_FILE_PROTOCOL *f = 0;
    if (!path) return 0;
    if (append) {
        if (EFI_ERROR(vfs_open(path, 1, &f)) || !f) return 0;
        f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);
    } else {
        if (EFI_ERROR(vfs_create(path, &f)) || !f) return 0;
        vfs_truncate(f);
    }
    fd_reset(TND_STDOUT);
    gFd[TND_STDOUT].kind = FD_FILE;
    gFd[TND_STDOUT].f = f;
    return 1;
}

int api_redirect_stdin(const char *path) {
    EFI_FILE_PROTOCOL *f = 0;
    if (!path) return 0;
    if (EFI_ERROR(vfs_open(path, 0, &f)) || !f) return 0;
    fd_reset(TND_STDIN);
    gFd[TND_STDIN].kind = FD_FILE;
    gFd[TND_STDIN].f = f;
    return 1;
}

void api_restore_stdio(void) {
    fd_reset(TND_STDIN); fd_reset(TND_STDOUT); fd_reset(TND_STDERR);
    gFd[TND_STDIN].kind  = FD_CONSOLE;
    gFd[TND_STDOUT].kind = FD_CONSOLE;
    gFd[TND_STDERR].kind = FD_CONSOLE;
}