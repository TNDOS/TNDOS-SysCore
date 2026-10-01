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

#define TND_MAX_FDS 16

typedef enum { FD_FREE = 0, FD_CONSOLE, FD_FILE, FD_DIR } FD_KIND;

typedef struct {
    int                kind;
    EFI_FILE_PROTOCOL *f;
    char               pattern[TND_NAME_MAX];   /* FD_DIR：匹配模式 */
    UINT8             *infoBuf;                 /* FD_DIR：目录项缓冲 */
    UINTN              infoSize;
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

/* 阻塞读一个键 */
static int con_read_key(void) {
    EFI_INPUT_KEY k;
    if (!gEnv.ST || !gEnv.ST->ConIn) return -1;
    for (;;) {
        if (!EFI_ERROR(gEnv.ST->ConIn->ReadKeyStroke(gEnv.ST->ConIn, &k))) {
            if (k.ScanCode) continue;          /* 方向键之类先丢掉 */
            return (int)k.UnicodeChar;
        }
        gEnv.BS->Stall(10000);                 /* 10 ms */
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
    if (e->kind != FD_FILE || !e->f) return -1;

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
    if (e->kind != FD_FILE || !e->f) return -1;

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
    if (e->kind != FD_DIR || !e->f) return -1;

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
static TND_ABI tnd_u64 api_ticks(void)     { return 0; }   /* 定时器子系统还没有 */

/* ------------------------------------------------------------ 屏幕与键盘 */
static TND_ABI void api_cls(void) {
    if (gEnv.ST && gEnv.ST->ConOut) gEnv.ST->ConOut->ClearScreen(gEnv.ST->ConOut);
}

static TND_ABI void api_gotoxy(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (gEnv.ST && gEnv.ST->ConOut)
        gEnv.ST->ConOut->SetCursorPosition(gEnv.ST->ConOut, (UINTN)x, (UINTN)y);
}

/* 阻塞读键，**保留扫描码** —— 方向键的 UnicodeChar 是 0，
 * 只看它等于什么都没读到。这是写全屏程序的前提。 */
static TND_ABI int api_getkey(void) {
    EFI_INPUT_KEY k;
    if (!gEnv.ST || !gEnv.ST->ConIn) return 0;
    for (;;) {
        if (!EFI_ERROR(gEnv.ST->ConIn->ReadKeyStroke(gEnv.ST->ConIn, &k)))
            return (int)(((UINT32)k.ScanCode << 16) | (UINT32)k.UnicodeChar);
        gEnv.BS->Stall(10000);
    }
}

static TND_ABI int api_cols(void) {
    UINTN c = 80, r = 25;
    if (gEnv.ST && gEnv.ST->ConOut && gEnv.ST->ConOut->Mode)
        gEnv.ST->ConOut->QueryMode(gEnv.ST->ConOut, gEnv.ST->ConOut->Mode->Mode, &c, &r);
    return (int)c;
}

static TND_ABI int api_rows(void) {
    UINTN c = 80, r = 25;
    if (gEnv.ST && gEnv.ST->ConOut && gEnv.ST->ConOut->Mode)
        gEnv.ST->ConOut->QueryMode(gEnv.ST->ConOut, gEnv.ST->ConOut->Mode->Mode, &c, &r);
    return (int)r;
}

static TND_ABI void api_setattr(int attr) { con_set_attr((UINTN)(attr & 0xFF)); }

static TND_ABI int api_getattr(void) {
    if (gEnv.ST && gEnv.ST->ConOut && gEnv.ST->ConOut->Mode)
        return (int)gEnv.ST->ConOut->Mode->Attribute;
    return 0x07;
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
    api_setattr, api_getattr
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
