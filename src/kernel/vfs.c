/* ============================================================================
 * TNDDOS -- virtual file system layer
 *
 * Underneath there is exactly one provider for now: the UEFI Simple File System
 * Protocol (FAT). What this layer does is translate DOS path semantics into
 * UEFI ones:
 *
 *   C:\EFI\TNDOS\KERNEL.EFI   ->  \EFI\TNDOS\KERNEL.EFI
 *   .\FOO.TXT                 ->  <current dir>\FOO.TXT
 *   ..\BAR                    ->  parent\BAR
 *   D:FOO                     ->  explicit drive (only C: is mounted today)
 *
 * Per the plan, this layer will later sit on TNDDOS's own FAT32 driver with the
 * interface unchanged.
 * ==========================================================================*/
#include "tnd.h"

#define TND_MAX_DRIVES 26          /* A..Z —— 一块盘上多个分区是常态，4 个不够 */
#define TND_MAX_COMPS  32

typedef struct {
    int   used;
    char  letter;
    char  label[16];
    EFI_FILE_PROTOCOL *root;
} TND_DRIVE;

static TND_DRIVE gDrives[TND_MAX_DRIVES];
static int  gDriveCount = 0;
static char gCwd[TND_MAX_PATH] = "\\";
static char gCurDrive = 'C';

const TND_MODULE_DEF gModVfs = { "vfs", "virtual file system / DOS path + drive mapping", vfs_init };

/* thin alias so the rest of the code keeps reading naturally */
const char *vfs_efi_error(EFI_STATUS s) { return t_status_str(s); }

int vfs_init(void) {
    if (!gEnv.Root) { log_kv("vfs", "no root handle, mount failed"); return 0; }
    gDrives[0].used = 1;
    gDrives[0].letter = 'C';
    t_strcpy(gDrives[0].label, "TNDDOS");
    gDrives[0].root = gEnv.Root;
    gDriveCount = 1;
    gCurDrive = 'C';

    /* ================================================================
     * **同时把 blk + FatFs 那条路也挂起来。**
     *
     * 两套都在：退出之前固件还活着，退出之后只剩 FatFs。
     * 但**只要 FatFs 挂上了，优先用它** —— 理由很硬：
     *
     *   我们自己往盘上写过之后，固件那份 FAT 缓存立刻过期，
     *   它再被问 "EFITNDOS 里有什么" 时会返回 EFI_MEDIA_CHANGED。
     *   实测就是这样：fatwrite 成功了，紧接着 dir 就打不开目录。
     *
     * 所以从第一个命令开始就走 FatFs，固件那条只做退路。
     * ================================================================ */
    {
        int bi, bn = blk_count();
        for (bi = 0; bi < bn && !fat_ready(); bi++) fat_mount(bi);
        if (fat_ready()) log_puts("[log] vfs: using blk + FatFs (firmware SFS is only a fallback)\r\n");
        else             log_puts("[log] vfs: no FAT volume on any block device, staying on the firmware SFS\r\n");
    }

    /* ================================================================
     * **初始目录不能写死成 "\"。**
     *
     * 真机上实测：提示符是 C:\>，而 autoexec.bat 在 \EFI\TNDOS\ 里 ——
     * 于是"脚本没有加载"。而在 QEMU 里之所以看不出问题，是因为启动流程
     * 恰好还能找到它。
     *
     * 所以：**优先进入内核自己所在的那个目录**（\EFI\TNDOS），
     * 进不去才退回根目录。这不是讨好用户，是唯一说得通的定义 ——
     * 系统的配置文件就在那儿。
     * ================================================================ */
    {
        EFI_FILE_PROTOCOL *probe = 0;
        EFI_STATUS ps;

        gCwd[0] = '\\'; gCwd[1] = 0;
        ps = gEnv.Root->Open(gEnv.Root, &probe, L"\\EFI\\TNDOS",
                             EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(ps) && probe) {
            probe->Close(probe);
            t_strcpy(gCwd, "\\EFI\\TNDOS");
            log_puts("[log] vfs: cwd = \\EFI\\TNDOS (the kernel's own directory)\r\n");
        } else {
            log_puts("[log] vfs: cwd = \\ (no \\EFI\\TNDOS on this volume)\r\n");
        }
    }

    /* 把"这是哪个卷"打到屏幕上 —— 真机上 dir 出来是空的，而我们不知道
     * C: 到底指到了哪个卷。卷标和容量是最好的身份证。 */
    con_puts("  drive C: label=\"");
    con_puts(gDrives[0].label);
    con_puts("\"  cwd=");
    con_puts(gCwd);
    con_puts("\r\n");

    log_puts("[log] vfs: mounted C: -> ESP root\r\n");
    vfs_mount_all();                 /* 其余可挂载的卷依次 D: E: F: ... */
    return 1;
}

