/* ============================================================================
 * TNDDOS — 内部公共接口
 * ==========================================================================*/
#ifndef TND_H
#define TND_H

#include "efi.h"
#include "tnd_utf8.h"
#include "tnx.h"
#include "tnd_api.h"   /* TND_API_TABLE —— 程序运行环境的类型 */

/* ---------------------------------------------------------------- 地址图
 * 在此之前 TNDDOS 没有地址规划：PMM 从 1MB 起首次适配地发页，
 * 内核被固件扔在 ~221MB，谁也不管谁。TNX 强制固定加载地址，这张图就躲不掉了。
 *
 *   0x0000000000000000 - 0x00000000000FFFFF   保留（实模式 / BIOS 遗迹）
 *   0x0000000000100000 - 0x0000000000FFFFFF   PMM 常规分配区（内核堆在这里长）
 *   0x0000000001000000 - 0x00000000017FFFFF   TNX 程序映像窗口（固定，PMM 永不发放）
 *   0x0000000001800000 - .....................  其余交给 PMM
 *
 * 窗口由 PMM 在初始化时就占住（标位图 + 真向固件 AllocatePages），
 * 否则堆会先把它吃掉，加载程序就永远失败。
 */
#define TNX_IMAGE_BASE   0x0000000001000000ULL   /* 16 MiB */
#define TNX_WINDOW_SIZE  0x0000000000800000ULL   /*  8 MiB，窗口 16MiB..24MiB */

#define TND_NAME    "TNDDOS"
#define TND_ALIAS   "2NDDOS"
#define TND_VERSION "0.3.2-M4-SP1"

/* 系统文件布局：除 BOOTX64.EFI 外全部在 \EFI\TNDOS\ */
#define TND_DIR        "\\EFI\\TNDOS"
#define TND_KERNEL     "\\EFI\\TNDOS\\kernel.efi"
#define TND_EFIDOS_SYS "\\EFI\\TNDOS\\efidos.sys"   /* 系统配置，先加载 */
#define TND_CONFIG_SYS "\\EFI\\TNDOS\\config.sys"   /* 用户配置 */
#define TND_AUTOEXEC   "\\EFI\\TNDOS\\autoexec.bat"

#define TND_MAX_LINE   256
#define TND_MAX_ENV    64
#define TND_FILE_CAP   16384   /* 配置文件足够 */
#define TND_BLOB_CAP   65536   /* 读 PE 映像用 */
#define TND_MAX_MODULES 16
#define TND_MAX_PATH   256

/* 全局运行环境，由各入口填好 */
typedef struct {
    EFI_HANDLE        ImageHandle;
    EFI_SYSTEM_TABLE *ST;
    EFI_BOOT_SERVICES    *BS;
    EFI_RUNTIME_SERVICES *RT;
    EFI_SERIAL_IO_PROTOCOL *Ser;
    EFI_FILE_PROTOCOL *Root;            /* ESP 根目录 */
    EFI_HANDLE EspHandle;               /* ESP 设备句柄 */
} TND_ENV;

extern TND_ENV gEnv;

/* ============================ 模块框架 ==================================== */
typedef struct {
    const char *name;
    const char *desc;
    int (*init)(void);          /* 返回 0 = 失败，非 0 = 成功 */
} TND_MODULE_DEF;

extern const TND_MODULE_DEF gModVfs;
extern const TND_MODULE_DEF gModPmm;
extern const TND_MODULE_DEF gModHeap;
extern const TND_MODULE_DEF gModDrv;
extern const TND_MODULE_DEF gModTnx;
extern const TND_MODULE_DEF gModConf;

void module_init_all(void);
void module_report(void);

/* ============================ 驱动加载（DEVICE=） ======================== */
#define TND_MAX_DRIVERS 16
int  drv_init(void);
EFI_STATUS drv_load(const char *path);
void drv_report(void);
int  drv_count(void);
int  drv_ok_count(void);

/* ============================ TNX 加载器 ================================= */
int  tnx_init(void);
EFI_STATUS tnx_load(const char *path, int *outCode, int verbose);
EFI_STATUS tnx_run(const char *path, int argc, char **argv, int *outCode);

/* ============================ 程序运行环境（API v2） ==================== */
void  api_setup(int argc, char **argv);      /* 程序开跑前：装好 fd 0/1/2 与 argv */
void  api_teardown(void);                    /* 程序返回后：关掉它没关的句柄 */
int   api_redirect_stdout(const char *path, int append);   /* 重定向，返回 1 成功 */
int   api_redirect_stdin(const char *path);
void  api_restore_stdio(void);
const TND_API_TABLE *api_get_table(void);
int  tnx_info(const char *path);
int  tnx_find(const char *name, char *out, UINTN cap);
void tnx_report(void);

