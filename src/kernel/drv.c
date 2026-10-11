/* ============================================================================
 * TNDDOS — 驱动加载器（M4 起点）
 *
 * 语义按方案：   DEVICE=<文件>   ≈   UEFI Shell 的   load fs0:\<文件>
 * 也就是：把该文件当 UEFI 映像读进来 -> LoadImage -> StartImage。
 *
 * 几点实现上的注意：
 *   1) 路径走 VFS，所以 DOS 路径语义（\ / . .. / 驱动器）都能用。
 *   2) 只写文件名的（DEVICE=VGA.EFI）先去 \EFI\TNDOS\DRIVERS\ 找，
 *      找不到再当相对路径算 —— 这是 TNDDOS 的约定。
 *   3) 用内存缓冲 LoadImage 时固件不会把设备句柄带下去，必须显式补上，
 *      否则驱动自己 HandleProtocol 会拿到 NULL（这点 M0 就踩过）。
 *   4) 映像缓冲故意不释放：留着最保险，代价是每个驱动几十 KB。
 *   5) 驱动加载失败绝不能让系统起不来 —— 记一笔，继续。
 * ==========================================================================*/
#include "tnd.h"

typedef struct {
    int        used;
    char       path[TND_MAX_PATH];
    EFI_HANDLE handle;
    void      *imageBuf;
    UINTN      imageSize;
    EFI_STATUS loadStatus;
    EFI_STATUS startStatus;
} TND_DRIVER;

static TND_DRIVER gDrv[TND_MAX_DRIVERS];

/* ---------------------------------------------- 已登记的驱动（v2.5 驱动模型）
 *
 * **这和上面那个 gDrv 不是一回事**，两个概念别混：
 *   gDrv      记「加载过哪些驱动文件」—— 路径、状态、加载次数
 *   gDrvDesc  记「有哪些驱动登记了自己」—— 这是驱动模型的表
 *
 * 驱动是常驻的（住在 TNX_DRIVER_BASE 那个窗口），所以这里存的指针
 * **一直有效** —— 程序加载时只清程序窗口，碰不到驱动窗口。 */
void drv_scan(void);   /* 定义在下面，先声明 */

#define TND_MAX_DRV_DESC 16
static const TND_DRIVER_DESC *gDrvDesc[TND_MAX_DRV_DESC];
static int gDrvDescCount = 0;

/* 驱动登记自己。
 *
 * **名字重复返回失败，不静默覆盖** —— 覆盖会让「到底哪个在生效」变成一件
 * 说不清的事，而那是 bug 的温床。宁可拒绝加载，也不猜。 */
int drv_register(const TND_DRIVER_DESC *d) {
    if (!d || !d->Name) return -1;

    /* **必须登记自驱动窗口。** 这不是洁癖，理由是硬的：
     * 程序窗口里跑的 TNX 返回之后，下一个程序加载时会**把整个程序窗口清零**，
     * 那时候这里存下的指针就悬空了 —— 而症状是「某个驱动偶尔发疯」，极难查。
     *
     * 顺带把驱动模型的规矩钉死：**驱动住在驱动窗口，程序住在程序窗口。** */
    {
        UINT64 a = (UINT64)(UINTN)d;
        if (a < TNX_DRIVER_BASE || a >= TNX_DRIVER_BASE + TNX_WINDOW_SIZE) {
            log_puts("[log] drv: descriptor is not in the driver window\r\n");
            return -5;
        }
    }
    if (d->StructSize < sizeof(TND_DRIVER_DESC)) {
        log_puts("[log] drv: descriptor too small (old driver header?)\r\n");
        return -2;
    }
    for (int i = 0; i < gDrvDescCount; i++) {
        if (!t_stricmp(gDrvDesc[i]->Name, d->Name)) {
            log_puts("[log] drv: duplicate driver name: "); log_puts(d->Name); log_puts("\r\n");
            return -3;
        }
    }
    if (gDrvDescCount >= TND_MAX_DRV_DESC) return -4;

    gDrvDesc[gDrvDescCount++] = d;
    log_puts("[log] drv: registered \""); log_puts(d->Name);
    log_puts("\"  type="); log_u64(d->Type);
    log_puts("  total="); log_u64((UINT64)gDrvDescCount); log_puts("\r\n");

    /* 新驱动马上见一眼已有设备 —— 不等下一次扫描 */
    drv_scan();
    return 0;
}