/* ---------------------------------------------------------- 卷枚举与挂载
 * 把**启动卷之外**的、固件认得的 FAT 卷，依次挂成 D: E: F: ...
 *
 * **这一层不解析 MBR / GPT。** 走的是 EFI_SIMPLE_FILE_SYSTEM，分区表是固件
 * 解析好的：能挂载的卷才会以句柄出现，MSR / 未格式化 / 非 FAT 的**根本不会
 * 出现**。所以"MSR 拒绝挂载"是天然成立的 —— 它压根不是文件系统。
 *
 * 自己解析分区表（GPT 备份头、CRC32、混合 MBR、4K 对齐……）是
 * **换掉固件、换用我们自己的 FAT 驱动**那一次的活，不是这一次。
 *
 * 识别启动卷靠 gEnv.EspHandle —— 直接比句柄。**不用设备路径比较**：
 * DevicePathSize 不在我们的 BootServices 结构里，而句柄本来就有。 */

/* EFI_FILE_SYSTEM_INFO 的 GUID 与结构。efi.h 里没有，就近放这儿。
 * ReadOnly 是 1 字节、后面 7 字节填充 —— 不显式写出来就会读错卷标。 */
static const EFI_GUID kFsInfoGuid = {
    0x09576E93, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

typedef struct {
    UINT64 Size;
    UINT8  ReadOnly;
    UINT8  Pad0[7];
    UINT64 VolumeSize;
    UINT64 FreeSpace;
    UINT32 BlockSize;
    CHAR16 VolumeLabel[1];
} TND_FS_INFO;

/* 读卷标。读不到就留空 —— 宁可空着，也不编一个出来。 */
static void read_label(EFI_FILE_PROTOCOL *root, char *out, UINTN cap) {
    UINT8 buf[512];
    UINTN sz = sizeof(buf);
    TND_FS_INFO *fi = (TND_FS_INFO *)buf;
    out[0] = 0;
    if (!root->GetInfo) return;
    {
        /* 把返回码记到串口 —— "读到空卷标" 和 "根本没读到" 是两件事，
         * 屏幕上看起来一模一样，只有这里分得开。 */
        EFI_STATUS ls = root->GetInfo(root, (EFI_GUID *)&kFsInfoGuid, &sz, buf);
        log_kv_u64("vfs.labelInfo", (UINT64)ls);
        if (EFI_ERROR(ls)) return;
    }
    {
        UINTN j = 0;
        for (UINTN i = 0; fi->VolumeLabel[i] && j + 1 < cap; i++) {
            CHAR16 c = fi->VolumeLabel[i];
            out[j++] = (c < 0x80) ? (char)c : '?';
        }
        out[j] = 0;
    }
}

/* DOS 的规矩：直接敲 "D:" 就切到 D 盘。
 * **这不是语法糖** —— 挂上了盘符却切不过去，等于没挂。
 * 有 X: 的形状但没挂载时也要把它接下，好报一句 invalid drive，
 * 而不是让它掉进 "Bad command or file name" —— 那样用户会以为是自己打错了。 */
static TND_DRIVE *drive_of(char letter);   /* 定义在后面，先声明 */

int vfs_try_chdrive(const char *cmd) {
    char letter;
    if (!cmd || !cmd[0] || cmd[1] != ':') return 0;
    if (cmd[2] != 0 && cmd[2] != '\\' && cmd[2] != '/') return 0;
    letter = (char)t_toupper((unsigned char)cmd[0]);
    if (letter < 'A' || letter > 'Z') return 0;
    if (!drive_of(letter)) {
        con_puts("  "); con_putc(letter); con_puts(":  invalid drive\r\n");
        return 1;
    }
    gCurDrive = letter;
    gCwd[0] = '\\'; gCwd[1] = 0;   /* DOS 切盘符回到该盘根目录（我们还没做每盘独立 CWD） */
    return 1;
}

int vfs_mount_all(void) {
    typedef EFI_STATUS (*TND_LOCATE_BUF)(int, EFI_GUID *, VOID *, UINTN *, EFI_HANDLE **);
    TND_LOCATE_BUF locate = (TND_LOCATE_BUF)gEnv.BS->LocateHandleBuffer;
    EFI_GUID guid = gEfiSimpleFileSystemProtocolGuid;
    EFI_HANDLE *bufs = 0;
    UINTN n = 0;
    int mounted = 0;
    char letter = 'D';

    if (!locate) return 0;
    /* 2 = ByProtocol（EFI_LOCATE_SEARCH_TYPE: AllHandles=0, ByRegisterNotify=1, ByProtocol=2） */
    if (EFI_ERROR(locate(2, &guid, 0, &n, &bufs)) || !bufs) {
        log_puts("[log] vfs: no SimpleFileSystem handles\r\n");
        return 0;
    }

    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        EFI_FILE_PROTOCOL *root = 0;
        char one[2];

        if (bufs[i] == gEnv.EspHandle) continue;    /* 启动卷已经是 C: 了，别再挂一遍 */
        if (gDriveCount >= TND_MAX_DRIVES || letter > 'Z') break;
        if (EFI_ERROR(gEnv.BS->HandleProtocol(bufs[i], &guid, (void **)&fs)) || !fs) continue;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        gDrives[gDriveCount].used   = 1;
        gDrives[gDriveCount].letter = letter;
        gDrives[gDriveCount].root   = root;
        read_label(root, gDrives[gDriveCount].label, sizeof(gDrives[gDriveCount].label));
        if (!gDrives[gDriveCount].label[0]) t_strcpy(gDrives[gDriveCount].label, "(no label)");
        gDriveCount++;
        mounted++;

        one[0] = letter; one[1] = 0;
        log_puts("[log] vfs: mounted "); log_puts(one); log_puts(":  label=");
        log_puts(gDrives[gDriveCount - 1].label); log_puts("\r\n");
        letter++;
    }

    if (gEnv.BS->FreePool) gEnv.BS->FreePool(bufs);
    return mounted;
}