/* ============================ 日志 / 控制台 =============================== */
void log_init(EFI_SYSTEM_TABLE *st);
void log_puts(const char *s);
void log_putc(char c);
void log_u64(UINT64 v);
void log_hex(UINT64 v);
void log_kv(const char *k, const char *v);
void log_kv_u64(const char *k, UINT64 v);

void con_puts(const char *s);
void con_putc(char c);
void con_write(const char *s, UINTN n);
void con_set_attr(UINTN attr);
void con_reset_attr(void);
void con_cursor(int visible);
UINTN con_cols(void);
UINTN con_rows(void);
void  con_gotoxy(UINTN x, UINTN y);

/* **光标位置也必须走后端接口。**
 * 教训：shell 的行编辑原来直接读 gEnv.ST->ConOut->Mode->CursorColumn/Row，
 * 切到 fb 之后那是**过期值** —— 我们自己写像素，从来没通知过 ConOut。
 * 结果重画落在错误位置，屏幕上出现 "ddidir" 这种鬼东西（命令本身是对的，
 * 只是显示乱了）。任何"当前光标在哪"的问题，答案只能来自当前后端。 */
void  con_getxy(UINTN *x, UINTN *y);
UINTN con_get_attr(void);
int   con_set_scale(int percent);
int   con_get_scale(void);
void  con_write_cells(UINTN x, UINTN y, UINTN w, UINTN h, const TND_CELL *cells, UINTN stride);
void  con_fill(UINTN x, UINTN y, UINTN w, UINTN h, UINTN ch, UINTN attr);
void  con_scroll(UINTN x, UINTN y, UINTN w, UINTN h, int dy, UINTN ch, UINTN attr);

/* ========================= 控制台服务层（M4-SP0）=========================
 * 目的是让内核**不再直接依赖 UEFI 的 ConOut**。
 *
 * 为什么必须分层：ExitBootServices 之后 ConOut 会跟着 Boot Services 一起消失，
 * 而输出是唯一能告诉你"为什么崩了"的东西。所以后端必须可换，而且要能提前换。
 *
 * 现在已经有两个后端：
 *   uefi   走 ConOut（默认，成熟）
 *   fb     自己写 framebuffer（GOP 取地址 + 内嵌字体画字）
 *
 * 接口约定：Write 收到的是**已经做过换行翻译**的字节流（\n -> \r\n 在
 * console.c 那一层做完），而且可能含 \0，所以必须带长度、不能靠 strlen。
 * UTF-8 -> 字形的转换由各后端自己负责 —— UEFI 要转 UCS-2，fb 要查码点。 */
typedef struct {
    const char *Name;
    int    (*Init)(void);                        /* 0 = 失败，后端保持不可用 */
    void   (*Write)(const char *s, UINTN n);
    void   (*Clear)(void);
    void   (*GotoXY)(UINTN x, UINTN y);
    void   (*GetXY)(UINTN *x, UINTN *y);
    void   (*SetAttr)(UINTN attr);               /* DOS 属性字节：fg | bg<<4 */
    void   (*Cursor)(int visible);
    UINTN  (*Cols)(void);
    UINTN  (*Rows)(void);

    /* 缩放。后端不支持就返回 0（UEFI 的字体由固件决定，改不了）。
     * 放在接口里而不是让调用方去猜后端是不是 fb —— 那又是绕过服务层。 */
    int    (*SetScale)(int percent);
    int    (*GetScale)(void);

    /* 批量原语。**后端可以不实现** —— 路由层会用 GotoXY+Write 逐格兜底
     * （慢但正确）。所以 UEFI 后端一行都不用改，fb 只加快速路径。 */
    void   (*WriteCells)(UINTN x, UINTN y, UINTN w, UINTN h, const TND_CELL *cells, UINTN stride);
    void   (*Fill)(UINTN x, UINTN y, UINTN w, UINTN h, UINTN ch, UINTN attr);
    void   (*Scroll)(UINTN x, UINTN y, UINTN w, UINTN h, int dy, UINTN ch, UINTN attr);
} TND_CONSOLE;

void        con_register(const TND_CONSOLE *c);
int         con_select(const char *name);        /* 1 = 成功 */
const char *con_current(void);
void        con_report(void);                    /* 列出后端；当前那个打 * */

/* 内置的两个后端。注册在 log_init 里，但只**选中** uefi ——
 * fb 要等内核堆就绪、用户显式要求时才 Init（它要分配影子缓冲）。 */
