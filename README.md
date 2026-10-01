# TNDOS-SysCore
TNDOS——一个全新定义的DOS！！！

---

## TNDDOS / 2NDDOS
**不是把传统 DOS 搬到 UEFI，而是重新做一个 UEFI 时代的 DOS。**

保留 DOS 的交互模型（简单、直接、命令行优先、`C:\` 盘符），
抛弃 DOS 的内存模型与历史包袱（16 位、BIOS、640KB、DOS ABI）。

- 平台：x86-64 + UEFI-only（ia32 / armv7 / armv8 / RISC-V 预留接口，暂不实现）
- 无 CSM、无 Legacy BIOS、无 16 位实模式
- 可执行格式：**PE 过渡期**、**TNX V1**
- UEFI 仍是当前赖以生存的平台（文件系统、磁盘、键盘全部借它），**不提前切断**

当前版本 **0.3.2**（M0 / M2 / M3 已完成，TNX 可执行格式已落地，API 已到 v2.x）。

### 相关仓库

| 仓库 | 内容 |
|---|---|
| **TNDOS-SysCore**（本仓库） | 操作系统本体：引导器、内核、Shell、样例驱动 |
| **TNDOS-SDK** | 写 TNX 程序和驱动需要的头文件 / 运行时 / 链接脚本 / 样例 |
| **TNDOS-ToolsKit** | 宿主机工具：tnxpack（ELF64 转 TNX）、tnxdump（检查器）、mkfat（FAT16 映像） |

TNX 格式规范见 docs/TNX-SPEC.md（已冻结在 v1.0）。

有Bug可以提交，欢迎完善！！！

---

## 启动链

```
UEFI Firmware
  -> \EFI\BOOT\BOOTX64.EFI     初始化器（地位 ≈ DOS 的 PBR，瞬态）
  -> \EFI\TNDOS\kernel.efi     内核   （≈ IO.SYS）
  -> \EFI\TNDOS\efidos.sys     系统配置（安装期部署，先加载）
  -> \EFI\TNDOS\config.sys     用户配置
  -> Shell
  -> \EFI\TNDOS\autoexec.bat   启动脚本（最后执行）
```

`efidos.sys` 与 `config.sys` 语法相同、加载顺序不同：前者是系统级、安装期写入，
后者完全交给用户，因此用户的设置可以覆盖系统默认值。

`EXIT` 从 Shell 退出后，kernel.efi 返回，初始化器收尾并退出 —— 这就是"瞬态 PBR"的实测语义。

---

## 目录

```
src/include/efi.h      手写的 UEFI 定义（偏移逐个按 UEFI 2.10 核对）
src/include/tnd.h      TNDDOS 内部公共接口
src/lib/               log(串口+控制台) util file(ESP 读写) guid(全局)
src/boot/bootx64.c     引导初始化器
src/kernel/kernel.c    内核入口与启动编排
src/kernel/module.c    内核模块框架（注册 / 按序初始化 / 状态报告）
src/kernel/vfs.c       M2 虚拟文件系统、DOS 路径语义、驱动器映射
src/kernel/pmm.c       M3 物理内存管理器（位图）
src/kernel/heap.c      M3 内核堆 kmalloc / kfree
src/kernel/drv.c       驱动加载器（DEVICE= -> LoadImage + StartImage）
src/kernel/tnx.c       TNX 加载器（校验 / 搬运 / 调用入口）
src/drv/               驱动 SDK 与示例驱动（VGA.EFI / KBD.EFI）
src/tnx/               TNX 样例程序 + 最小运行时 + 链接脚本
src/include/tnx.h      TNX 格式定义（内核与工具共用）
src/include/tnd_api.h  TNX 程序能用的内核 API 契约
src/kernel/conf.c      efidos.sys / config.sys 解析、环境变量
src/kernel/shell.c     Shell、命令、autoexec.bat 批处理
boot/                  随源码部署的引导期配置文件
tools/build-run.ps1    一键构建 + 启动验证
tools/mkfat.ps1        从目录生成真实 FAT16 磁盘映像
tools/tnxpack.ps1      ELF64 -> TNX 打包器
build/                 构建产物（build/esp 为树，build/esp.img 为磁盘映像）
```

---

## 构建与验证

```powershell
\.\tools\build-run.ps1                       # 编译 -> 组 ESP -> 造 FAT16 映像 -> QEMU/OVMF 启动
\.\tools\build-run.ps1 -NoRun                # 只编译
\.\tools\build-run.ps1 -Seconds 60 -Keys 'h,e,l,p,ret'   # 经 QEMU monitor 注入按键
\.\tools\build-run.ps1 -Fat vvfat            # 退回 QEMU 的 vvfat（写操作会崩，仅供对照）
```

### 工具链：全部走环境变量

脚本里**不写死任何机器相关的路径**。取不到就先自动探测（PATH、以及 QEMU 自带的
`share\` 目录），再取不到就报错，并把可以直接粘贴的 `setx` 命令打印出来。

| 环境变量 | 指向 | 必填 |
|---|---|---|
| `TNDDOS_LLVM_BIN` | 含 `clang.exe` 的**目录** | 是（clang 已在 PATH 上时可省） |
| `TNDDOS_QEMU` | `qemu-system-x86_64.exe` 的**完整路径** | 是（qemu 已在 PATH 上时可省） |
| `TNDDOS_OVMF_CODE` | OVMF 代码固件 `*.fd` | 否，默认从 QEMU 目录的 `share\` 里找 |
| `TNDDOS_OVMF_VARS` | OVMF 变量存储模板 `*.fd` | 否，同上 |

```powershell
# 实际只要设这两个 —— OVMF 会从 QEMU 自己的 share\ 目录自动找到
setx TNDDOS_LLVM_BIN "D:\LLVM\bin"
setx TNDDOS_QEMU     "D:\qemu\qemu-system-x86_64.exe"