static TND_DRIVE *drive_of(char letter) {
    for (int i = 0; i < gDriveCount; i++)
        if (gDrives[i].used && gDrives[i].letter == letter) return &gDrives[i];
    return 0;
}

const char *vfs_cwd(void) { return gCwd; }
char vfs_drive(void) { return gCurDrive; }
void vfs_set_drive(char letter) {
    letter = (char)t_toupper((unsigned char)letter);
    if (drive_of(letter)) gCurDrive = letter;
}

static void path_normalize(const char *in, char *out, UINTN cap) {
    char comps[TND_MAX_COMPS][64];
    int n = 0;
    const char *p = in;

    while (*p && n < TND_MAX_COMPS) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        char c[64]; UINTN k = 0;
        while (*p && *p != '\\' && *p != '/' && k + 1 < sizeof(c)) c[k++] = *p++;
        c[k] = 0;
        while (*p && *p != '\\' && *p != '/') p++;
        if (!t_strcmp(c, ".")) continue;
        if (!t_strcmp(c, "..")) { if (n > 0) n--; continue; }
        t_strncpy(comps[n], c, sizeof(comps[0]));
        n++;
    }

    UINTN o = 0;
    if (o + 1 < cap) out[o++] = '\\';
    for (int i = 0; i < n; i++) {
        UINTN l = t_strlen(comps[i]);
        if (o + l + 2 >= cap) break;
        for (UINTN k = 0; k < l; k++) out[o++] = comps[i][k];
        if (i + 1 < n) out[o++] = '\\';
    }
    out[o] = 0;
    if (o == 0) { out[0] = '\\'; out[1] = 0; }
}

