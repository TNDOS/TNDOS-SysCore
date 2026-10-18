# TNDDOS 求助：UEFI 应用在 AMD 新平台上枚举不到 NVMe 控制器

下面这份是自包含的，不需要看别的上下文。

---

## 一 项目是什么

**TNDDOS / 2NDDOS** —— 不是把传统 DOS 搬进 UEFI，而是**重做一个 UEFI 时代的 DOS**：
保留 DOS 的交互模型（命令行、盘符、config.sys/autoexec.bat、8.3 名字），
丢掉它的内存模型（实模式、分段、640KB）。

- 语言：C（C99），x86-64，**完全没有 16 位代码、没有 BIOS、没有 CSM**
- 产物：一个 PE32+ UEFI 应用（我们的内核），从 ESP 上的 \EFI\BOOT\BOOTX64.EFI 启动
- 自己实现了：控制台（帧缓冲直接写）、输入（PS/2 驱动）、时基（TSC）、
  物理内存管理、堆、VFS、shell、批处理、TNX 可执行格式及加载器
- 目标：**调用 ExitBootServices 之后，机器仍然完全可用** —— 这是整个项目的核心目标

工具链：clang + lld（E:\LLVM），QEMU + OVMF 做日常验证。

---

## 二 现在做到哪了

**已经跑通的（QEMU + OVMF，全链路）：**

1. `ExitBootServices` 成功返回，机器继续运行
2. 退出之后仍然可用：
   - 控制台：fb 后端直接写帧缓冲
   - 输入：PS/2（8042 初始化**故意推迟到退出之后**，否则固件收尾时卡死）
   - 时基：TSC
   - 内存分配：接管表里把 AllocatePool/FreePool/Stall 换成了自己的实现
   - 文件系统：驱动提供块设备 -> blk 层 -> 自己的 FAT16 读取器 -> 目录项
3. 多个 TNX 驱动可以同时存在（每个驱动一个独立的内存窗口槽）
4. AHCI 和 NVMe 驱动都能把自己的块设备注册给内核

**已知未完成：**

- FAT16 only（真机 ESP 是 FAT32）
- cd/type 还走固件，退出后被挡住
- 构建流程不是原子的（中途失败会留半成品镜像）

---

## 三 问题（这才是要问的）

### 现象

**在目标真机上，内核一个块设备都没有。**

真机屏幕输出：

```
Drivers (4 total: 3 ok, 1 failed)
  OK        VGA.EFI   3584 bytes
  OK        PS2.TNX   10976 bytes
  OK        NVME.TNX  13696 bytes
  LOAD-FAIL AHCI.TNX  0 bytes          <- 这个另说，见下

Registered (2 drivers, 2 bindings)
  PS2   type=2  bound=1
  NVME  type=3  bound=1      <- 驱动"绑定"成功了

...然后 exitboot ...
---- takeover self-check ----
  block devices : 1 NVME0    <- 但只有一个块设备，而实际上它没工作
  fat           : not found
```

串口日志里 **没有任何 `[nvme]` 开头的行** —— 而同一个驱动在 QEMU 上会打一长串。

**在 QEMU + OVMF 上，同一个构建、同样的配置、同样的顺序：完全正常**，
NVMe 控制器被认出来，块设备注册成功，FAT 挂载成功，dir 能列目录。

### 目标真机

```
OEM/型号   ASUSTeK TX Gaming FA608WM（华硕 天选6 / TUF Gaming A16）
BIOS       FA608WM.304 (AMI)
CPU        **AMD Ryzen AI 9 HX 370**（Zen 5，2024 年底新平台）
内存       15.1 GB
固件       UEFI
存储       Samsung MZVL81T0HELB (1TB) NVMe，GPT
           用户说物理上是 2 块 1TB RAID1，但——
           Windows 用**微软原生 stornvme** 驱动就能看到 disk0，
           **没有任何 Intel VMD / AMD RAID / iaStor 控制器**
           -> 那块盘对操作系统就是一个标准 NVMe 名字空间
ESP        disk0 part1，FAT32，256MB，已用约 105MB
```

### 我们的 PCI 枚举是怎么做的