# setx 只影响新开的进程，所以要重开一个终端。先确认定位结果：
.	ools\build-run.ps1 -ShowEnv
```

`-ShowEnv` 会打印四个路径的实际落点，不编译也不启动，排查环境问题用这个。

验证方式是 **headless 启动 + 串口日志**。`build\serial.log` 里既有 `[log]` 前缀的诊断行，
也有 ConOut 的用户可见输出（OVMF 的 console splitter 会把 ConOut 一并送到串口）。

### 为什么自己造 FAT16 映像

QEMU 的 `-drive file=fat:rw:<目录>`（vvfat）读没问题，**写回时会直接把 QEMU 干掉**：

```
ERROR: block/vvfat.c:2429: commit_direntries: assertion failed: (mapping)
```

TNDDOS 是个 DOS，不能拿一个写不进去的文件系统来测。所以 `tools/mkfat.ps1` 自己造一块
真 FAT16：MBR + 单分区（type 0xEF）+ 双 FAT + 固定根目录 + 数据区，只支持 8.3 短名
（本项目的 ESP 里全是 8.3，省掉长文件名一整块复杂度）。

---

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

---

## 开发过程中真抓到的坑（都是"编译过、跑起来才现形"）

| # | 症状 | 真因 |
|---|---|---|
| 1 | `HandleProtocol` 找不到文件系统 | Simple File System 的 GUID 抄成了 `0x0964E5B2`，应为 `0x964E5B22`。看似"设备没文件系统"，其实是 GUID 不对 |
| 2 | `LoadImage` 只回一个 `Unsupported` | 读内核的缓冲 cap 16384 < kernel.efi 17920，文件被自己截断 |
| 3 | 内核 `DeviceHandle == 0` | 用内存缓冲调 `LoadImage` 时 EDK2 **不会**把父映像的设备句柄传下去，必须交棒前显式写入 |
| 4 | 日志里每条输出出现两遍 | OVMF 的 console splitter 本就把 ConOut 送到串口，再镜像一遍就是双份 |
| 5 | PMM 报告 268435456 页 / 位图 32 MB | 把 MMIO 空洞也算进了物理内存。QEMU/OVMF 的 MMIO 落在 TB 级地址，必须按 RAM 类型过滤 |
| 6 | 固件认得出分区却找不到 `\EFI\BOOT\BOOTX64.EFI` | 自造 FAT16 时目录簇的 FAT 表项留成了 0 —— 而 0 的含义是"空闲簇"，于是目录被当成空的 |
| 7 | FAT16 数据区整体偏移一个扇区 | PowerShell 的 `[int]` 是**四舍五入**不是截断，根目录扇区数被算成 33 而非 32 |
| 9* | TNX 程序一跑就 `#UD 无效指令`，RIP 还落在内核里 | **两种调用约定撞车**：TNX 用 `x86_64-unknown-none`（System V，第一个参数在 RDI），内核用 `x86_64-pc-windows-msvc`（MS ABI，第一个参数在 RCX）。程序把指针放 RDI，内核去 RCX 拿垃圾。修法：把 TNX 的 ABI 钉死为 SysV，内核那侧用 `sysv_abi` 适配 |
| 8 | 根目录**第二次 DIR 是空的**（第一次正常） | 卷根句柄是复用的，而 EDK2 的 FAT 驱动枚举到末尾后把读位置停在 EOF，第二次 `Read` 直接返回长度 0。非根目录每次新开句柄所以看不出来。修法：枚举前 `SetPosition(0)` |


