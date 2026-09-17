/* ============================================================================
 * TNDDOS driver: VGA / console
 *
 * Loaded by   DEVICE=\EFI\TNDOS\DRIVERS\VGA.EFI   in efidos.sys.
 * For now it does the starting point of what a driver should do: ask the
 * firmware what the display device can do, and actually flip a text attribute
 * once to prove the write path works. Once the in-house framebuffer console
 * exists this will take over the GOP framebuffer instead.
 * ==========================================================================*/
#include "drv.h"

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *st) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *co;
    UINTN cols = 0, rows = 0;
    (void)ImageHandle;

    dputs(st, "[VGA.EFI] TNDDOS console driver\r\n");
    co = st->ConOut;
    if (!co) { dputs(st, "          !! no ConOut, unloading\r\n"); return 0; }

    if (co->QueryMode && !EFI_ERROR(co->QueryMode(co, 0, &cols, &rows))) {
        dputs(st, "          text mode size: ");
        dnum(st, (UINT64)cols); dputs(st, " cols x "); dnum(st, (UINT64)rows); dputs(st, " rows\r\n");
    }
    if (co->Mode) {
        dputs(st, "          mode current/max: ");
        dnum(st, (UINT64)(UINT32)co->Mode->Mode); dputs(st, " / ");
        dnum(st, (UINT64)(UINT32)co->Mode->MaxMode);
        dputs(st, "   attribute ");
        dhex(st, (UINT64)(UINT32)co->Mode->Attribute & 0xFF);
        dputs(st, "\r\n");
    }

    /* Actually touch the device: change attribute -> write -> restore.
     * That is what makes this a driver and not a printf. */
    if (co->SetAttribute) {
        co->SetAttribute(co, 0x0F);
        dputs(st, "          [attribute test] this line is 0x0F bright white on black\r\n");
        co->SetAttribute(co, 0x07);
        dputs(st, "          [attribute test] restored to 0x07\r\n");
    }

    dputs(st, "[VGA.EFI] init done -> EFI_SUCCESS\r\n");
    return 0;
}