EFI_STATUS vfs_resolve(const char *dos, char *out, UINTN cap) {
    /* **退出之后这里一律拒绝。**
     *
     * 路径解析要问固件（卷列表、当前目录都在 SFS 里）。退出之后那些代码
     * 已经没了 —— 结果是跳进被回收的内存，屏幕上看到的是：
     *
     *   *** CPU EXCEPTION ***
     *   vector : 13   general protection
     *   RIP    : 0x0000000000001001        <- 不是地址，垃圾
     *
     * 实测就是这么崩的（在 cd efi\tndos 之后）。所以这里挡住，
     * 让它变成一句"不支持"，而不是一次 #GP。
     *
     * dir 不受影响：它在上面就分岔到 blk+fat 那条路了。 */
    if (gBsGone) return EFI_UNSUPPORTED;

    char base[TND_MAX_PATH];
    char letter = gCurDrive;

    while (*dos == ' ') dos++;
    if (dos[0] && dos[1] == ':') {
        letter = (char)t_toupper((unsigned char)dos[0]);
        dos += 2;
    }
    if (!drive_of(letter)) return EFI_NOT_FOUND;

    if (dos[0] == '\\' || dos[0] == '/') {
        t_strncpy(base, dos, sizeof(base));
    } else {
        t_strncpy(base, gCwd, sizeof(base));
        UINTN l = t_strlen(base);
        if (l && base[l - 1] != '\\' && l + 1 < sizeof(base)) { base[l] = '\\'; base[l + 1] = 0; }
        t_strncpy(base + t_strlen(base), dos, sizeof(base) - t_strlen(base));
    }

    path_normalize(base, out, cap);
    return EFI_SUCCESS;
}

EFI_STATUS vfs_open(const char *dos, int write, EFI_FILE_PROTOCOL **out) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_STATUS s;
    TND_DRIVE *d;

    if (out) *out = 0;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    if (!t_strcmp(path, "\\")) return EFI_ACCESS_DENIED;   /* the root itself: use vfs_dir */

    d = drive_of(gCurDrive);
    if (!d || !d->root) return EFI_NOT_FOUND;

    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    s = d->root->Open(d->root, out, wname,
                      write ? (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) : EFI_FILE_MODE_READ, 0);
    return s;
}

/* 带 CREATE 的打开。给 TNX 程序的 TND_O_CREATE 用。 */
EFI_STATUS vfs_create(const char *dos, EFI_FILE_PROTOCOL **out) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_STATUS s;
    TND_DRIVE *d = drive_of(gCurDrive);

    if (out) *out = 0;
    if (!d || !d->root) return EFI_NOT_FOUND;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    if (!t_strcmp(path, "\\")) return EFI_ACCESS_DENIED;

    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    return d->root->Open(d->root, out, wname,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
}

/* 截断成 0 字节。UEFI 没有 truncate，靠 SetInfo 把 FileSize 写成 0。 */
EFI_STATUS vfs_truncate(EFI_FILE_PROTOCOL *f) {
    static UINT8 info[512];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    UINTN sz = sizeof(info);
    EFI_STATUS s;
    if (!f) return EFI_INVALID_PARAMETER;
    s = f->GetInfo(f, &gEfiFileInfoGuid, &sz, info);
    if (EFI_ERROR(s)) return s;
    fi->FileSize = 0;
    /* SetInfo 的参数不是 const（GetInfo 是），要显式转一下 */
    s = f->SetInfo(f, (EFI_GUID *)&gEfiFileInfoGuid, sz, info);
    if (!EFI_ERROR(s)) f->SetPosition(f, 0);
    return s;
}

static int use_fatfs(void);      /* 定义在后面 */

int vfs_exists(const char *dos) {
    EFI_FILE_PROTOCOL *f = 0;
    /* **"文件在不在"也走 FatFs。**
     * tnx_find() 靠这个函数找程序，所以这一步决定了 tree/attrib/fc
     * 这些外部命令能不能跑起来。之前它走固件，于是屏幕上只有一句
     * "Bad command or file name" —— 而文件明明在目录里。 */
    if (use_fatfs()) return fat_exists(dos);
    if (EFI_ERROR(vfs_open(dos, 0, &f)) || !f) return 0;
    f->Close(f);
    return 1;
}

EFI_STATUS vfs_read_all(const char *dos, char *buf, UINTN cap, UINTN *len) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    UINTN total = 0;

    s = vfs_open(dos, 0, &f);
    if (EFI_ERROR(s) || !f) return s;
    for (;;) {
        UINTN want = 512;
        if (total + want >= cap) { want = cap - total - 1; if (!want) break; }
        s = f->Read(f, &want, buf + total);
        if (EFI_ERROR(s) || want == 0) break;
        total += want;
    }
    f->Close(f);
    buf[total] = 0;
    if (len) *len = total;
    if (total + 1 >= cap) {
        log_puts("[log] vfs_read_all: buffer full, file may be truncated: "); log_puts(dos); log_puts("\r\n");
        return EFI_BUFFER_TOO_SMALL;
    }
    return EFI_SUCCESS;
}

