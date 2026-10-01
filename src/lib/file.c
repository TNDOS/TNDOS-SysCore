/* ============================================================================
 * TNDDOS — ESP 文件访问
 * 现阶段完全借 UEFI Simple File System Protocol（符合"不早退 UEFI"的决定）
 * ==========================================================================*/
#include "tnd.h"

/* 打开本映像所在设备的根目录，并做一次运行期自检：
 * LoadedImage->SystemTable 必须等于我们收到的 SystemTable —— 不等就说明
 * EFI_LOADED_IMAGE_PROTOCOL 的字段偏移写错了。这类错误编译器不会报。 */
EFI_STATUS t_open_root(EFI_HANDLE image) {
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_STATUS s;

    s = gEnv.BS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (void **)&li);
    if (EFI_ERROR(s) || !li) { log_kv("LoadedImage", "HandleProtocol FAILED"); return s; }

    if (li->SystemTable != gEnv.ST) {
        log_puts("[log] !! LoadedImage.SystemTable != SystemTable: EFI_LOADED_IMAGE_PROTOCOL offsets are wrong\r\n");
    } else {
        log_puts("[log] selfcheck: LoadedImage.SystemTable == SystemTable  OK\r\n");
    }
    log_puts("[log] LoadedImage.ParentHandle  = "); log_hex((UINT64)li->ParentHandle); log_puts("\r\n");
    log_puts("[log] LoadedImage.DeviceHandle  = "); log_hex((UINT64)li->DeviceHandle); log_puts("\r\n");
    log_puts("[log] LoadedImage.ImageBase     = "); log_hex((UINT64)li->ImageBase);     log_puts("\r\n");
    log_kv_u64("LoadedImage.ImageSize", li->ImageSize);

    if (!li->DeviceHandle) { log_kv("SFS", "DeviceHandle is NULL"); return EFI_NOT_FOUND; }

    s = gEnv.BS->HandleProtocol(li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&sfs);
    if (EFI_ERROR(s) || !sfs) { log_kv("SimpleFileSystem", "HandleProtocol FAILED"); return s; }

    s = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(s) || !root) { log_kv("OpenVolume", "FAILED"); return s; }

    gEnv.Root = root;
    gEnv.EspHandle = li->DeviceHandle;
    log_puts("[log] ESP root opened\r\n");
    return EFI_SUCCESS;
}

/* 把一个 ASCII 路径整体读进 buf。成功时 *outLen 是实际长度，并补一个 0。 */
EFI_STATUS t_read_file(const char *name, char *buf, UINTN cap, UINTN *outLen) {
    EFI_FILE_PROTOCOL *f = 0;
    CHAR16 wname[260];
    EFI_STATUS s;
    UINTN total = 0;

    if (!gEnv.Root) return EFI_NOT_FOUND;
    t_ascii_to_u16(name, wname, 260);

    s = gEnv.Root->Open(gEnv.Root, &f, wname, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !f) { log_puts("[log] open FAILED: "); log_puts(name); log_puts("\r\n"); return s; }

    for (;;) {
        UINTN want = 512;
        if (total + want >= cap) { want = cap - total - 1; if (!want) break; }
        s = f->Read(f, &want, buf + total);
        if (EFI_ERROR(s) || want == 0) break;
        total += want;
    }
    f->Close(f);
    buf[total] = 0;
    if (outLen) *outLen = total;
    log_puts("[log] read "); log_u64(total); log_puts(" bytes <- "); log_puts(name); log_puts("\r\n");

    /* 缓冲填满 = 文件可能被截断。绝不能让这种事静默发生：
     * 上次 kernel.efi 被砍掉 1.5KB，LoadImage 只回了一个没头没脑的 Unsupported。
     *
     * 注意这里**只判断"填满了"**。早先的版本还加了 buf[total-1] != 0
     * 来避免"文件刚好 cap-1 字节"的误报 —— 结果是只要截断点那个字节
     * 恰好是 0x00（PE 文件里到处都是），这个检查就永远不触发。
     * 一个会自己静默掉的"响亮报错"，比没有更危险。 */
    if (total + 1 >= cap) {
        log_puts("[log] !! buffer full, file may be truncated: "); log_puts(name);
        log_puts("  (cap="); log_u64(cap); log_puts(")\r\n");
        return EFI_BUFFER_TOO_SMALL;
    }
    return EFI_SUCCESS;
}

/* 按文件实际大小分配并读入，调用方用 FreePool 释放。
 *
 * **不要**再用固定上限：那个数字迟早会被超过（这个坑踩过两次，
 * 第一次 16KB -> 17920，第二次 64KB -> 66048，两次的症状一模一样：
 * LoadImage 回一个没头没脑的 Unsupported）。大小是文件的属性，
 * 不是调用方该猜的东西。 */
EFI_STATUS t_read_file_alloc(const char *name, void **outBuf, UINTN *outLen) {
    EFI_FILE_PROTOCOL *f = 0;
    CHAR16 wname[260];
    UINTN size = 0;
    void *buf = 0;
    EFI_STATUS s;

    if (outBuf) *outBuf = 0;
    if (outLen) *outLen = 0;
    if (!gEnv.Root) return EFI_NOT_FOUND;

    t_ascii_to_u16(name, wname, 260);
    s = gEnv.Root->Open(gEnv.Root, &f, wname, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !f) { log_puts("[log] open FAILED: "); log_puts(name); log_puts("\r\n"); return s; }

    {
        static UINT8 info[512];
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
        UINTN isz = sizeof(info);
        s = f->GetInfo(f, &gEfiFileInfoGuid, &isz, info);
        if (EFI_ERROR(s)) { f->Close(f); return s; }
        size = (UINTN)fi->FileSize;
    }
    if (size == 0) { f->Close(f); return EFI_NOT_FOUND; }

    s = gEnv.BS->AllocatePool(EfiLoaderData, size + 1, &buf);
    if (EFI_ERROR(s) || !buf) { f->Close(f); return s; }

    {
        UINTN total = 0;
        for (;;) {
            UINTN want = 65536;
            s = f->Read(f, &want, (UINT8 *)buf + total);
            if (EFI_ERROR(s) || want == 0) break;
            total += want;
            if (total >= size) break;
        }
        f->Close(f);
        ((char *)buf)[total] = 0;
        if (outLen) *outLen = total;
        log_puts("[log] read "); log_u64(total); log_puts(" bytes <- "); log_puts(name);
        log_puts("  (file size "); log_u64(size); log_puts(")\r\n");
        if (total != size) {
            log_puts("[log] !! short read: got "); log_u64(total);
            log_puts(" of "); log_u64(size); log_puts("\r\n");
            gEnv.BS->FreePool(buf);
            return EFI_VOLUME_CORRUPTED;
        }
    }

    if (outBuf) *outBuf = buf; else gEnv.BS->FreePool(buf);
    return EFI_SUCCESS;
}