int drv_desc_count(void) { return gDrvDescCount; }

/* ------------------------------------------------- 已知设备（内置）
 *
 * 这些是**板上就有、但枚举不出来**的东西：8042 键盘控制器、PIT、串口……
 * PCI 设备将来由 PCI 总线驱动枚举，不走这张表。
 *
 * PS/2 的 Base 是**数据端口号**（0x60），Size 是端口数（2：0x60 和 0x64）。 */
static const TND_DEVICE gBuiltin[] = {
    { sizeof(TND_DEVICE), TND_DRV_INPUT, 0, 0, 0x60, 2, 0, 0, 0, "PS2" },
};
#define TND_BUILTIN_COUNT ((int)(sizeof(gBuiltin) / sizeof(gBuiltin[0])))

/* ------------------------------------------------------------ 绑定实例
 *
 * 一个「驱动 x 设备」的组合。**状态挂在 ctx 上**，不放全局变量 ——
 * 一个驱动可能绑多个设备，全局变量会让它们互相打架。 */
typedef struct {
    const TND_DRIVER_DESC *desc;
    const TND_DEVICE      *dev;
    void                  *ctx;     /* 驱动自己填的实例指针 */
} TND_BINDING;

#define TND_MAX_BINDINGS 16
static TND_BINDING gBind[TND_MAX_BINDINGS];
static int gBindCount = 0;

int drv_binding_count(void) { return gBindCount; }

const TND_DRIVER_DESC *drv_binding_desc(int i) {
    return (i >= 0 && i < gBindCount) ? gBind[i].desc : 0;
}
const TND_DEVICE *drv_binding_dev(int i) {
    return (i >= 0 && i < gBindCount) ? gBind[i].dev : 0;
}
void *drv_binding_ctx(int i) {
    return (i >= 0 && i < gBindCount) ? gBind[i].ctx : 0;
}

/* 把这个设备交给一个驱动试试。
 * probe 说不认领 -> 什么都不做（**不算失败**，一个设备本来就可能没驱动认）。 */
static int offer(const TND_DRIVER_DESC *d, const TND_DEVICE *dev) {
    void *ctx;

    if (!d->probe || !d->attach) return 0;
    if (!d->probe(dev)) return 0;

    if (gBindCount >= TND_MAX_BINDINGS) {
        log_puts("[log] drv: too many bindings\r\n");
        return -1;
    }

    ctx = d->attach(dev);
    if (!ctx) {
        /* **认领了但初始化失败要吭声** —— 静默地把设备放着不管，
         * 症状是「设备明明在，就是不能用」，最难查。 */
        log_puts("[log] drv: attach FAILED for \"");
        log_puts(d->Name); log_puts("\" on "); log_puts(dev->Name); log_puts("\r\n");
        return -1;
    }

    gBind[gBindCount].desc = d;
    gBind[gBindCount].dev  = dev;
    gBind[gBindCount].ctx  = ctx;
    gBindCount++;

    log_puts("[log] drv: bound \""); log_puts(d->Name);
    log_puts("\" -> "); log_puts(dev->Name);
    log_puts("  bindings="); log_u64((UINT64)gBindCount); log_puts("\r\n");
    return 1;
}

/* 把已知设备过一遍，让每个已登记的驱动认领。
 *
 * **驱动登记之后要再扫一次** —— 否则后登记的驱动永远看不到已有的设备。
 * 这是驱动模型的标准做法（EDK II 的 ConnectController 是同一个意思）。 */
void drv_scan(void) {
    for (int i = 0; i < gDrvDescCount; i++) {
        for (int j = 0; j < TND_BUILTIN_COUNT; j++) {
            /* 已经绑过的组合不再试 —— 否则每次扫描都会重复 attach */
            int seen = 0;
            for (int k = 0; k < gBindCount; k++)
                if (gBind[k].desc == gDrvDesc[i] && gBind[k].dev == &gBuiltin[j]) { seen = 1; break; }
            if (seen) continue;
            offer(gDrvDesc[i], &gBuiltin[j]);
        }
    }
}

