# 里程碑状态  > 本文件从 README.md 拆出。最后更新随源码走。 
## 当前状态

### M0（已完成）

- [x] UEFI 从 FAT 盘加载并执行 `BOOTX64.EFI`
- [x] 初始化器定位 ESP、读出 `kernel.efi`、`LoadImage`/`StartImage` 交棒
- [x] 解析 `efidos.sys` 与 `config.sys`，环境变量与 `%VAR%` 展开
- [x] 键盘输入、Shell、`autoexec.bat` 最后执行
- [x] 内核返回后初始化器收尾退出（瞬态 PBR 语义）

### M1（跳过）

### M2（已完成）—— VFS 与内核模块化

- [x] **内核模块框架**：vfs / pmm / heap / drv / conf 注册、按序初始化、逐个报告状态
- [x] **VFS 层**：DOS 路径语义翻译成 UEFI 路径
      `C:\EFI\TNDOS\K.efi` / `.` / `..` / 相对路径 / 驱动器字母
- [x] **驱动器映射**：`C:` -> ESP 根目录
- [x] **命令**：DIR / CD / MD / RD / TYPE / DEL / REN / COPY / VOL / DRIVERS / LOAD / TNX / TNXRUN
- [x] **敲程序名就执行**（DOS 的规矩），扩展名可选
- [x] **写操作在真实 FAT16 上验证通过**（建目录、复制、列出、删除、删目录）

### M3（已完成）—— 物理内存管理器与内核堆

- [x] **PMM**：从 UEFI Memory Map 建位图，管理 255 MB / 65400 页，位图仅 7 KB
- [x] 只统计真 RAM 类型（MMIO 会落在 TB 级地址，必须排除）
- [x] 扣掉位图自身与低端 1MB
- [x] 取页走 UEFI `AllocatePages(AllocateAddress)` 实际占住，防止固件重复分配
- [x] **内核堆**：按地址排序的块链 + 首次适配 + 相邻合并，`kmalloc` / `kfree`
- [x] **自检**：PMM 分配 8 页写满模式读回比对再归还；堆分配 100/4000/64 三块验证互不串扰后释放回收
- [x] `MEM` 同时给出 UEFI 口径与 TNDDOS 自有口径；`MEMTEST` 可随时重跑自检

### M4（已起步）—— 驱动加载器

- [x] **`DEVICE=<文件>` 真的加载**：等价于 UEFI Shell 的 `load fs0:\<文件>`，
      即读进来 -> `LoadImage` -> `StartImage`
- [x] 路径走 VFS；只写文件名时去 `\EFI\TNDOS\DRIVERS\` 找
- [x] 驱动加载失败不中断启动，失败项在 `DRIVERS` 里可见
- [x] **驱动 SDK**：`src/drv/drv.h` + `drvlib.c`，写驱动只要实现 `efi_main`
- [x] **两个示例驱动**：`VGA.EFI`（查文本模式、真实切换一次属性）、
      `KBD.EFI`（探输入设备、清空缓冲区）
- [x] **交互式 `LOAD` 命令**，实测可重复加载

### 运行期自检

代码里埋了几处自检，专门抓**编译器看不见**的错：

1. `LoadedImage.SystemTable == SystemTable` —— 校验 `EFI_LOADED_IMAGE_PROTOCOL` 偏移
2. `FirmwareVendor == "EDK II"` —— 校验 `EFI_SYSTEM_TABLE` 偏移
3. `t_read_file` / `vfs_read_all` 缓冲满即报错 —— 文件截断绝不静默
4. `heap.c` 的 `kfree` 校验魔数与重复释放

