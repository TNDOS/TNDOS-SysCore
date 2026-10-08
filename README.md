# TNDOS-SysCore
TNDOS——一个全新定义的DOS！！！

---

## TNDDOS / 2NDDOS
**不是把传统 DOS 搬到 UEFI，而是重新做一个 UEFI 时代的 DOS。**

保留 DOS 的交互模型（简单、直接、命令行优先、`C:\` 盘符），
抛弃 DOS 的内存模型与历史包袱（16 位、BIOS、640KB、DOS ABI）。

- 平台：x86-64 + UEFI-only（ia32 / armv7 / armv8 / RISC-V / LoongArch 预留，暂不实现）
- 无 CSM、无 Legacy BIOS、无 16 位实模式
- 可执行格式：**PE 过渡期**、**TNX V1**
- UEFI 仍是当前赖以生存的平台（文件系统、磁盘、键盘全部借它），**不提前切断**

功能与进度的**逐条清单**见 [TNDOS-目标清单.txt](TNDOS-目标清单.txt)，
版本路线见 [docs/ROADMAP.md](docs/ROADMAP.md)。

### 相关仓库

| 仓库 | 内容 |
|---|---|
| **TNDOS-SysCore**（本仓库） | 操作系统本体：引导器、内核、Shell、样例驱动 |
| **TNDOS-SDK** | 写 TNX 程序和驱动需要的头文件 / 运行时 / 链接脚本 / 样例 |
| **TNDOS-SysAPP** | 随系统发行的外部命令（TREE / FIND / FC / ATTRIB / MORE / EDIT …） |
| **TNDOS-ToolsKit** | 宿主机工具：tnxpack（ELF64 转 TNX）、tnxdump（检查器）、mkfat（FAT16 映像） |
| **WebSrv** | 项目网站与版本索引 |

TNX 格式规范见 [docs/TNX-SPEC.md](docs/TNX-SPEC.md)（已冻结在 v1.0）。

有 Bug 可以提交，欢迎完善！！！

---

## 上手前先看

**下面五条是"会让你白折腾"的东西，不是免责声明。**

### 1. 不支持 EFI 的机器 —— 不是没救，但特别折腾

**两条硬限制：**

- 不支持 EFI 的机器**启动不了**
- 也没法用 **CSM 兼容模式**启动（x86 系）—— TNDDOS 要的是真 UEFI，CSM 给的是 BIOS

**x86 平台**需要 [OpenCore](https://github.com/acidanthera/OpenCorePkg)（它的 OpenDuet
分支一直在维护 DUET 组件）这类兼容层，先用软件把 UEFI 模拟出来。
**不推荐，除非你本来就想折腾。**

有黑苹果经验的可以直接上手 —— **但这条路我们没测过可行性**。
我们实际验证过的只有 **QEMU + SeaBIOS + TianoCore DUET**：
<https://gitlab.com/77-0/tianocore_uefi_duet_installer/>

**其余架构平台**如果没有 UEFI 支持，可以尝试自行移植 UEFI 固件。
**同样不推荐，除非你本来就想折腾。**

### 2. 真机写盘 —— 请先看这条

仓库里默认的 `autoexec.bat` **每次开机都会在真实 ESP 上做写操作**：

```
md TMP  /  copy HELLO.TXT TMP\COPY.TXT  /  del  /  rd TMP
```

破坏范围很小（在 `\EFI\TNDOS\` 下建个临时目录再删掉），但**那是真实写入**。

**上真机前建议先把这几行注释掉。** 这条排第二，是因为它最容易让人在不该出事的地方出事。

### 3. Secure Boot

固件开了 Secure Boot 的话，未签名的 `BOOTX64.EFI` **会被直接拒绝** ——
症状是"什么都没发生"。需要在固件设置里关掉，或者自己加信任。

### 4. 一次一个程序，没有内存隔离

程序都跑在 **ring 0**，物理地址就是虚拟地址，没有分页、没有保护。

**程序崩了系统就崩，而且不会优雅地失败。** 这不是 bug，是当前的设计状态 ——
真 DOS 也是这样，而"丢掉 DOS 的内存模型"正是这个项目后面要做的事。

### 5. 只在 QEMU + OVMF 上验证过

真机处于"**能用但没系统测过**"的状态，不是"支持"。

曾经在真机上跑通过一次，那次**是安全的** —— 但安全的原因是当时的功能面很小，
不是因为做了保护。
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
src/include/tnx.h      TNX 格式定义（内核与工具共用）
src/include/tnd_api.h  TNX 程序能用的内核 API 契约
src/lib/               log(串口) util file(ESP 读写) guid(全局) uni(Unicode)
src/lib/con_uefi.c     控制台后端：UEFI ConOut
src/lib/con_fb.c       控制台后端：framebuffer（自己写像素）
src/lib/fontvga.c      内嵌 8x12 CP437 点阵字体（从 vgaoem.fon 提取）
src/boot/bootx64.c     引导初始化器
src/kernel/kernel.c    内核入口与启动编排
src/kernel/module.c    内核模块框架（注册 / 按序初始化 / 状态报告）
src/kernel/vfs.c       M2 虚拟文件系统、DOS 路径语义、驱动器映射
src/kernel/pmm.c       M3 物理内存管理器（位图）
src/kernel/heap.c      M3 内核堆 kmalloc / kfree
src/kernel/drv.c       驱动加载器（DEVICE= -> LoadImage + StartImage）
src/kernel/tnx.c       TNX 加载器（校验 / 搬运 / 调用入口）
src/kernel/api.c       TNX 程序 API 表（v2.4）与句柄表
src/kernel/conf.c      efidos.sys / config.sys 解析、环境变量
src/kernel/shell.c     Shell、命令、autoexec.bat 批处理
src/drv/               驱动 SDK 与示例驱动（VGA.EFI / KBD.EFI）
boot/                  随源码部署的引导期配置文件
tools/build-run.ps1    一键构建 + 启动验证
build/                 构建产物（build/esp 为树，build/esp.img 为磁盘映像）
```

