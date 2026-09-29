/* ============================================================================
 * TNDDOS — 最小 UEFI 定义
 * 按 UEFI 2.10 规范手写。所有结构体字段顺序严格照规范，中间字段一个都不能删，
 * 因为函数指针的"偏移"就是 ABI 本体。删一个字段 = 整个结构体错位。
 * ==========================================================================*/
#ifndef TND_EFI_H
#define TND_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef signed char        INT8;
typedef short              INT16;
typedef int                INT32;
typedef long long          INT64;
typedef unsigned char      BOOLEAN;
typedef unsigned long long UINTN;
typedef long long          INTN;
typedef void               VOID;
typedef UINT16             CHAR16;
typedef char               CHAR8;

typedef UINTN  EFI_STATUS;
typedef VOID  *EFI_HANDLE;
typedef VOID  *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

/* x86-64 上所有调用约定统一为 MS ABI，EFIAPI 是空宏 */
#define EFIAPI

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR       0x8000000000000001ULL
#define EFI_INVALID_PARAMETER 0x8000000000000002ULL
#define EFI_UNSUPPORTED      0x8000000000000003ULL
#define EFI_BAD_BUFFER_SIZE  0x8000000000000004ULL
#define EFI_OUT_OF_RESOURCES 0x8000000000000009ULL
#define EFI_ERROR(s)   (((INTN)(s)) < 0)
#define EFI_NOT_READY  0x8000000000000006ULL
#define EFI_NOT_FOUND  0x800000000000000EULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_DEVICE_ERROR     0x8000000000000007ULL
#define EFI_WRITE_PROTECTED  0x8000000000000009ULL
#define EFI_NOT_FOUND        0x800000000000000EULL
#define EFI_ACCESS_DENIED    0x800000000000000FULL
#define EFI_NO_MEDIA         0x8000000000000010ULL

typedef struct { UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8]; } EFI_GUID;

struct EFI_SYSTEM_TABLE;
struct EFI_BOOT_SERVICES;
struct EFI_RUNTIME_SERVICES;

/* ---------------------------------------------------------------- 基础表头 */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;                     /* 恰好 24 字节 —— 这是最容易写错的地方 */

/* -------------------------------------------------------------- 控制台输出 */
typedef struct {
    INT32 MaxMode;
    INT32 Mode;
    INT32 Attribute;
    INT32 CursorColumn;
    INT32 CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);                      /*  0 */
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);              /*  8 */
    EFI_STATUS (*TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);                /* 16 */
    EFI_STATUS (*QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN *, UINTN *);  /* 24 */
    EFI_STATUS (*SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);                      /* 32 */
    EFI_STATUS (*SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);                 /* 40 */
    EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);                         /* 48 */
    EFI_STATUS (*SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN);     /* 56 */
    EFI_STATUS (*EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);               /* 64 */
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;                                                    /* 72 */
};

/* -------------------------------------------------------------- 键盘输入 */
typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);                        /*  0 */
    EFI_STATUS (*ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);        /*  8 */
    EFI_EVENT  WaitForKey;                                                                 /* 16 */
};

/* ------------------------------------------------------------ 串口（调试） */
typedef struct EFI_SERIAL_IO_PROTOCOL EFI_SERIAL_IO_PROTOCOL;
struct EFI_SERIAL_IO_PROTOCOL {
    UINT32 Revision; UINT32 _pad;
    EFI_STATUS (*Reset)(EFI_SERIAL_IO_PROTOCOL *);                                         /*  8 */
    EFI_STATUS (*SetAttributes)(EFI_SERIAL_IO_PROTOCOL *, UINT64, UINT64, UINT64, UINT8);   /* 16 */
    EFI_STATUS (*SetControlBits)(EFI_SERIAL_IO_PROTOCOL *, UINT32);                        /* 24 */
    EFI_STATUS (*GetControlBits)(EFI_SERIAL_IO_PROTOCOL *, UINT32 *);                      /* 32 */
    EFI_STATUS (*Write)(EFI_SERIAL_IO_PROTOCOL *, UINTN *, VOID *);                        /* 40 */
    EFI_STATUS (*Read)(EFI_SERIAL_IO_PROTOCOL *, UINTN *, VOID *);                         /* 48 */
    VOID *Mode;                                                                            /* 56 */
};