/* ---------------------------------------------------------------- DIR */
int vfs_dir(const char *dos) {
    char path[TND_MAX_PATH];
    EFI_FILE_PROTOCOL *dir = 0, *target = 0;
    TND_DRIVE *d = drive_of(gCurDrive);
    void *buf = 0;
    UINTN isz = 0;
    EFI_STATUS s;
    UINT64 files = 0, dirs = 0, bytes = 0;
    int closedDir = 0;
    (void)dir;

    /* ================================================================
     * **ExitBootServices 之后走我们自己的 FAT。**
     *
     * 在这之前，DIR 走的是固件的 SimpleFileSystem，退出之后它连同整块代码
     * 一起消失 —— 所以那时的 DIR 只能报错。
     *
     * 我们的 blk + fat 不需要任何固件服务（扇区是 TNX 驱动给的），
     * 所以这条路在退出之后依然成立。**这就是"接手"的具体形状。**
     *
     * 现在只支持根目录 —— 子目录要沿簇链走，那是下一步。
     * ================================================================ */
    /* **只要 FatFs 能挂上就走它。**
     *
     * 这里必须**再试一次挂载** —— 不能只靠 vfs_init。实测过：
     * 开机后第一次 dir 报 EFI_MEDIA_CHANGED，第二次就正常了。
     * 原因是 vfs_init 跑的时候驱动还没加载，blk_count() 是 0，
     * 那一挂不可能成功；之后再没有别的地方试过。
     *
     * 所以每次进来都问一句"挂上了吗，没挂就再试" —— blk 是幂等的，
     * 重复尝试的代价只是几次扇区读。 */
    if (!fat_ready()) {
        (void)fat_mount_any();
        /* **挂上之后立刻把 shell 的当前目录同步给 FatFs。**
         *
         * 这两个是**两个**当前目录：gCwd 是 shell 的（vfs_init 里设成 EFITNDOS），
         * FatFs 自己也有一个（默认 "."，也就是根）。
         * 不同步的话，f_getcwd 会诚实地告诉你 "."，而 dir 就列根目录 ——
         * 表头写 "Directory of C:." 看起来还挺像回事。
         *
         * 实测就是这么发现的：C:EFITNDOS> dir 列出了根目录的三个条目。 */
    }
    if (fat_ready()) {
        /* **把参数传下去。** 原来这里写的是 (void)dos + fat_report()，
         * 于是 dir 永远列根目录 —— 而且表头还写着 "Directory of C:\"，
         * 看起来像是对的。命令行列出错目录比报错危险得多。 */
        while (*dos == ' ') dos++;
        fat_list(*dos ? dos : 0);
        return 0;
    }
    if (gBsGone) {
        int bi, bn = blk_count();

        /* **先把块设备列出来。** 真机上"目录列不出来"可能有两个完全不同的原因：
         *   一个设备都没注册  -> 驱动没接上（或者扇区大小被 blk 拒了）
         *   有设备但挂不上    -> 文件系统起点/参数不对
         * 不把这两件事分开，用户和我们都只能猜。 */
        if (bn <= 0) {
            con_puts("  no block device at all\r\n");
            con_puts("  LOAD AHCI.TNX or NVME.TNX before EXITBOOT\r\n");
            return 0;
        }
        {
            const TND_BLOCK *b;
            con_puts("  block devices: ");
            for (bi = 0; bi < bn; bi++) {
                b = blk_at(bi);
                if (!b) continue;
                con_puts(b->Name ? b->Name : "?");
                con_puts(" ");
            }
            con_puts("\r\n");
        }

        if (!fat_ready())
            for (bi = 0; bi < bn && !fat_ready(); bi++) fat_mount(bi);
        if (!fat_ready()) {
            con_puts("  no FAT volume found on any of them\r\n");
            return 0;
        }
        fat_report();
        return 0;
    }

    if (!d) return 0;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) { con_puts("  invalid path\r\n"); return 0; }

    if (!t_strcmp(path, "\\")) {
        target = d->root;
    } else {
        s = vfs_open(dos, 0, &dir);
        if (EFI_ERROR(s) || !dir) {
            con_puts("  cannot open: "); con_puts(path); con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
            return 0;
        }
        target = dir; closedDir = 1;
    }

    isz = 0;
    s = target->GetInfo(target, &gEfiFileInfoGuid, &isz, 0);
    if (s != EFI_BUFFER_TOO_SMALL && EFI_ERROR(s)) isz = 4096;
    isz += 256;
    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, isz, &buf)) || !buf) {
        con_puts("  out of memory\r\n");
        if (closedDir) dir->Close(dir);
        return 0;
    }

    con_puts("\r\n Volume "); con_putc(gCurDrive); con_puts(":  Directory of ");
    con_puts(path); con_puts("\r\n\r\n");

    /* The read position MUST be rewound before enumerating.
     * EDK2's FAT driver leaves the position at EOF after a full enumeration, so
     * the next Read returns length 0 -- which showed up as "second DIR of the
     * volume root comes back empty". Non-root directories open a fresh handle
     * every time, so only the reused volume root handle was affected. */
    if (target->SetPosition) target->SetPosition(target, 0);

    for (;;) {
        UINTN sz = isz;
        s = target->Read(target, &sz, buf);
        if (EFI_ERROR(s) || sz == 0) break;
        {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
            char name[TND_MAX_PATH];
            UINTN k = 0;
            while (fi->FileName[k] && k + 1 < sizeof(name)) { name[k] = (char)(fi->FileName[k] & 0xFF); k++; }
            name[k] = 0;
            if (!name[0]) continue;
            /* classic DOS DIR does not list . or .. */
            if (!t_strcmp(name, ".") || !t_strcmp(name, "..")) continue;

            if (fi->Attribute & EFI_FILE_DIRECTORY) {
                /* 目录用亮青 —— DOS 本身没颜色（那是 DIR /不同 时代的事），
                 * 但既然 UEFI 的 ConOut 支持属性，白给的可读性没理由不要。 */
                dirs++;
                con_set_attr(TND_ATTR(TND_LIGHTCYAN, TND_BLACK));
                con_puts("   <DIR>          ");
                t_upper(name);
                con_puts(name); con_puts("\r\n");
                con_reset_attr();
            } else {
                files++; bytes += fi->FileSize;
                con_puts("   ");
                char nb[24]; t_utoa(fi->FileSize, nb);
                for (UINTN i = t_strlen(nb); i < 10; i++) con_puts(" ");
                con_puts(nb); con_puts("  ");
                if (fi->ModificationTime.Year > 1970) {
                    char yb[8]; t_utoa(fi->ModificationTime.Year, yb);
                    con_puts(yb); con_puts("-");
                    if (fi->ModificationTime.Month < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Month); con_puts("-");
                    if (fi->ModificationTime.Day < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Day); con_puts(" ");
                    if (fi->ModificationTime.Hour < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Hour); con_puts(":");
                    if (fi->ModificationTime.Minute < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Minute); con_puts("  ");
                } else {
                    con_puts("                   ");
                }
                /* 可执行文件亮绿，普通文件浅灰 */
                {
                    int nl = (int)t_strlen(name);
                    int ex = 0;
                    if (nl > 4 && name[nl - 4] == '.') {
                        ex = (!t_stricmp(name + nl - 4, ".TNX") || !t_stricmp(name + nl - 4, ".EFI") ||
                              !t_stricmp(name + nl - 4, ".EXE") || !t_stricmp(name + nl - 4, ".COM") ||
                              !t_stricmp(name + nl - 4, ".BAT"));
                    }
                    con_set_attr(ex ? TND_ATTR(TND_LIGHTGREEN, TND_BLACK)
                                    : TND_ATTR(TND_LIGHTGRAY, TND_BLACK));
                }
                t_upper(name);
                con_puts(name); con_puts("\r\n");
                con_reset_attr();
            }
        }
    }

    con_puts("\r\n   "); con_u64(files); con_puts(" file(s)   ");
    con_u64(bytes); con_puts(" bytes     "); con_u64(dirs); con_puts(" dir(s)\r\n");

    gEnv.BS->FreePool(buf);
    if (closedDir) dir->Close(dir);
    log_kv_u64("dir.files", files);
    log_kv_u64("dir.dirs", dirs);
    return 1;
}

