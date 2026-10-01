/* ============================================================================
 * TNDDOS -- TNX 程序能用的内核 API（v2）
 *
 * v1 只有 puts/alloc/free/ticks —— 那个规模什么都写不了，连 MORE 都不行
 * （它要读文件，而表里没有文件 API）。v2 把 DOS 的骨架补上：
 *
 *   句柄模型（fd 0/1/2）   ->  重定向、管道、工具组合
 *   文件 API               ->  真能读写
 *   目录遍历               ->  TREE / XCOPY / DELTREE
 *   参数与环境             ->  工具有输入
 *
 * ---------------------------------------------------------------------------
 * 两个和"调用约定"同一级别的坑，写在这里免得再踩：
 *
 * ① ABI 是 System V，不是 MS ABI。
 *    内核用 sysv_abi 适配（见下面的 TND_ABI）。
 *
 * ② **这个头文件里一个 long 都不能有。**
 *    x86_64-unknown-none 是 SysV 模型，long = 8 字节；
 *    内核的 x86_64-pc-windows-msvc 目标，long = 4 字节。
 *    同一个结构体在两边尺寸不同 —— 比调用约定还阴，因为结构体偏移会整体错位。
 *    所以全部用定宽类型：tnd_u32 / tnd_u64 / tnd_i64。
 * ---------------------------------------------------------------------------
 *
 * 零依赖：内核和 TNX 程序都包含本文件，所以只用朴素 C 类型。
 * ==========================================================================*/
#ifndef TND_API_H
#define TND_API_H

typedef unsigned int       tnd_u32;
typedef unsigned short     tnd_u16;
typedef unsigned char      tnd_u8;
typedef unsigned long long tnd_u64;
typedef long long          tnd_i64;
typedef unsigned long long tnd_size;

#if defined(_MSC_VER)
#  define TND_ABI __attribute__((sysv_abi))
#else
#  define TND_ABI
#endif

#define TND_API_VERSION 0x00020300u

/* 光标。UEFI 只能显隐，**不能设形状** —— 所以 DOS 的"下划线/整块"
 * 没法照搬，固件给什么形状就是什么形状。
 * 想要整块只能自己用反白画（EDIT 的覆盖模式就是这么做的）。 */

/* ------------------------------------------------------------ 颜色
 * UEFI 的 EFI_TEXT_ATTR(fg, bg) 就是 fg | (bg << 4) —— 和 DOS 的 VGA
 * 属性字节**恰好一模一样**（连调色板的顺序都一样）。
 * 所以下面这些 DOS 常量可以原样用，不需要任何翻译层。 */
#define TND_BLACK         0
#define TND_BLUE          1
#define TND_GREEN         2
#define TND_CYAN          3
#define TND_RED           4
#define TND_MAGENTA       5
#define TND_BROWN         6
#define TND_LIGHTGRAY     7
#define TND_DARKGRAY      8
#define TND_LIGHTBLUE     9
#define TND_LIGHTGREEN   10
#define TND_LIGHTCYAN    11
#define TND_LIGHTRED     12
#define TND_LIGHTMAGENTA 13
#define TND_YELLOW       14
#define TND_WHITE        15

#define TND_ATTR(fg, bg) ((int)(fg) | ((int)(bg) << 4))

/* ------------------------------------------------------------ 屏幕与键盘
 * v2.0 只有控制台输出 —— 那写不了全屏程序。EDIT 一上手就暴露了：
 * 没有 cls/gotoxy，光标没法定位；读键也拿不到方向键（扫描码被丢掉了）。 */
#define TND_KEY(c)   ((int)((c) & 0xFFFF))
#define TND_SCAN(c)  ((int)(((c) >> 16) & 0xFFFF))

/* UEFI 的扫描码（EFI_INPUT_KEY.ScanCode），直接沿用 */
#define TND_S_UP      0x01
#define TND_S_DOWN    0x02
#define TND_S_RIGHT   0x03
#define TND_S_LEFT    0x04
#define TND_S_HOME    0x05
#define TND_S_END     0x06
#define TND_S_INSERT  0x07
#define TND_S_DELETE  0x08
#define TND_S_PGUP    0x09
#define TND_S_PGDN    0x0A
#define TND_S_ESC     0x17