**TNX 的样例程序、最小运行时、链接脚本不在本仓库** —— 它们在 **TNDOS-SDK**，
构建时直接从那里取。`tnxpack` / `tnxdump` / `mkfat` 同理，在 **TNDOS-ToolsKit**。

（以前 `src/tnx/` 下有一份副本，和 SDK 静默分叉过 —— 改 SDK 不进构建且毫无提示。
一个会漂移的镜像比一个明确的依赖更糟，所以副本删了，改成直接依赖。）
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

## 深入了解

| 文档 | 内容 |
|---|---|
| [docs/STATUS.md](docs/STATUS.md) | 里程碑进度与运行期自检 |
| [docs/PITFALLS.md](docs/PITFALLS.md) | 开发中真抓到的坑（全是"编译过、跑起来才现形"） |
| [docs/DRIVERS.md](docs/DRIVERS.md) | 驱动模型（`DEVICE=`）语义与约定 |
| [docs/CONSOLE.md](docs/CONSOLE.md) | 控制台字符集策略、本地化的正确路径 |
| [docs/TNX-SPEC.md](docs/TNX-SPEC.md) | **TNX 可执行格式规范**（v1.0，已冻结） |
| [docs/TNX-DESIGN.md](docs/TNX-DESIGN.md) | TNX 的设计取舍、v1 边界与反目标 |
| [docs/TODO.md](docs/TODO.md) | 已知问题、待办、后续路线 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 版本路线图 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 提交与代码规范 |
| [CONTRIBUTORS.md](CONTRIBUTORS.md) | 贡献者 |
| [TNDOS-目标清单.txt](TNDOS-目标清单.txt) | 功能清单（已实现 / 未实现，逐条） |

English: [README.en.md](README.en.md)

## 许可

见 [LICENSE](LICENSE)。