---

## 驱动（DEVICE=）

语义按方案：**DEVICE=<文件> 约等于 UEFI Shell 的 load fs0:\<文件>**。
内核把该文件当 UEFI 映像读进来，LoadImage 之后 StartImage 调它的入口点。

```
── efidos.sys  系统配置（安装期部署，先加载）
    DEVICE    = VGA.EFI
[log] drv: \EFI\TNDOS\DRIVERS\VGA.EFI  4096 bytes
[VGA.EFI] TNDDOS 控制台驱动
          文本模式尺寸: 80 列 x 25 行
          模式号 当前/最大: 2 / 3   属性 0x0000000000000007
          [属性切换测试] 这一行是 0x0F 白字黑底
[VGA.EFI] 初始化完成 -> EFI_SUCCESS
          已加载 4096 字节，入口返回 EFI_SUCCESS

── config.sys  用户配置
    DEVICE    = KBD.EFI
[log] drv: \EFI\TNDOS\DRIVERS\KBD.EFI  3584 bytes
[KBD.EFI] TNDDOS 键盘驱动
          ConsoleInHandle: 0x000000000E7A7818
          WaitForKey 事件: 有效
          清空输入缓冲区: 完成
[KBD.EFI] 初始化完成 -> EFI_SUCCESS

  驱动 (2 个：成功 2，失败 0)
    OK        VGA.EFI   4096 字节
    OK        KBD.EFI   3584 字节
```

细节约定：

- 路径走 **VFS**，所以 DOS 路径语义（`\` / `.` / `..` / 驱动器字母）都能用。
- 只写文件名时（`DEVICE=VGA.EFI`），先去 `\EFI\TNDOS\DRIVERS\` 找，找不到再当相对路径算。
- 驱动加载**失败绝不中断启动**，只记一笔，`DRIVERS` 命令里能看到失败项。
- 用内存缓冲 `LoadImage` 时固件不会把设备句柄传下去，必须显式补上（M0 踩过一次）。

写一个驱动只要实现 `efi_main(ImageHandle, SystemTable)`，链接 `drvlib.c` + `utf8.c`，
再用 `-Wl,/subsystem:efi_application,/entry:efi_main` 编译即可 —— 见 `src/drv/vga.c`。

```
C:\EFI\TNDOS>drivers

  驱动 (2 个：成功 2，失败 0)
    OK        VGA.EFI   4096 字节
    OK        KBD.EFI   3584 字节

C:\EFI\TNDOS>load vga.efi
[VGA.EFI] TNDDOS 控制台驱动
  ...
  OK