/* -------------------------------------------------------------- 内存 */
typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData,
    EfiBootServicesCode, EfiBootServicesData, EfiRuntimeServicesCode,
    EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace, EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;                 /* 48 字节 */

/* -------------------------------------------------------------- 文件 */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;                                                                        /*  0 */
    EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);/*  8 */
    EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);                                               /* 16 */
    EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *);                                              /* 24 */
    EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);                               /* 32 */
    EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);                              /* 40 */
    EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *, UINT64 *);                               /* 48 */
    EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *, UINT64);                                 /* 56 */
    EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *, const EFI_GUID *, UINTN *, VOID *);          /* 64 */
    EFI_STATUS (*SetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, VOID *);                  /* 72 */
    EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);                                               /* 80 */
    EFI_STATUS (*OpenEx)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64, VOID *); /* 88 */
    EFI_STATUS (*ReadEx)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);                             /* 96 */
    EFI_STATUS (*WriteEx)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);                            /*104 */
    EFI_STATUS (*FlushEx)(EFI_FILE_PROTOCOL *);                                             /*112 */
};

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct {
    UINT16 Year;
    UINT8  Month, Day, Hour, Minute, Second, Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight, Pad2;
} EFI_TIME;                              /* 恰好 16 字节 */

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[];                   /* 柔性数组，头部固定 80 字节 */
} EFI_FILE_INFO;

#define EFI_FILE_DIRECTORY 0x0000000000000010ULL

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;                                                                        /*  0 */
    EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);       /*  8 */
};

/* ------------------------------------------------------- 已加载映像协议 */
typedef struct {
    UINT32 Revision;                    /*  0 (+4 填充) */
    EFI_HANDLE ParentHandle;            /*  8 —— 容易漏！漏了就全错位 */
    struct EFI_SYSTEM_TABLE *SystemTable;/* 16 —— 可用来做运行期自检 */
    EFI_HANDLE DeviceHandle;            /* 24 —— 从这里才能拿到 SimpleFileSystem */
    VOID *FilePath;                     /* 32 */
    VOID *Reserved;                     /* 40 */
    UINT32 LoadOptionsSize;             /* 48 (+4 填充) */
    VOID *LoadOptions;                  /* 56 */
    VOID *ImageBase;                    /* 64 */
    UINT64 ImageSize;                   /* 72 */
    UINT32 ImageCodeType;               /* 80 */
    UINT32 ImageDataType;               /* 84 */
    VOID *Unload;                       /* 88 */
} EFI_LOADED_IMAGE_PROTOCOL;

/* -------------------------------------------------------- Boot Services */
typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(UINT32, UINTN, VOID **);
typedef EFI_STATUS (*EFI_FREE_POOL)(VOID *);
typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(UINT32, UINT32, UINTN, EFI_PHYSICAL_ADDRESS *);
typedef EFI_STATUS (*EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS, UINTN);

#define AllocateAnyPages  0
#define AllocateMaxAddress 1
#define AllocateAddress   2