extern const TND_CONSOLE gConUefi;
extern const TND_CONSOLE gConFb;
void con_u64(UINT64 v);
void con_hex(UINT64 v);
void con_clear(void);
void con_kv(const char *k, UINT64 v, const char *unit);   /* 对齐的 "标签 : 数字 单位" */

/* ============================ 字符串 / 内存工具 ========================== */
UINTN t_strlen(const char *s);
int   t_strcmp(const char *a, const char *b);
int   t_strncmp(const char *a, const char *b, UINTN n);
int   t_stricmp(const char *a, const char *b);
int   t_strnicmp(const char *a, const char *b, UINTN n);
void  t_strcpy(char *d, const char *s);
void  t_strncpy(char *d, const char *s, UINTN cap);
void  t_memzero(void *d, UINTN n);
void  t_memcpy(void *d, const void *s, UINTN n);
char *t_skip_ws(char *s);
void  t_rtrim(char *s);
void  t_utoa(UINT64 v, char *out);
int   t_is_space(char c);
int   t_toupper(int c);
void  t_upper(char *s);
const char *t_status_str(EFI_STATUS s);

/* ============================ 文件（UEFI 层） ============================ */
EFI_STATUS t_open_root(EFI_HANDLE image);
EFI_STATUS t_read_file(const char *name, char *buf, UINTN cap, UINTN *outLen);
EFI_STATUS t_read_file_alloc(const char *name, void **buf, UINTN *len);

/* ============================ VFS（M2） ================================== */
#define VFS_SEEK_SET 0

int  vfs_init(void);
const char *vfs_cwd(void);
void vfs_set_drive(char letter);
char vfs_drive(void);
EFI_STATUS vfs_resolve(const char *dos, char *out, UINTN cap);
EFI_STATUS vfs_open(const char *dos, int write, EFI_FILE_PROTOCOL **out);
EFI_STATUS vfs_read_all(const char *dos, char *buf, UINTN cap, UINTN *len);
int  vfs_exists(const char *dos);
EFI_STATUS vfs_create(const char *dos, EFI_FILE_PROTOCOL **out);
EFI_STATUS vfs_truncate(EFI_FILE_PROTOCOL *f);
int  vfs_dir(const char *dos);
int  vfs_type(const char *dos);
int  vfs_mkdir(const char *dos);
int  vfs_rmdir(const char *dos);
int  vfs_unlink(const char *dos);
int  vfs_rename(const char *a, const char *b);
int  vfs_copy(const char *a, const char *b);
int  vfs_chdir(const char *dos);
void vfs_report(void);
const char *vfs_efi_error(EFI_STATUS s);

/* ============================ PMM（M3） ================================== */
typedef struct {
    UINT64 TotalPages;      /* 位图覆盖的物理页总数 */
    UINT64 ManagedPages;    /* 纳入管理的 Conventional 页数 */
    UINT64 FreePages;
    UINT64 UsedPages;
    UINT64 ReserveLowPages; /* 低端保留区（1MB 以下） */
    UINT64 BitmapBytes;
    UINT64 AllocCount;
    UINT64 FreeCount;
    UINT64 FailCount;
} PMM_STATS;

int    pmm_init(void);
void   pmm_stats(PMM_STATS *out);
void   pmm_report(void);
UINT64 pmm_alloc_page(void);
UINT64 pmm_alloc_pages(UINTN count);
void   pmm_free_page(UINT64 addr);
void   pmm_free_pages(UINT64 addr, UINTN count);
void   pmm_reserve(UINT64 base, UINTN pages);
int    pmm_selftest(void);

/* ============================ 内核堆（M3） =============================== */
typedef struct {
    UINT64 Chunks;
    UINT64 BlockCount;
    UINT64 FreeBlocks;
    UINT64 UsedBytes;
    UINT64 FreeBytes;
    UINT64 PeakUsed;
    UINT64 AllocCount;
    UINT64 FreeCount;
} HEAP_STATS;

int  heap_init(void);
void *kmalloc(UINTN size);
void  kfree(void *p);
void  heap_stats(HEAP_STATS *out);
void  heap_report(void);
int   heap_selftest(void);

/* ============================ 配置 / 环境 ================================ */
int  conf_init(void);
void conf_parse(const char *path, int isSystem);
const char *env_get(const char *name);
int  env_set(const char *kv);
void env_dump(void);

/* ============================ Shell ====================================== */
int  shell_start(void);
void shell_exec_line(char *line);
void shell_exec_batch(const char *label, const char *text, UINTN len);

#endif /* TND_H */