```c
/* 从 ACPI 取 MCFG -> 得到 ECAM 基址；然后只扫一条总线 */
for (bus = gEcamBusStart; bus < gEcamBusStart + 1; bus++) {
    for (dev = 0; dev < 32; dev++) {
        for (fn = 0; fn < 8; fn++) {
            /* ECAM: 基址 + (bus<<20) + (dev<<15) + (fn<<12) + 偏移，普通内存读写 */
            读 vendor/class，存进设备表
        }
    }
}
/* 之后：找 PCI 桥（class 0x060400），读偏移 0x18 的 dword，bit8..15 是 secondary bus，扫它 */
/* 之后：找 class 0x0104（RAID/VMD），读配置空间 0x40 的 VMCAP 拿总线范围，扫那段 */
```

**NVMe 驱动的匹配条件是 class 0x010802（NVM）。**

### 已经排除的

- 不是 Intel VMD（AMD 平台，且 Windows 用原生 NVMe 驱动）
- 不是"盘被 RAID 藏起来"（Windows 不用任何 RAID 驱动就能看）
- 驱动文件本身没问题（文件在 ESP 上、大小正确、QEMU 上同一个文件能跑）
- 不是窗口基址冲突（每个驱动一个槽，且同槽会被拒绝加载）

### 我们的怀疑（未验证）

1. **AMD Zen 5 平台上 MCFG 可能有多条记录**，而我们只取了第一条、且只扫一条总线
   —— 如果第一条不覆盖 NVMe 所在的总线，就会"枚举看起来正常但设备少"
2. ECAM 的 bus 号计算在这台机器上可能和 OVMF 不同
3. NVMe 控制器被枚举到了，但 attach 阶段失败（这一条和上面矛盾：
   串口里连一句 [nvme] 都没有，说明很可能连"找到控制器"都没走到）

### 诊断手段（已就绪，等真机输出）

- `PCILIST`：把枚举到的每个设备按 BDF / class / vendor:device 列出来，
  并标注哪个是 NVMe（0x010802）、哪个是 RAID/VMD（0x0104）
- `exitboot`：自带接管自检，退出后自动打块设备列表 / FAT 挂载 / 根目录
- `FSINFO`：把每个探测点的引导扇区解出来（bytes/sec、spc、rootEnt、fatsz、类型串）

### 附带一个问题（量级小得多，但也是真的）

真机上，读 ESP 上的配置文件时**前 5 次 Open 成功，从第 6 次开始全部失败**：

```
#1 efidos.sys    1864 B  OK
#2 PS2.TNX      10976 B  OK
#3 PS2.TNX      10976 B  OK    （TNX 加载器会再读一次）
#4 NVME.TNX     13696 B  OK
#5 NVME.TNX     13696 B  OK
#6 AHCI.TNX     10560 B  FAIL   Open 成功但 Read 第一次就返回 0 字节
#7 config.sys     880 B  FAIL   同样 0 字节
#8 autoexec.bat  2783 B  FAIL   Open 本身失败
```

换个顺序就换个文件倒霉；文件大小无关（失败的那个比先成功的还小）。
QEMU 上不复现 —— 而 **QEMU 的 ESP 是 FAT16，真机是 FAT32**（FatPkg 里这是两条独立代码路径）。

---

## 四 想问的

1. **AMD Zen 5 平台上，一个自制的 UEFI 应用用 MCFG/ECAM 扫 PCI，
   有哪些和 Intel/OVMF 不一样的坑？** MCFG 多条记录是不是常见？怎么选对那一条？
2. 只扫 bus 0（+ 跟桥 + 跟 VMD）这种做法，在 AMD 平台上够不够？
   AMD 平台的设备会不会出现在更远的总线上？
3. **ExitBootServices 之后"设备枚举"这个词**：我们的枚举只在退出前做一次，
   退出后只是用已经注册好的块设备。这个理解对不对？
4. FAT32 卷上"前 N 次 Open 成功、之后全失败"，除了句柄泄漏和池耗尽，
   还有没有第三类原因（比如平台 FAT 驱动在 FAT32 路径上的某个上限）？