#define EFI_PAGE_SIZE 4096ULL
#define EFI_SIZE_TO_PAGES(n) (((n) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(EFI_HANDLE, const EFI_GUID *, VOID **);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(const EFI_GUID *, VOID *, VOID **);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
typedef EFI_STATUS (*EFI_LOAD_IMAGE)(BOOLEAN, EFI_HANDLE, VOID *, VOID *, UINTN, EFI_HANDLE *);
typedef EFI_STATUS (*EFI_START_IMAGE)(EFI_HANDLE, UINTN *, CHAR16 **);
typedef EFI_STATUS (*EFI_EXIT)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
typedef EFI_STATUS (*EFI_STALL)(UINTN);
typedef EFI_STATUS (*EFI_UNLOAD_IMAGE)(EFI_HANDLE);

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;                                    /*   0..24 */
    VOID *RaiseTPL;                                          /*  24 */
    VOID *RestoreTPL;                                        /*  32 */
    EFI_ALLOCATE_PAGES AllocatePages;                        /*  40 */
    EFI_FREE_PAGES     FreePages;                            /*  48 */
    EFI_GET_MEMORY_MAP GetMemoryMap;                         /*  56 */
    EFI_ALLOCATE_POOL  AllocatePool;                         /*  64 */
    EFI_FREE_POOL      FreePool;                             /*  72 */
    VOID *CreateEvent;                                       /*  80 */
    VOID *SetTimer;                                          /*  88 */
    VOID *WaitForEvent;                                      /*  96 */
    VOID *SignalEvent;                                       /* 104 */
    VOID *CloseEvent;                                        /* 112 */
    VOID *CheckEvent;                                        /* 120 */
    VOID *InstallProtocolInterface;                          /* 128 */
    VOID *ReinstallProtocolInterface;                        /* 136 */
    VOID *UninstallProtocolInterface;                        /* 144 */
    EFI_HANDLE_PROTOCOL HandleProtocol;                      /* 152 */
    VOID *Reserved;                                          /* 160 */
    VOID *RegisterProtocolNotify;                            /* 168 */
    VOID *LocateHandle;                                      /* 176 */
    VOID *LocateDevicePath;                                  /* 184 */
    VOID *InstallConfigurationTable;                         /* 192 */
    EFI_LOAD_IMAGE  LoadImage;                               /* 200 */
    EFI_START_IMAGE StartImage;                              /* 208 */
    EFI_EXIT        Exit;                                    /* 216 */
    EFI_UNLOAD_IMAGE UnloadImage;                            /* 224 */
    VOID *ExitBootServices;                                  /* 232 */
    VOID *GetNextMonotonicCount;                             /* 240 */
    EFI_STALL Stall;                                         /* 248 */
    VOID *SetWatchdogTimer;                                  /* 256 */
    VOID *ConnectController;                                 /* 264 */
    VOID *DisconnectController;                              /* 272 */
    VOID *OpenProtocol;                                      /* 280 */
    VOID *CloseProtocol;                                     /* 288 */
    VOID *OpenProtocolInformation;                           /* 296 */
    VOID *ProtocolsPerHandle;                                /* 304 */
    VOID *LocateHandleBuffer;                                /* 312 */
    EFI_LOCATE_PROTOCOL LocateProtocol;                      /* 320 */
} EFI_BOOT_SERVICES;

/* ----------------------------------------------------- Runtime Services */
typedef EFI_STATUS (*EFI_RESET_SYSTEM)(UINT32, EFI_STATUS, UINTN, VOID *);
typedef struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER Hdr;                                    /*   0..24 */
    VOID *GetTime;                                           /*  24 */
    VOID *SetTime;                                           /*  32 */
    VOID *GetWakeupTime;                                     /*  40 */
    VOID *SetWakeupTime;                                     /*  48 */
    VOID *SetVirtualAddressMap;                              /*  56 */
    VOID *ConvertPointer;                                    /*  64 */
    VOID *GetVariable;                                       /*  72 */
    VOID *GetNextVariableName;                               /*  80 */
    VOID *SetVariable;                                       /*  88 */
    VOID *GetNextHighMonotonicCount;                         /*  96 */
    EFI_RESET_SYSTEM ResetSystem;                            /* 104 */
} EFI_RUNTIME_SERVICES;

#define EfiResetCold     0
#define EfiResetWarm     1
#define EfiResetShutdown 2

/* --------------------------------------------------------- System Table */
typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;                                    /*   0..24 */
    CHAR16 *FirmwareVendor;                                  /*  24 */
    UINT32  FirmwareRevision;                                /*  32 (+4 填充) */
    EFI_HANDLE ConsoleInHandle;                              /*  40 */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;                  /*  48 */
    EFI_HANDLE ConsoleOutHandle;                             /*  56 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;                 /*  64 */
    EFI_HANDLE StandardErrorHandle;                          /*  72 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;                 /*  80 */
    EFI_RUNTIME_SERVICES *RuntimeServices;                   /*  88 */
    EFI_BOOT_SERVICES    *BootServices;                      /*  96 */
    UINTN NumberOfTableEntries;                              /* 104 */
    VOID *ConfigurationTable;                                /* 112 */
} EFI_SYSTEM_TABLE;

/* --------------------------------------------------------------- GUIDs */
extern const EFI_GUID gEfiLoadedImageProtocolGuid;
extern const EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern const EFI_GUID gEfiSerialIoProtocolGuid;
extern const EFI_GUID gEfiFileInfoGuid;

#endif /* TND_EFI_H */