```

---

## 控制台字符集：一律 ASCII

**所有面向屏幕的输出都是 ASCII，一条中文都没有。** 这不是偷懒，是必须的：

UEFI 固件的点阵字体**不保证**带 ASCII 以外的字形。实测 OVMF 就是这样 ——
中文 codepoint 找不到字形，屏幕上是一片**纯空白**。一个读不出字的控制台比没有控制台更糟。

所以定位是：**控制台用任何固件都画得出来的字符集；本地化是后面加的一层，不是把中文硬编码进每条消息。**

这也正好是 DOS 自己的历史：真 DOS 就是英文的，中文是后来挂 UCDOS 那类东西才有的。
要做中文，正确路径是 framebuffer console + 自带点阵字体 + 消息目录（message catalog），
而不是把字符串散落在各个 .c 文件里。

源码里的**注释仍然用中文**（那是给人读源码的），只有 `con_*` / `log_*` / `dputs` 的字符串是英文。

---

## TNX 可执行格式（v1）

> **TNX 不解决「现代可执行文件格式的一切问题」，它只负责把一个已经链接好的
> TNDDOS 程序，用最低成本装进 TNDDOS 的地址空间。**

### 为什么不用 PE

PE 今天是**免费的** —— `LoadImage` 是固件替我们写的加载器。所以 TNX 的价值不是
「省代码」，而是：**当我们不能再调 `LoadImage` 的时候，我们还能加载程序。**

它真正要替代的不是 PE，是「离开 UEFI 之后自己写一个 PE 加载器」——那需要处理
DOS 头、PE 签名、COFF、可选头、段表、RVA 映射、基址重定位、导入解析，八百行且全是边界情况。
TNX 加载器是 `memcpy` 加一个跳转，**它不可能有 bug 到哪去**。

### 核心取舍

| 决定 | 省掉的东西 |
|---|---|
| 固定加载地址 | 整个重定位表 |
| 无动态导入 | 符号解析；系统调用走传入的 API 表 |
| 4 个固定段 tag | 段表分支 30 行写完 |
| 文件内连续排布 | 碎片处理 |

### 布局（头部恰好 48 字节，段表项 40 字节）

```
+---------------------------+
| TNX_HEADER   (48 字节)     |  魔数/版本/头长/标志/段数/入口/基址/映像大小
+---------------------------+
| 段表 (40 字节 x N)         |  CODE / DATA / RODT / RSRC / SIGN
+---------------------------+
| 段数据（16 字节对齐）        |
+---------------------------+
```

加载路径：校验魔数 → 校验版本/标志/基址/段表边界 → 逐段校验偏移与长度 →
清零映像窗口 → 逐段搬运 + BSS 置零 → 调入口。**没有重定位、没有导入、没有符号表。**

### 地址图

```
0x0000000000000000 - 0x00000000000FFFFF   保留（实模式 / BIOS 遗迹）
0x0000000000100000 - 0x0000000000FFFFFF   PMM 常规分配区（内核堆在这里长）
0x0000000001000000 - 0x00000000017FFFFF   TNX 程序映像窗口（8 MiB，PMM 永不发放）
```

窗口由 PMM 在初始化时就占住（标位图 + 真向固件 `AllocatePages`），
否则内核堆的首次适配会先把它吃掉，之后每次加载 TNX 都失败。

### 工具链

TNX **不需要自己的编译器，也不需要自己的链接器**：

```
hello.c -> clang -> hello.o -> ld.lld -> hello.elf -> tnxpack -> HELLO.TNX
```

`tools/tnxpack.ps1` 只做最后一步：读 ELF64 的 `PT_LOAD` 段，写 TNX 的段表加段数据。
（用 PowerShell 而非 C 是现实取舍 —— 这台机器上只有 clang，没有 C 运行库，
写 C 版宿主工具反而要手搓文件 IO。等 SDK 成形再重写并自举。）

### 实测

```
C:\EFI\TNDOS>HELLO.TNX          <- 直接敲程序名就执行（DOS 的规矩）

  ================================================
   Hello from a TNX program
  ================================================
  image addr    : 0x0000000001000650
  fixed base    : inside the 16MiB window          OK
  [rodata] read-only section is live
  [data]        wrote+read back: 0x123456789ABCDEF0
  [bss]         zero-filled by the loader          OK
  [heap]        kernel heap reachable from a TNX program
  ticks         : 0   (0 = no timer subsystem yet)
  returning 0

