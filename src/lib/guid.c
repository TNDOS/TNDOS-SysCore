/* ============================================================================
 * TNDDOS — 全局定义：GUID 表 与 全局运行环境 gEnv
 * 放在库里而不是各入口里，是因为 BOOTX64.EFI 和 kernel.efi 是两个独立的 PE，
 * 各自都要能从同一处拿到 GUID 与 gEnv。
 * ==========================================================================*/
#include "tnd.h"

const EFI_GUID gEfiLoadedImageProtocolGuid      = { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
const EFI_GUID gEfiSimpleFileSystemProtocolGuid = { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
const EFI_GUID gEfiSerialIoProtocolGuid         = { 0xBB25CF6F, 0xF1D4, 0x11D2, { 0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } };
/* 注意：这个 GUID 的 Data1 确实以 0 开头，和 SFS 那个不是一回事 */
const EFI_GUID gEfiFileInfoGuid                 = { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

TND_ENV gEnv;
