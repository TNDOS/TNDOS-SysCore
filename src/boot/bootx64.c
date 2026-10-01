/* ============================================================================
 * TNDDOS boot initializer -- BOOTX64.EFI
 *
 * Positioned like the PBR in classic DOS: it is not resident, it only brings the
 * system up and hands over.
 *   1) bring up console + serial log
 *   2) locate the ESP we were loaded from
 *   3) read \EFI\TNDOS\kernel.efi into memory
 *   4) LoadImage / StartImage the kernel
 *   5) when the kernel returns, unload it and exit (transient)
 *
 * All user-visible text is ASCII on purpose: UEFI firmware fonts are not
 * guaranteed to carry anything beyond ASCII, and a console you cannot read is
 * worse than no console. Localization is a later layer (font + codepage), not
 * something to hardcode into every message.
 * ==========================================================================*/
#include "tnd.h"

static void banner(void) {
    con_puts("\r\n");
    con_puts("  " TND_NAME " (" TND_ALIAS ")  Version " TND_VERSION "\r\n");
    con_puts("  UEFI x86-64 boot initializer   [stage 1 / PBR]\r\n");
    con_puts("\r\n");
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    /* 不用固定上限的静态缓冲：上限迟早被超过（踩过两次：16KB、64KB），
     * 而且症状永远是 LoadImage 回一个没头没脑的 Unsupported。
     * 大小是文件的属性，按它分配。 */
    void *kbuf = 0;
    UINTN klen = 0;
    void *image = 0;
    EFI_HANDLE kernelHandle = 0;
    EFI_STATUS s;

    /* --- set up the global environment --- */
    gEnv.ImageHandle = ImageHandle;
    gEnv.ST = SystemTable;
    gEnv.BS = SystemTable->BootServices;
    gEnv.RT = SystemTable->RuntimeServices;
    gEnv.Root = 0;
    gEnv.EspHandle = 0;

    log_init(SystemTable);

    log_puts("[log] === BOOTX64.EFI entry ===\r\n");
    log_kv_u64("SystemTable.FirmwareRevision", SystemTable->FirmwareRevision);
    log_puts("[log] SystemTable.BootServices    = "); log_hex((UINT64)gEnv.BS); log_puts("\r\n");
    log_puts("[log] SystemTable.RuntimeServices = "); log_hex((UINT64)gEnv.RT); log_puts("\r\n");
    log_kv("SerialIo", gEnv.Ser ? "OK" : "NOT FOUND");

    banner();

    /* --- locate the ESP --- */
    con_puts("[1/4] Locating EFI System Partition ...\r\n");
    s = t_open_root(ImageHandle);
    if (EFI_ERROR(s)) {
        con_puts("      FAILED: no file system. Cannot continue.\r\n");
        log_kv("boot", "t_open_root FAILED");
        return s;
    }
    con_puts("      OK\r\n");

    /* --- read the kernel --- */
    con_puts("[2/4] Reading " TND_KERNEL " ...\r\n");
    s = t_read_file_alloc(TND_KERNEL, &kbuf, &klen);
    if (EFI_ERROR(s) || klen == 0) {
        con_puts("      FAILED: kernel image not readable.\r\n");
        log_kv("boot", "read kernel FAILED");
        return s;
    }
    con_puts("      "); con_u64(klen); con_puts(" bytes\r\n");

    /* --- hand it to the firmware loader --- */
    con_puts("[3/4] LoadImage / StartImage the kernel ...\r\n");
    /* LoadImage 会在调用期间把源缓冲整个吃掉，所以可以直接把读进来的缓冲交给它，
     * 少一次 klen 大小的拷贝。 */
    image = kbuf;

    s = gEnv.BS->LoadImage(1 /*BootPolicy*/, ImageHandle, 0 /*DevicePath*/,
                           image, klen, &kernelHandle);
    if (EFI_ERROR(s) || !kernelHandle) {
        con_puts("      FAILED: LoadImage returned ");
        con_puts(t_status_str(s));
        con_puts("\r\n");
        log_kv("boot", "LoadImage FAILED");
        log_puts("[log] LoadImage status = "); log_hex((UINT64)s); log_puts("\r\n");
        return s;
    }
    log_puts("[log] LoadImage OK, kernel handle = "); log_hex((UINT64)kernelHandle); log_puts("\r\n");

    /* An image loaded from a memory buffer does NOT inherit a device handle
     * (measured behaviour), so hand our ESP handle over explicitly -- otherwise
     * the kernel's own HandleProtocol returns NULL and it cannot read config. */
    {
        EFI_LOADED_IMAGE_PROTOCOL *kli = 0;
        EFI_STATUS hs = gEnv.BS->HandleProtocol(kernelHandle, &gEfiLoadedImageProtocolGuid, (void **)&kli);
        if (!EFI_ERROR(hs) && kli) {
            kli->DeviceHandle = gEnv.EspHandle;
            log_puts("[log] handed ESP handle to kernel: "); log_hex((UINT64)gEnv.EspHandle); log_puts("\r\n");
        } else {
            log_puts("[log] !! cannot get kernel LoadedImage, ESP handle not passed\r\n");
        }
    }
    con_puts("      OK\r\n");

    /* --- hand over --- */
    con_puts("[4/4] Handing over to kernel.efi ...\r\n\r\n");
    log_puts("[log] --- handing off to kernel ---\r\n");
    s = gEnv.BS->StartImage(kernelHandle, 0, 0);
    log_puts("[log] --- kernel returned ---\r\n");

    con_puts("\r\n");
    con_puts("Kernel returned; initializer is wrapping up.\r\n");

    gEnv.BS->UnloadImage(kernelHandle);
    gEnv.BS->FreePool(image);

    con_puts(TND_NAME " boot initializer exiting (transient stage done).\r\n");
    log_kv("boot", "BOOTX64.EFI exit");
    return EFI_SUCCESS;
}
