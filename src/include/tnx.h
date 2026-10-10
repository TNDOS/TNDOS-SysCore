/* ============================================================================
 * TNDDOS -- TNX 可执行格式 v1
 *
 * 一句话：TNX 不解决"现代可执行文件格式的一切问题"，它只负责把一个**已经
 * 链接好的** TNDDOS 程序，用最低成本装进 TNDDOS 的地址空间。
 *
 * 核心取舍（每条都对应一段加载器代码的消失）：
 *   固定加载地址   -> 没有重定位表
 *   无动态导入     -> 没有符号解析；系统调用走 API 表（参数传入，见 tnd_api.h）
 *   4 个段 tag     -> 段表分支 30 行写完
 *   文件内连续排布 -> 不用处理的碎片
 *
 * 非目标（Anti-goals）—— 以下东西 TNX 永远不做，做了就说明在重造 PE：
 *   动态链接 / 导入表 / 导出表
 *   TLS
 *   异常元数据（.eh_frame / unwind）
 *   调试信息 / 符号表
 *   重定位
 *   资源目录（不是"资源"，是"目录"）
 *   ABI 自动发现
 *
 * 扩展规则：任何新字段要进 header，必须先回答"加载器拿它干什么"。
 * 答不上来的一律进 RSRC —— 加载器不解析 RSRC，所以塞什么都不会污染格式。
 *
 * 本文件同时被内核和宿主机工具（tools/tnxpack.ps1）使用，必须保持零依赖。
 * ==========================================================================*/
#ifndef TND_TNX_H
#define TND_TNX_H

/* ---------------------------------------------------------------- 基础类型 */
typedef unsigned char      tnx_u8;
typedef unsigned short     tnx_u16;
typedef unsigned int       tnx_u32;
typedef unsigned long long tnx_u64;

/* ------------------------------------------------------------------ 魔数 */
/* 磁盘上就是 54 4E 58 1A 00 00 00 00，小端读出来是这个数。
 * 不能和 PE（4D 5A）或 ELF（7F 45 4C 46）撞车。 */
#define TNX_MAGIC   0x000000001A584E54ULL

#define TNX_VERSION_10  0x00010000u      /* v1.0 —— 没有 Machine 字段 */
#define TNX_VERSION_11  0x00010001u      /* v1.1 —— 头 56 字节，多一个 Machine */
#define TNX_VERSION     TNX_VERSION_11   /* 打包器写这个；加载器两个都收 */

#define TNX_HEADER_SIZE     48u          /* v1.0 的头长 */
#define TNX_HEADER_SIZE_V11 56u          /* v1.1 的头长（48 + Machine） */

/* --------------------------------------------------- 架构标识（v1.1 新增）
 * TNX 标的是 **CPU，不是固件**。约定表见 TNX-SPEC.md 3.3。
 *
 * 0x00 是**无效值** —— v1.0 的兼容由 Version 决定，不由值决定。
 *   一个没有 Machine 字段的 v1.0 文件，语义上就是 **AMD64**（TNX 至今只
 *   在 AMD64 上产生过二进制）。写成"按本机处理"的话，别的架构会把 x86
 *   机器码按自己的架构解 —— 直接崩。见规范 3.3。
 * 0x80-0xFF 是实验段，官方永不占用，加载器**一律拒绝**。 */
#define TNX_MACHINE_INVALID     0x00u
#define TNX_MACHINE_AMD64       0x01u
#define TNX_MACHINE_IA32        0x02u
#define TNX_MACHINE_ARMV7       0x03u
#define TNX_MACHINE_AARCH64     0x04u
#define TNX_MACHINE_RISCV32     0x05u
#define TNX_MACHINE_RISCV64     0x06u
#define TNX_MACHINE_LOONGARCH32 0x07u
#define TNX_MACHINE_LOONGARCH64 0x08u
#define TNX_MACHINE_IA64        0x09u
/* **官方不维护，仅作预留。** EFI 就是从这里起家的 —— 但那条线是 EFI 1.10 / EDK I，
 * 不是 UEFI 2.x，**不是加个 target 就能编的移植，是另一份固件绑定**。
 * 占号是为了将来真有人要移植时不用现拍板。**预留的是位置，不是承诺。** */

/* ------------------------------------------------------------------ Flags */
#define TNX_FLAG_CONSOLE   0x00000001u   /* 程序需要控制台 */
#define TNX_KNOWN_FLAGS    0x00000001u   /* 认得的位；出现别的位一律拒绝，不猜 */

/* --------------------------------------------------------------- 段 tag */
#define TNX_TAG_CODE 0x45444F43u   /* 'CODE' */
#define TNX_TAG_DATA 0x41544144u   /* 'DATA' */
#define TNX_TAG_RODT 0x54444F52u   /* 'RODT' */
#define TNX_TAG_RSRC 0x43525352u   /* 'RSRC' -- 加载器只搬运，绝不解析 */
#define TNX_TAG_SIGN 0x4E474953u   /* 'SIGN' -- 加载器只记录，绝不验证 */

/* ------------------------------------------------------------- 段 Flags */
#define TNX_SEC_R     0x1u
#define TNX_SEC_W     0x2u
#define TNX_SEC_X     0x4u
#define TNX_SEC_ZERO  0x8u   /* 纯 BSS：FileSize 必须为 0 */

/* ------------------------------------------------------------------ 头部 */
typedef struct {
    tnx_u64 Magic;         /*  0 */
    tnx_u32 Version;       /*  8 */
    tnx_u32 HeaderSize;    /* 12  扩展靠它：新版本加字段就把它变大 */
    tnx_u32 Flags;         /* 16 */
    tnx_u32 SectionCount;  /* 20 */
    tnx_u64 EntryRVA;      /* 24  相对 ImageBase */
    tnx_u64 ImageBase;     /* 32  v1 固定，见 tnd.h 的 TNX_IMAGE_BASE */
    tnx_u64 ImageSize;     /* 40  内存映像总大小（含 BSS） */
    /* ---- v1.1 追加。v1.0 的文件在 48 处就结束了 ---- */
    tnx_u64 Machine;       /* 48  架构标识（u64 而不是 u8：u8 会在 SectionCount
                            *      后面留 3 个字节空洞，而空洞只能叫 Reserved
                            *      —— 见规范 3.3 的说明） */
} TNX_HEADER;              /* v1.0: 48 / v1.1: 56 字节 */

typedef struct {
    tnx_u32 Tag;           /*  0 */
    tnx_u32 Flags;         /*  4 */
    tnx_u64 FileOffset;    /*  8 */
    tnx_u64 RVA;           /* 16 */
    tnx_u64 FileSize;      /* 24 */
    tnx_u64 MemSize;       /* 32  >= FileSize，多出来的部分加载器置零 */
} TNX_SECTION;             /* 恰好 40 字节 */

#define TNX_SECTION_ALIGN 16u

/* ------------------------------------------------------------------ 工具 */
#define TNX_TAG4(a,b,c,d) ((tnx_u32)(a) | ((tnx_u32)(b) << 8) | ((tnx_u32)(c) << 16) | ((tnx_u32)(d) << 24))

#endif /* TND_TNX_H */
