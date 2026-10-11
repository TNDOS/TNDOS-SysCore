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

#define TND_API_VERSION 0x00020500u

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

/* --------------------------------------------------- 批量原语用的类型（v2.4）
 * 对照 Win32 控制台的 CHAR_INFO：一个格子 = 字符 + 属性。 */
typedef struct {
    tnd_u32 Ch;      /* 字符。现在只用低 8 位（CP437）；宽字符是下一步 */
    tnd_u32 Attr;    /* TND_ATTR(fg, bg) */
} TND_CELL;

/* 屏幕状态一次拿全 —— 对照 GetConsoleScreenBufferInfo。
 * 分开问 cols/rows/getattr/getxy 要四次调用，每次重画都问一遍太浪费。 */
typedef struct {
    tnd_u32 StructSize;
    int     X, Y;            /* 光标所在格 */
    int     Cols, Rows;      /* 网格 */
    int     Attr;            /* 当前属性 */
    int     CursorVisible;
    tnd_u32 Reserved;
} TND_SCREEN;

/* ------------------------------------------------------------------ 驱动（v2.5）
 *
 * 驱动和程序是**同一件事**：都是 TNX 程序，都从 tnx_main 进。
 * 区别只在驱动会调 drv_register 登记自己，然后**留着不走**。
 *
 * 为什么不做成另一种文件类型：那要动 TNX 格式。
 * **能用入口说清的事，不动格式** —— 和包管理器那次是同一个教训。
 *
 * 驱动住在 0x2000000 那个窗口（见 tnd.h 的 TNX_DRIVER_BASE），**常驻**：
 * 程序加载时只清自己的窗口，碰不到它。这就是多窗口的全部意义。
 */

/* 驱动类型 */
#define TND_DRV_DISPLAY 1
#define TND_DRV_INPUT   2
#define TND_DRV_BLOCK   3
#define TND_DRV_BUS     4    /* 总线，产出设备交给别的驱动 */

/* 设备描述。**现在是扁平的，没有设备树** —— 总线驱动的产物就是这些东西。 */
typedef struct {
    tnd_u32 StructSize;
    tnd_u32 Type;
    tnd_u32 Unit;          /* 同类里第几个，从 0 起 */
    tnd_u32 Flags;
    tnd_u64 Base;          /* MMIO 基址，或端口号（看 Type） */
    tnd_u64 Size;
    tnd_u32 Irq;           /* **现在恒为 0** —— 没有 IDT，位置先留着 */
    tnd_u32 Vendor;
    tnd_u32 Device;
    tnd_u32 Class;         /* PCI 类代码（高 24 位有意义），非 PCI 设备为 0 */
    char    Name[16];      /* 给人看：VGA / PS2 / AHCI */
} TND_DEVICE;

/* 驱动描述 —— 驱动填好交给 drv_register。
 *
 * probe / attach / detach 的形状借自 EDK II 的 DriverBinding，但那套形状是对的，
 * 不是巧合。差别：我们暂时不做 RemainingDevicePath（没有设备树）。
 *
 * **状态不要放全局变量** —— 一个驱动可能绑多个设备（两块显卡、两个串口）。
 * attach 返回的那个指针是**绑定实例**，每个设备一份，状态挂在它上面。 */
typedef struct TND_DRIVER_DESC {
    tnd_u32      StructSize;
    tnd_u32      Type;
    const char  *Name;
    int  (*TND_ABI probe)(const TND_DEVICE *dev);    /* 这个设备你要不要 */
    void *(*TND_ABI attach)(const TND_DEVICE *dev);  /* 要了就初始化，返回实例 */
    void (*TND_ABI detach)(void *ctx);               /* 放开。暂时没人调 */
} TND_DRIVER_DESC;

/* ------------------------------------------------------------------ 输入后端（v2.5）
 *
 * 和 TND_CONSOLE 一个路数：**上层只问「当前后端」**，不问固件。
 * 现在只有固件的 ConIn；PS/2 驱动注册一个之后就优先用它 ——
 * 那就是「键盘不再依赖固件」的那一步。
 *
 * **驱动返回之后这个后端依然有效** —— 它住在驱动窗口，加载程序时不清零。
 * 这是多窗口那个改动的直接收益。 */
typedef struct {
    tnd_u32     StructSize;
    const char *Name;
    /* 取一个键。0 = 现在没有；非 0 = (扫描码 << 16) | UnicodeChar ——
     * 和固件 EFI_INPUT_KEY 的拼法一致，上层不用分两种情况。
     * 拿不到字形时 UnicodeChar 可以是 0（方向键）。 */
    int (*TND_ABI GetKey)(void);
} TND_INPUT;

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

    /* --- 批量原语（v2.4 追加）
     * 对照 Win32 控制台：WriteConsoleOutput / FillConsoleOutputCharacter /
     * ScrollConsoleScreenBuffer / GetConsoleScreenBufferInfo。
     * microsoft/edit 用的就是那一套（windows-sys 的 Win32_System_Console）。
     *
     * 为什么必须有：EDIT 这类程序每次重画几百个格子。逐格 gotoxy + putc
     * 光是调用开销就吃掉一切 —— 这不是"高级功能"，是性能底线。
     *
     * 坐标一律相对屏幕左上角，越界部分自动裁掉，不报错。 */
    void (*TND_ABI writecells)(int x, int y, int w, int h, const TND_CELL *cells, int stride);
    void (*TND_ABI fill)      (int x, int y, int w, int h, int ch, int attr);
    void (*TND_ABI scroll)    (int x, int y, int w, int h, int dy, int ch, int attr);
    void (*TND_ABI screen)    (TND_SCREEN *out);   /* 返回 StructSize 填充后的状态 */

    /* --- 驱动（v2.5 追加）
     * 驱动调它登记自己。返回 0 成功；名字重复也返回失败 ——
     * **不静默覆盖**，覆盖是 bug 的温床。 */
    int  (*TND_ABI drv_register)(const TND_DRIVER_DESC *desc);

    /* --- 输入后端（v2.5）
     * 注册一个输入后端。**上层从此不再直连固件 ConIn。** */
    int  (*TND_ABI in_register)(const TND_INPUT *in);

} TND_API_TABLE;

#endif /* TND_API_H */