int vfs_type(const char *dos) {
    static char buf[TND_FILE_CAP];
    UINTN len = 0;
    EFI_STATUS s = vfs_read_all(dos, buf, TND_FILE_CAP, &len);
    if (EFI_ERROR(s)) {
        con_puts("  cannot read "); con_puts(dos); con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
        return 0;
    }
    con_puts(buf);
    if (len && buf[len - 1] != '\n') con_puts("\r\n");
    log_kv_u64("type.bytes", len);
    return 1;
}

int vfs_chdir(const char *dos) {
    char path[TND_MAX_PATH];
    TND_DRIVE *d;
    EFI_STATUS s;

    while (*dos == ' ') dos++;
    if (dos[0] && dos[1] == ':') {
        char letter = (char)t_toupper((unsigned char)dos[0]);
        if (!drive_of(letter)) { con_puts("  invalid drive\r\n"); return 0; }
        gCurDrive = letter;
        d = drive_of(letter);
        if (!dos[2]) { gCwd[0] = '\\'; gCwd[1] = 0; return 1; }
    }
    d = drive_of(gCurDrive);
    if (!d) return 0;

    /* ================================================================
     * **FatFs 挂上了就用它的 f_chdir。**
     *
     * 用户明确要求过"cd 不能被拦"。之前 cd 走固件，退出之后必定失败，
     * 所以被 vfs_resolve 挡着；现在文件系统是自己的了，就不该再挡。
     *
     * gCwd 用 f_getcwd 回读 —— 不自己拼路径。拼路径容易和 FatFs 的
     * 规范化规则（大小写、斜杠、"." 和 ".."）对不上，而那类不一致
     * 事后极难查。让它告诉我们它到底在哪。
     * ================================================================ */
    if (fat_ready()) {
        /* **不再用 f_chdir / f_getcwd。**
         *
         * 新的 FAT 驱动是无状态的：所有函数都收绝对路径，当前目录由 vfs
         * 自己那份 gCwd 维护。这样只有**一个**当前目录，不会出现
         * "gCwd 说在 A、驱动说在 B" 那种两个真相的局面。
         *
         * 路径规范化（. / .. / 多余分隔符）统一在 fat_resolve 里做 ——
         * 规则只有一份，调用方不用各写一遍。 */
        char abs[TND_MAX_PATH];
        char joined[TND_MAX_PATH];

        if (dos[0] == 92 || dos[0] == 47) {
            fat_resolve(dos, abs, sizeof(abs));
        } else {
            UINTN l;
            t_strncpy(joined, gCwd, sizeof(joined));
            l = t_strlen(joined);
            if (l + 1 < sizeof(joined)) { joined[l] = 92; joined[l + 1] = 0; }
            t_strncpy(joined + t_strlen(joined), dos, sizeof(joined) - t_strlen(joined));
            fat_resolve(joined, abs, sizeof(abs));
        }

        if (!(abs[0] == 92 && abs[1] == 0) && !fat_exists(abs)) {
            con_puts("  invalid directory\r\n");
            return 0;
        }
        t_strncpy(gCwd, abs, TND_MAX_PATH);
        return 1;    }

    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) { con_puts("  invalid path\r\n"); return 0; }

    if (!t_strcmp(path, "\\")) { t_strncpy(gCwd, "\\", TND_MAX_PATH); return 1; }

    {
        EFI_FILE_PROTOCOL *f = 0;
        CHAR16 wname[TND_MAX_PATH + 4];
        t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
        s = d->root->Open(d->root, &f, wname, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(s) || !f) {
            con_puts("  no such directory: "); con_puts(path); con_puts("\r\n");
            return 0;
        }
        /* make sure it really is a directory */
        {
            static UINT8 info[256];
            UINTN sz = sizeof(info);
            EFI_STATUS gs = f->GetInfo(f, &gEfiFileInfoGuid, &sz, info);
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
            if (EFI_ERROR(gs) || !(fi->Attribute & EFI_FILE_DIRECTORY)) {
                con_puts("  not a directory: "); con_puts(path); con_puts("\r\n");
                f->Close(f);
                return 0;
            }
        }
        f->Close(f);
    }

    t_strncpy(gCwd, path, TND_MAX_PATH);
    log_puts("[log] cd -> "); log_puts(gCwd); log_puts("\r\n");
    return 1;
}