C:\EFI\TNDOS>HELLO               <- 扩展名可选
C:\EFI\TNDOS>NOPE.TNX
  Bad command or file name      <- DOS 的原话

  ================================================
   Hello from a TNX program
  ================================================
  image addr    : 0x0000000001000650
  fixed base    : inside the 16MiB window          OK
  [rodata] read-only section is live
  [data]        wrote+read back: 0x123456789ABCDEF0
  [bss]         zero-filled by the loader          OK
  [heap]        kernel heap reachable from a TNX program
  ticks         : 0   (0 = no timer subsystem yet)
  returning 0

```

### 命令语义

| 输入 | 行为 |
|---|---|
| `HELLO.TNX` / `HELLO` | **直接执行**。加载器不打印任何自己的东西 —— 跑一个程序不该先跟你汇报段表 |
| `TNX <file>` | 打印头部与段表，**不执行** |
| `TNXRUN <file>` | 执行，并打印加载器的完整跟踪（调试用） |

查找顺序和 DOS 一样：当前目录优先，然后依次查 `PATH` 的每一项；
名字里没有 `.` 就自动补 `.TNX`。找不到就是 `Bad command or file name`。

### v1 的边界（写清楚，不含糊）

- **一次只能装一个程序，没有隔离。** 没有分页，物理地址 == 虚拟地址。
  这不是缺陷 —— DOS 就是这样：固定段、一次一个、没有保护。
- **程序跑在内核栈上、ring 0。** 它崩了系统就崩了。真正的答案需要异常处理，
  而异常处理需要自己的 IDT，而 IDT 需要 `ExitBootServices`。
- **`SYSCALL` 现在还实现不了**，所以 API 是「加载器把函数表当参数传给入口」。
  源码级 API 不变，将来换成 syscall 桩，程序一个字节都不用改。

### 非目标（Anti-goals）

以下东西 TNX **永远不做**，做了就说明在重造 PE：

```
动态链接 / 导入表 / 导出表       TLS
异常元数据（.eh_frame / unwind）  调试信息 / 符号表
重定位                            资源目录（不是「资源」，是「目录」）
ABI 自动发现
```

**扩展规则**：任何新字段要进 header，必须先回答「加载器拿它干什么」。
答不上来的一律进 RSRC —— 加载器不解析 RSRC，所以塞什么都不会污染格式。
（`Checksum` 和 `Reserved` 就是被这条规则砍掉的。）
---

## 已知问题 / 待办

- **IDT 尚未接管**：Boot Services 存活期间固件拥有 IDT，此时抢占会打断固件的
  定时器 / USB / 磁盘中断。GDT/IDT 与异常处理要等 `ExitBootServices` 那一刻做一次性切换。
- **没有虚拟内存 / 分页**：目前是 UEFI 建立的身份映射。
- **Shell 仍编译在 kernel.efi 内**：按方案最终要分离成独立用户程序。
- **没有 Syscall / 用户态 / 调度器**。
- **驱动模型还很薄**：`DEVICE=` 只做 LoadImage + StartImage，驱动之间、驱动与内核之间
  没有协议（GUID）契约 —— 内核目前只「知道加载过什么」，还不能「向驱动要服务」。
- **驱动映像缓冲不释放**：每个驱动几十 KB，故意留着最保险。
- **驱动必须是单个 PE**：不支持依赖库，也没有私有驱动格式。
- **`REN` 是复制后删源**（UEFI 没有 rename），受 64 KB 缓冲限制。
- **`COPY` 不支持目标为目录**，需要写全文件名。
- **PMM 空闲查找是 O(n) 线性扫描**，页多了会慢；够用但迟早要换成空闲链。
- **本地化尚未做**：控制台是纯 ASCII（见上面「控制台字符集」一节），中文界面要等字体层。

## 后续（按方案）

驱动模型 -> VFS 脱离 UEFI 换成自有 FAT32 驱动 -> 调度 -> Syscall -> 用户程序。
`ExitBootServices` 不再是里程碑，等驱动模型就绪后再决定何时切换。