const TND_DRIVER_DESC *drv_desc(int i) {
    return (i >= 0 && i < gDrvDescCount) ? gDrvDesc[i] : 0;
}
static int gCount = 0, gOk = 0, gFail = 0;

const TND_MODULE_DEF gModDrv = { "drv", "driver loader (DEVICE= -> LoadImage + StartImage)", drv_init };

int drv_init(void) {
    for (int i = 0; i < TND_MAX_DRIVERS; i++) gDrv[i].used = 0;
    gCount = gOk = gFail = 0;
    return 1;
}

int drv_count(void)    { return gCount; }
int drv_ok_count(void) { return gOk; }

static TND_DRIVER *record(const char *path) {
    TND_DRIVER *d;
    if (gCount >= TND_MAX_DRIVERS) return 0;
    d = &gDrv[gCount++];
    d->used = 1;
    t_strncpy(d->path, path, TND_MAX_PATH);
    d->handle = 0; d->imageBuf = 0; d->imageSize = 0;
    d->loadStatus = 0; d->startStatus = 0;
    return d;
}

/* 只给文件名时，先到系统驱动目录找 */
static int has_separator(const char *s) {
    for (; s && *s; s++) if (*s == '\\' || *s == '/') return 1;
    return 0;
}

EFI_STATUS drv_load(const char *path) {
    char full[TND_MAX_PATH];
    char sizebuf[400];                 /* EFI_FILE_INFO 足够放下普通文件名 */
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)sizebuf;
    EFI_FILE_PROTOCOL *f = 0;
    EFI_HANDLE h = 0;
    TND_DRIVER *d;
    void *buf = 0;
    UINTN isz = sizeof(sizebuf), want, size = 0, total = 0;
    EFI_STATUS s, ss;

    while (*path == ' ') path++;
    if (!*path) return EFI_NOT_FOUND;

    if (!has_separator(path)) {
        t_strcpy(full, "\\EFI\\TNDOS\\DRIVERS\\");
        t_strncpy(full + t_strlen(full), path, TND_MAX_PATH - t_strlen(full));
    } else {
        t_strncpy(full, path, TND_MAX_PATH);
    }

    d = record(path);
    if (!d) { con_puts("          !! driver table full\r\n"); return EFI_OUT_OF_RESOURCES; }

    s = vfs_open(full, 0, &f);
    if (EFI_ERROR(s) || !f) {
        con_puts("          !! cannot open "); con_puts(full);
        con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
        d->loadStatus = s;
        gFail++;
        log_puts("[log] drv open FAILED: "); log_puts(full); log_puts("\r\n");
        return s;
    }

    s = f->GetInfo(f, &gEfiFileInfoGuid, &isz, sizebuf);
    if (EFI_ERROR(s)) {
        con_puts("          !! cannot read file info\r\n");
        f->Close(f); d->loadStatus = s; gFail++; return s;
    }
    size = (UINTN)fi->FileSize;
    if (size < 64) {
        con_puts("          !! file too small to be a PE image\r\n");
        f->Close(f); d->loadStatus = EFI_LOAD_ERROR; gFail++; return EFI_LOAD_ERROR;
    }

    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, size, &buf)) || !buf) {
        con_puts("          !! out of memory\r\n");
        f->Close(f); d->loadStatus = EFI_OUT_OF_RESOURCES; gFail++; return EFI_OUT_OF_RESOURCES;
    }

    while (total < size) {
        want = size - total;
        ss = f->Read(f, &want, (UINT8 *)buf + total);
        if (EFI_ERROR(ss) || want == 0) break;
        total += want;
    }
    f->Close(f);
    if (total != size) {
        con_puts("          !! short read\r\n");
        log_kv_u64("drv.read.want", size);
        log_kv_u64("drv.read.got", total);
        gEnv.BS->FreePool(buf); d->loadStatus = EFI_LOAD_ERROR; gFail++; return EFI_LOAD_ERROR;
    }

    log_puts("[log] drv: "); log_puts(full); log_puts("  ");
    log_u64(total); log_puts(" bytes\r\n");

    /* ==== 这一步就是 "load fs0:\xxx.efi" ==== */
    s = gEnv.BS->LoadImage(0 /*BootPolicy=FALSE*/, gEnv.ImageHandle, 0, buf, total, &h);
    d->loadStatus = s;
    if (EFI_ERROR(s) || !h) {
        con_puts("          !! LoadImage failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n");
        log_puts("[log] drv LoadImage status = "); log_hex((UINT64)s); log_puts("\r\n");
        gEnv.BS->FreePool(buf); gFail++;
        return s;
    }

    /* 内存缓冲加载不继承设备句柄，补上 */
    {
        EFI_LOADED_IMAGE_PROTOCOL *li = 0;
        if (!EFI_ERROR(gEnv.BS->HandleProtocol(h, &gEfiLoadedImageProtocolGuid, (void **)&li)) && li)
            li->DeviceHandle = gEnv.EspHandle;
    }

    ss = gEnv.BS->StartImage(h, 0, 0);
    d->startStatus = ss;
    d->handle = h;
    d->imageBuf = buf;
    d->imageSize = total;

    if (EFI_ERROR(ss)) {
        con_puts("          !! driver entry returned: "); con_puts(vfs_efi_error(ss)); con_puts("\r\n");
        gFail++;
        return ss;
    }

    con_puts("          loaded "); con_u64(total); con_puts(" bytes, entry returned EFI_SUCCESS\r\n");
    gOk++;
    return EFI_SUCCESS;
}