/* ------------------------------------------------- directories / files */
static EFI_STATUS create_dir(const char *dos) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    TND_DRIVE *d = drive_of(gCurDrive);
    if (!d) return EFI_NOT_FOUND;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    s = d->root->Open(d->root, &f, wname,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                      EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(s) && f) f->Close(f);
    return s;
}

/* **只要 FatFs 挂上了，写操作就走它。** 理由和 vfs_dir 一样：
 * 我们自己写过盘之后，固件那份 FAT 缓存就过期了，它的 Delete/Open 会
 * 返回 EFI_MEDIA_CHANGED。写操作不能留在那条路上。 */
static int use_fatfs(void) {
    if (!fat_ready()) {
        int bi, bn = blk_count();
        for (bi = 0; bi < bn && !fat_ready(); bi++) fat_mount(bi);
    }
    return fat_ready();
}

int vfs_mkdir(const char *dos) {
    if (use_fatfs()) return fat_mkdir(dos) == 0;
    { EFI_STATUS s = create_dir(dos);
    if (EFI_ERROR(s)) { con_puts("  mkdir failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    con_puts("  created directory "); con_puts(dos); con_puts("\r\n");
    return 1; }
}

int vfs_rmdir(const char *dos) {
    if (use_fatfs()) return fat_unlink(dos) == 0;
    { EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s = vfs_open(dos, 1, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    s = f->Delete(f);
    if (EFI_ERROR(s)) { con_puts("  delete failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); f->Close(f); return 0; }
    con_puts("  removed directory "); con_puts(dos); con_puts("\r\n");
    return 1; }
}

int vfs_unlink(const char *dos) {
    if (use_fatfs()) return fat_unlink(dos) == 0;
    { EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s = vfs_open(dos, 1, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    s = f->Delete(f);
    if (EFI_ERROR(s)) { con_puts("  delete failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); f->Close(f); return 0; }
    con_puts("  deleted "); con_puts(dos); con_puts("\r\n");
    return 1; }
}

int vfs_copy(const char *a, const char *b) {
    if (use_fatfs()) return fat_copy(a, b) == 0;
    { EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    void *buf = 0;
    UINTN got = 0, total = 0;

    s = vfs_open(a, 0, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open source: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }

    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, TND_BLOB_CAP, &buf)) || !buf) {
        f->Close(f); con_puts("  out of memory\r\n"); return 0;
    }
    for (;;) {
        UINTN want = 1024;
        s = f->Read(f, &want, (UINT8 *)buf + total);
        if (EFI_ERROR(s) || want == 0) break;
        total += want;
        if (total + 1024 > TND_BLOB_CAP) break;
    }
    f->Close(f);

    {
        char path[TND_MAX_PATH];
        CHAR16 wname[TND_MAX_PATH + 4];
        TND_DRIVE *d = drive_of(gCurDrive);
        EFI_FILE_PROTOCOL *o = 0;
        if (!d) { gEnv.BS->FreePool(buf); return 0; }
        if (EFI_ERROR(vfs_resolve(b, path, sizeof(path)))) { gEnv.BS->FreePool(buf); return 0; }
        t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
        s = d->root->Open(d->root, &o, wname,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (EFI_ERROR(s) || !o) {
            con_puts("  cannot open target: "); con_puts(vfs_efi_error(s)); con_puts("\r\n");
            gEnv.BS->FreePool(buf); return 0;
        }
        got = total;
        s = o->Write(o, &got, buf);
        o->Close(o);
        if (EFI_ERROR(s) || got != total) {
            con_puts("  write failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n");
            gEnv.BS->FreePool(buf); return 0;
        }
    }
    gEnv.BS->FreePool(buf);
    con_puts("  copied "); con_puts(a); con_puts(" -> "); con_puts(b);
    con_puts("  ("); con_u64(total); con_puts(" bytes)\r\n");
    log_kv_u64("copy.bytes", total);
    return 1; }
}

int vfs_rename(const char *a, const char *b) {
    /* **FatFs 有真正的 rename**（改目录项），不用"拷贝再删除"。
     * 老路那样做在大文件上既慢又可能半途失败留下半个副本。 */
    if (use_fatfs()) return fat_rename(a, b) == 0;
    /* UEFI has no rename, so it is copy-then-delete-source */
    if (!vfs_copy(a, b)) return 0;
    if (!vfs_unlink(a)) { con_puts("  !! copied, but the source could not be deleted\r\n"); return 0; }
    return 1;
}

void vfs_report(void) {
    for (int i = 0; i < gDriveCount; i++) {
        if (!gDrives[i].used) continue;
        con_puts("  ");
        con_putc(gDrives[i].letter);
        con_puts(":  ");
        con_puts(gDrives[i].label);
        con_puts("   (UEFI Simple File System / FAT)\r\n");
    }
    con_puts("  current directory: ");
    con_putc(gCurDrive);
    con_puts(":");
    con_puts(gCwd);
    con_puts("\r\n");
}