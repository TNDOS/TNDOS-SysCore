# 目标机器（真机）

写于 2026-10-18。**这份信息一开始就该有** —— 之前靠猜"厂商硬阵列 / Intel VMD"，
猜错了整整两天。查它只需要一条命令。

## 硬件

```
OEM      ASUSTeK COMPUTER INC.
型号     TX Gaming FA608WM_FA608WM      （华硕 天选6 / TUF Gaming A16）
BIOS     FA608WM.304   (American Megatrends International)
CPU      AMD Ryzen AI 9 HX 370           **Zen 5，2024 年底的新平台**
内存     15.1 GB
固件     UEFI
```

## 存储

```
Windows 看到的：disk0  SAMSUNG MZVL81T0HELB-00BTW  NVMe  953.9GB  GPT
用户说：       物理上是两块 1TB（RAID 1 镜像）

驱动：         Standard NVM Express Controller / stornvme.inf
                （微软原生 NVMe 驱动）
               **没有** AMD RAID 控制器、iaStorVD、RAIDXpert
```

## 分区（disk0，GPT，9 个分区）

```
part1  System      260MB   -> H:    **ESP**，FAT32，已用约 105MB
part2  Reserved     16MB   -> MSR
part3  Basic    201827MB   -> C:    Windows
part4  Recovery    886MB
part5  Basic    476160MB   -> D:
part6  Basic    262852MB   -> E:
part7  Basic      5825MB   -> F:
part8  Basic     28672MB
part9  Recovery    260MB
```

## 由此推出的三条（都很重要）

**一 不是 Intel VMD。** AMD 平台。而且 Windows 用的是原生 stornvme，
没有看到任何 RAID 控制器 —— 说明那块盘对操作系统**就是一个标准 NVMe 名字空间**。

**二 所以我们的 NVMe 驱动本该也能看见它。** 看不见 -> 问题在 **PCI 枚举**这一层，
而不是"盘被藏起来了"。这比 VMD 那条好查得多。

**三 这是 Zen 5 新平台。** ECAM / MCFG 的形态不能假定和 OVMF 一样：

```
· MCFG 可能有**多条**记录，第一条未必覆盖 bus 0
· 我们只取了第一条，而且只扫一条总线
· 新平台上的 PCI 段 / 总线号分配也可能不同
```

**PCILIST 就是为这条准备的** —— 它把枚举结果原样列出来，一屏就能看出
"是没看见"还是"看见了没接住"。

## ESP 是 FAT32，不是 FAT16

品牌机一律如此。而我们的 FAT16 读取器只认 FAT16 ——
所以退出之后的 dir 就算找到卷也读不了。

**这也是为什么所有文件系统相关的症状只在真机出现、QEMU 一次都不复现。**

## 一条纪律

**先查机器，再猜故障。** 这台机器的型号 / CPU / 存储控制器，
用一条 `Get-CimInstance Win32_ComputerSystem` 就能拿到。