/* ------------------------------------------------------------------ 句柄 */
#define TND_STDIN   0
#define TND_STDOUT  1
#define TND_STDERR  2

/* open 的 flags */
#define TND_O_RDONLY  0x0001
#define TND_O_WRONLY  0x0002
#define TND_O_RDWR    0x0003
#define TND_O_CREATE  0x0010
#define TND_O_TRUNC   0x0020

/* seek 的 whence */
#define TND_SEEK_SET  0
#define TND_SEEK_CUR  1
#define TND_SEEK_END  2

/* stat 的 Attr 位 */
#define TND_ATTR_DIR     0x0001
#define TND_ATTR_RDONLY  0x0002

#define TND_NAME_MAX     64
#define TND_PATH_MAX     256

typedef struct {
    tnd_u32 Size;
    tnd_u32 Attr;
    tnd_u32 Year;
    tnd_u32 Month;
    tnd_u32 Day;
    tnd_u32 Hour;
    tnd_u32 Minute;
    tnd_u32 Reserved;
} TND_STAT;

typedef struct {
    char    Name[TND_NAME_MAX];
    tnd_u32 Size;
    tnd_u32 Attr;
    tnd_u32 Reserved;
} TND_FIND;

/* ------------------------------------------------------------------ API 表 */
typedef struct {
    tnd_u32 StructSize;
    tnd_u32 Version;

    /* --- 控制台快捷方式（等价于写 fd 1） --- */
    void (*TND_ABI puts)(const char *s);
    void (*TND_ABI putc)(int c);
    void (*TND_ABI putu)(tnd_u64 v);
    void (*TND_ABI putx)(tnd_u64 v);

    /* --- 程序环境 --- */
    int         (*TND_ABI argc)(void);
    const char *(*TND_ABI argv)(int i);
    const char *(*TND_ABI env)(const char *name);

    /* --- 句柄 I/O --- */
    int    (*TND_ABI open)(const char *path, int flags);
    int    (*TND_ABI close)(int fd);
    tnd_i64 (*TND_ABI read)(int fd, void *buf, tnd_i64 count);
    tnd_i64 (*TND_ABI write)(int fd, const void *buf, tnd_i64 count);
    tnd_i64 (*TND_ABI seek)(int fd, tnd_i64 offset, int whence);

    /* --- 文件系统 --- */
    int (*TND_ABI unlink)(const char *path);
    int (*TND_ABI mkdir)(const char *path);
    int (*TND_ABI rmdir)(const char *path);
    int (*TND_ABI rename)(const char *from, const char *to);
    int (*TND_ABI stat)(const char *path, TND_STAT *st);

    /* --- 目录遍历（DOS 式 findfirst/findnext） --- */
    int (*TND_ABI findfirst)(const char *pattern, TND_FIND *out);
    int (*TND_ABI findnext)(int fh, TND_FIND *out);
    int (*TND_ABI findclose)(int fh);

    /* --- 内存 / 时间 --- */
    void    *(*TND_ABI alloc)(tnd_size n);
    void     (*TND_ABI free)(void *p);
    tnd_u64  (*TND_ABI ticks)(void);

    /* --- 屏幕与键盘（v2.1 追加）
     * 一律加在**表尾** —— StructSize 让老程序能安全地不认识新字段。 */
    void (*TND_ABI cls)(void);
    void (*TND_ABI gotoxy)(int x, int y);
    int  (*TND_ABI getkey)(void);   /* 阻塞。返回 (扫描码 << 16) | UnicodeChar */
    int  (*TND_ABI cols)(void);
    int  (*TND_ABI rows)(void);

    /* --- 颜色（v2.2 追加）--- */
    void (*TND_ABI setattr)(int attr);   /* TND_ATTR(fg, bg) */
    int  (*TND_ABI getattr)(void);

    /* --- 光标（v2.3 追加）--- */
    void (*TND_ABI cursor)(int visible);

} TND_API_TABLE;

#endif /* TND_API_H */
