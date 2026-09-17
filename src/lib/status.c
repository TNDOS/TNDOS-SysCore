/* ============================================================================
 * TNDDOS -- EFI_STATUS to string
 * Shared by the boot initializer, the kernel and anything else that has to
 * report a firmware error. No dependencies, so every image can link it.
 * ==========================================================================*/
#include "tnd.h"

const char *t_status_str(EFI_STATUS s) {
    switch ((UINT64)s) {
        case 0:                     return "success";
        case 0x8000000000000001ULL: return "EFI_LOAD_ERROR";
        case 0x8000000000000002ULL: return "EFI_INVALID_PARAMETER";
        case 0x8000000000000003ULL: return "EFI_UNSUPPORTED";
        case 0x8000000000000004ULL: return "EFI_BAD_BUFFER_SIZE";
        case 0x8000000000000005ULL: return "EFI_BUFFER_TOO_SMALL";
        case 0x8000000000000006ULL: return "EFI_NOT_READY";
        case 0x8000000000000007ULL: return "EFI_DEVICE_ERROR";
        case 0x8000000000000008ULL: return "EFI_WRITE_PROTECTED";
        case 0x8000000000000009ULL: return "EFI_OUT_OF_RESOURCES";
        case 0x800000000000000AULL: return "EFI_VOLUME_CORRUPTED";
        case 0x800000000000000BULL: return "EFI_VOLUME_FULL";
        case 0x800000000000000CULL: return "EFI_NO_MEDIA";
        case 0x800000000000000DULL: return "EFI_MEDIA_CHANGED";
        case 0x800000000000000EULL: return "EFI_NOT_FOUND";
        case 0x800000000000000FULL: return "EFI_ACCESS_DENIED";
        case 0x8000000000000010ULL: return "EFI_NO_RESPONSE";
        case 0x8000000000000011ULL: return "EFI_NO_MAPPING";
        case 0x8000000000000012ULL: return "EFI_TIMEOUT";
        case 0x8000000000000013ULL: return "EFI_NOT_STARTED";
        case 0x8000000000000014ULL: return "EFI_ALREADY_STARTED";
        case 0x8000000000000015ULL: return "EFI_ABORTED";
        case 0x8000000000000016ULL: return "EFI_ICMP_ERROR";
        case 0x8000000000000017ULL: return "EFI_TFTP_ERROR";
        case 0x8000000000000018ULL: return "EFI_PROTOCOL_ERROR";
        case 0x8000000000000019ULL: return "EFI_INCOMPATIBLE_VERSION";
        case 0x800000000000001AULL: return "EFI_SECURITY_VIOLATION";
        case 0x800000000000001BULL: return "EFI_CRC_ERROR";
        case 0x800000000000001CULL: return "EFI_END_OF_MEDIA";
        default:                    return "unknown EFI status";
    }
}
