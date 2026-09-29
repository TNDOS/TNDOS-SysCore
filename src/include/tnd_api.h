/* ============================================================================
 * TNDDOS -- TNX 程序能用的内核 API（v1）
 *
 * 为什么是"传进来的函数表"而不是 syscall：
 *   我们现在还活在 Boot Services 底下，**固件握着 IDT**，装不了自己的中断门，
 *   所以 SYSCALL / INT 两条路都走不通（详见 TNX 设计讨论）。
 *   于是 v1 由加载器把这张表**作为参数传给程序入口**。
 *
 *   源码级 API 不变：程序永远写 tnd_puts(...)。
 *   将来 ExitBootServices 之后，把表里的指针换成 syscall 桩即可，程序不用改。
 *
 * 零依赖：内核和 TNX 程序都包含本文件，所以只用朴素 C 类型。
 * ==========================================================================*/
#ifndef TND_API_H
#define TND_API_H

typedef unsigned int       tnd_u32;
typedef unsigned long long tnd_u64;
typedef unsigned long long tnd_size;

#define TND_API_VERSION 0x00010000u

/* ============================================================================
 * 调用约定 —— 这里是全项目最容易踩、也最难查的一个坑。
 *
 * TNX 程序用 -target x86_64-unknown-none 编译 => System V AMD64 ABI
 * TNDDOS 内核用 -target x86_64-pc-windows-msvc 编译 => Microsoft x64 ABI
 *
 * 两者**不一样**：SysV 第一个参数放 RDI，MS 第一个参数放 RCX。
 * 不钉死的话，程序把字符串放进 RDI，内核函数去 RCX 拿 —— 拿到的是垃圾，
 * 而且报出来的是 #UD（无效指令），看起来跟参数传递毫无关系。
 *
 * 决定：**TNX 的 ABI 就是 System V**（TNX 是从 ELF64 出来的，这是自然选择）。
 * 内核那侧用 sysv_abi 属性把 API 入口适配过去。
 * TNX 程序侧不需要任何修饰 —— SysV 本来就是它的默认。
 * ==========================================================================*/
#if defined(_MSC_VER)
#  define TND_ABI __attribute__((sysv_abi))
#else
#  define TND_ABI
#endif

typedef struct {
    /* --- 头 --- */
    tnd_u32 StructSize;      /* 整张表的大小，程序用它判断内核给了多少 */
    tnd_u32 Version;

    /* --- 控制台 --- */
    void (*TND_ABI puts)(const char *s);   /* 输出以 0 结尾的字符串 */
    void (*TND_ABI putc)(int c);           /* 输出一个字符 */
    void (*TND_ABI putu)(tnd_u64 v);       /* 输出无符号十进制 */
    void (*TND_ABI putx)(tnd_u64 v);       /* 输出 0x + 16 位十六进制 */

    /* --- 内核堆 --- */
    void *(*TND_ABI alloc)(tnd_size bytes);
    void  (*TND_ABI free)(void *p);

    /* --- 环境 --- */
    tnd_u64 (*TND_ABI ticks)(void);        /* 单调递增的毫秒计数，失败返回 0 */

} TND_API_TABLE;

#endif /* TND_API_H */
