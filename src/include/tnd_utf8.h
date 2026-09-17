/* ============================================================================
 * TNDDOS — UTF-8 -> UTF-16 转换（内核与驱动共用，无任何依赖）
 * 源码是 UTF-8，UEFI ConOut 只吃 UCS-2，不转码中文在屏幕上就是一串乱码。
 * ==========================================================================*/
#ifndef TND_UTF8_H
#define TND_UTF8_H

#include "efi.h"

void t_ascii_to_u16(const char *s, CHAR16 *d, UINTN max);
void t_utf8_to_u16(const char *s, CHAR16 *d, UINTN maxChars);

#endif /* TND_UTF8_H */
