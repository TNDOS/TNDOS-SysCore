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
    log_kv_u64("drv.count", (UINT64)gCount);
    log_kv_u64("drv.ok", (UINT64)gOk);
    log_kv_u64("drv.fail", (UINT64)gFail);
}
