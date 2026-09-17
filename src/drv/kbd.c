/* ============================================================================
 * TNDDOS driver: keyboard / input
 *
 * Loaded by   DEVICE=\EFI\TNDOS\DRIVERS\KBD.EFI   in config.sys.
 * Again: the starting point of what a driver should do -- probe the input
 * device and flush its buffer. Once the in-house IDT plus PS/2 or USB HID
 * driver exists this becomes interrupt driven.
 * ==========================================================================*/
#include "drv.h"

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *st) {
    (void)ImageHandle;

    dputs(st, "[KBD.EFI] TNDDOS keyboard driver\r\n");

    dputs(st, "          ConsoleInHandle: ");
    dhex(st, (UINT64)st->ConsoleInHandle);
    dputs(st, "\r\n");

    if (!st->ConIn) { dputs(st, "          !! no ConIn, unloading\r\n"); return 0; }

    dputs(st, "          WaitForKey event: ");
    dputs(st, st->ConIn->WaitForKey ? "valid\r\n" : "invalid (poll only)\r\n");

    /* Actually touch the device: drop any keys left over from boot */
    if (st->ConIn->Reset) {
        EFI_STATUS s = st->ConIn->Reset(st->ConIn, 0 /* FALSE */);
        dputs(st, "          flush input buffer: ");
        dputs(st, EFI_ERROR(s) ? "failed\r\n" : "done\r\n");
    }

    dputs(st, "[KBD.EFI] init done -> EFI_SUCCESS\r\n");
    return 0;
}
