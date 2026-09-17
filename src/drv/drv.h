/* ============================================================================
 * TNDDOS 驱动 SDK（最小版）
 *
 * 一个 TNDDOS 驱动就是一个普通的 UEFI 映像：入口 efi_main(ImageHandle, ST)。
 * 内核用 DEVICE= 指令把它 LoadImage + StartImage 起来 —— 等价于 UEFI Shell 里的
 *     load fs0:\EFI\TNDOS\DRIVERS\XXX.EFI
 *
 * 驱动不要依赖内核的任何东西，只依赖这里。
 * ==========================================================================*/
#ifndef TND_DRV_H
#define TND_DRV_H

#include "efi.h"
#include "tnd_utf8.h"

#define DRV_NAME "TNDDOS Driver"

void dputs(EFI_SYSTEM_TABLE *st, const char *s);
void dputc(EFI_SYSTEM_TABLE *st, char c);
void dnum(EFI_SYSTEM_TABLE *st, UINT64 v);
void dhex(EFI_SYSTEM_TABLE *st, UINT64 v);

#endif /* TND_DRV_H */