void drv_report(void) {
    con_puts("\r\n  Drivers (");
    con_u64((UINT64)gCount); con_puts(" total: "); con_u64((UINT64)gOk);
    con_puts(" ok, "); con_u64((UINT64)gFail); con_puts(" failed)\r\n");

    if (!gCount) { con_puts("    (none)\r\n"); return; }

    for (int i = 0; i < gCount; i++) {
        TND_DRIVER *d = &gDrv[i];
        if (!d->used) continue;
        con_puts("    ");
        if (!d->handle)                            con_puts("LOAD-FAIL ");
        else if (EFI_ERROR(d->startStatus))        con_puts("ENTRY-FAIL");
        else                                       con_puts("OK        ");
        con_puts(d->path);
        con_puts("   ");
        con_u64((UINT64)d->imageSize);
        con_puts(" bytes\r\n");
    }

    /* ---- 已登记的驱动（v2.5 驱动模型）
     * 和上面那段不是一回事：上面是「加载过哪些驱动文件」，
     * 这里是「有哪些驱动登记了自己、认领了什么」。 */
    con_puts("\r\n  Registered (");
    con_u64((UINT64)gDrvDescCount); con_puts(" driver"); if (gDrvDescCount != 1) con_puts("s");
    con_puts(", "); con_u64((UINT64)gBindCount); con_puts(" binding"); if (gBindCount != 1) con_puts("s");
    con_puts(")\r\n");

    if (!gDrvDescCount) {
        con_puts("    (none)\r\n");
    } else {
        for (int i = 0; i < gDrvDescCount; i++) {
            int n = 0;
            for (int k = 0; k < gBindCount; k++) if (gBind[k].desc == gDrvDesc[i]) n++;
            con_puts("    "); con_puts(gDrvDesc[i]->Name);
            con_puts("   type="); con_u64(gDrvDesc[i]->Type);
            con_puts("   bound="); con_u64((UINT64)n);
            con_puts("\r\n");
        }
        for (int i = 0; i < gBindCount; i++) {
            con_puts("      -> "); con_puts(gBind[i].dev->Name);
            con_puts("   ctx="); con_hex((UINT64)(UINTN)gBind[i].ctx);
            con_puts("\r\n");
        }
    }
    log_kv_u64("drv.count", (UINT64)gCount);
    log_kv_u64("drv.ok", (UINT64)gOk);
    log_kv_u64("drv.fail", (UINT64)gFail);
}
