/* ============================================================================
 * TNDDOS —— ESP 文件访问
 *
 * **整个文件都是过渡期代码。**
 *
 * 这里完全借 UEFI 的 SimpleFileSystem 来读文件 —— 而它的生死由固件决定：
 * ExitBootServices 之后这套协议连同代码一起消失。
 *
 * 所以它的每一处调用都标了 TND-TRANSITIONAL。去掉的条件写在每个标记后面，
 * 全部是同一件事：**VFS 完全走 blk + FatFs，不再有固件路径**。
 *
 * 到那时这个文件整个删掉。它不需要被改造成别的东西 —— 它本来就该消失。
 *
 * 数一下有多少：
 *     grep -c TND-TRANSITIONAL src/lib/file.c
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

    /* TND-TRANSITIONAL: 用 HandleProtocol 找 LoadedImage 拿 ESP 的设备句柄。
     * 去掉的条件：内核有自己加载映像的能力（TNX 加载器 + 自己的设备表）。 */
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

    /* TND-TRANSITIONAL: 用 HandleProtocol 找 SimpleFileSystem。
     * 去掉的条件：换成 blk 层 —— 驱动注册块设备，我们自己解析分区和文件系统。 */
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
/* **把 EFI_STATUS 翻成符号名。**
 * 只打 0x8000000000000002 这种数字，人会按错的含义去理解 ——
 * 我自己就把 2 当成 EFI_LOAD_ERROR，而它其实是 EFI_INVALID_PARAMETER。 */
static const char *efi_status_name(UINT64 s) {
    switch (s) {
    case 0:  return "EFI_SUCCESS";
    case 2:  return "EFI_INVALID_PARAMETER";
    case 3:  return "EFI_UNSUPPORTED";
    case 4:  return "EFI_BAD_BUFFER_SIZE";
    case 5:  return "EFI_BUFFER_TOO_SMALL";
    case 6:  return "EFI_NOT_READY";
    case 7:  return "EFI_DEVICE_ERROR";
    case 8:  return "EFI_WRITE_PROTECTED";
    case 9:  return "EFI_OUT_OF_RESOURCES";
    case 14: return "EFI_NOT_FOUND";
    case 15: return "EFI_ACCESS_DENIED";
    default: return "(other)";
    }
}

EFI_STATUS t_read_file(const char *name, char *buf, UINTN cap, UINTN *outLen) {
    EFI_FILE_PROTOCOL *f = 0;
    CHAR16 wname[260];
    EFI_STATUS s;
    UINTN total = 0;

    if (!gEnv.Root) return EFI_NOT_FOUND;
    t_ascii_to_u16(name, wname, 260);

    /* ================================================================
     * **每次打开都记一笔，带序号。**
     *
     * 真机上的症状：前 5 次打开都成功，从第 6 次开始全失败 —— 而且
     * 失败的顺序跟着 efidos.sys 里的顺序走（换个顺序就换个倒霉鬼）。
     * 这不是路径问题，也不是大小问题（失败的那个 10.5KB 比成功的 13.7KB 还小）。
     *
     * 要分清两种可能：
     *   句柄泄漏   -> 序号一直涨，但每次失败的都是"第 N 次之后"
     *   内存耗尽   -> 堆的空闲页在几次读取之间持续下降
     * 所以下面把序号、结果、堆的空闲页一起打出来 —— 一行就能分开。
     * ================================================================ */
    {
        static UINT32 seq;
        seq++;
        log_puts("[log] file#"); log_u64((UINT64)seq);
        log_puts(" open "); log_puts(name); log_puts("\r\n");
    }

    /* TND-TRANSITIONAL: 用固件的文件协议打开文件。
     * 去掉的条件：FatFs 接管 —— f_open 替代这一句。 */
    s = gEnv.Root->Open(gEnv.Root, &f, wname, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !f) {
        log_puts("[log] file# open FAILED: "); log_puts(name);
        log_puts("  status="); log_u64((UINT64)s);
        log_puts(" ("); log_puts(efi_status_name((UINT64)s)); log_puts(")");
        /* **把实际传进去的 UTF-16 名也打出来。**
         * EFI_INVALID_PARAMETER 的一个来源就是 FileName 本身非法，
         * 而那是我们自己的 t_ascii_to_u16 生成的 —— 不看一眼没法排除。 */
        log_puts("  wname=["); 
        { int k; for (k = 0; k < 12 && wname[k]; k++) log_putc((char)(wname[k] < 128 ? wname[k] : '?')); }
        log_puts("]\r\n");
        return s;
    }

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