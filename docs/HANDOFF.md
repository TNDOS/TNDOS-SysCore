# 交接：明早从这里继续

写于 2026-10-18 00:41（上次真机测试之后）。

## 现在能用的东西

```
exitboot  -> Boot Services 交还给固件之后，机器仍然可用
             [boot_exit] A B C D  -> ** BOOT SERVICES ARE GONE **
             ---- takeover self-check ----
               block devices : N 名字
               fat           : mounted
               === root directory, read through blk + fat (no firmware) ===
```

**QEMU 上全通**（NVMe 挂 ESP，PS2 + NVMe 驱动并存，dir 不碰固件）。
真机上分两种情况，见下。

## 明早要试的三件事（按顺序）

### 1  VMD 是否真的解决了「no block device at all」

真机是笔记本 + 厂商硬阵列（Intel RST/VMD）。VMD 把 NVMe 控制器藏在
自己那段总线后面，而原来的 `pci_scan` 只扫 bus 0 —— 所以一个都找不到，
屏幕上连 `[nvme]` 都没有。

已改成：找 class `0x0104` 的设备，读它配置空间 0x40（VMCAP）拿总线范围，
只扫那一段（不盲扫，别名问题是真的）。

**要看的两行**：
```
[log] pci: VMD xx covers buses A..B -- scanning them
[log] blk: registered NVME0 ...
```

- 有 VMD 那一行、但没有 blk  -> VMCAP 的位定义要校准（厂商不一致）
- 连 VMD 那一行都没有          -> 那台机器不是 VMD，是别的东西藏了盘

### 2  滚屏速度

两刀都下了：开机自动开 write-combining + 滚屏改用 `movnti`（非临时存储，
不需要 MTRR 权限）。QEMU 上 `fb.wc = 0`（MTRR 被拒），所以主要靠 movnti。

**要看的一行**：
```
  write-combining : ON  (scroll is fast now)
  write-combining : ** refused, code N ** -- scrolling will be slow
```
N 告诉我 MTRR 为什么拒绝（`FBWC` 命令会打细节）。

### 3  cd / type 还是被挡住的

用户明确要求「CD 啥的也别拦截，全部挂到新模块」。方向对，但需要 FAT 支持
**子目录 / 读文件**，而现在只做得到列根目录。

**这件事已经做完了（0.3.2-M4-SP2.5）。** 但走的路和当初的计划不一样：

```
当初计划  换 FatFs（ChaN R0.15）—— 一次拿到 子目录 + 长文件名 + FAT32 + 写
实际做法  先真换了 FatFs R0.16，发现它在跨簇读取时交出错数据，导致 TNX 映像被写坏，
          于是又把它删掉，改写成自己的 FAT12/16/32（src/kernel/fat.c，1064 行）
结果      cd / type / dir / 子目录 / FAT32 / 写，全部可用；vfs_* 已经全接到自研驱动上
```

注意：真机的 ESP **通常是 FAT32** —— 自研驱动按簇数判定 FAT12/16/32，这点已经覆盖。

**教训**：换轮子省下来的时间，可能要用几倍的时间去查它造成的故障。
证据链见 [FATFS.md](FATFS.md)。

## 已知未修（按重要性）

```
1  构建不是原子的
   build-run.ps1 先 Remove-Item 整个 $Esp 再重建。中途失败 -> ESP 留在
   半成品，而它看起来正常（kernel.efi 在）。
   已经咬过两次：一次是我改坏 ahci.c，一次是瞬时失败 + 我的测试脚本
   把构建输出吞了（Out-Null）。
   修法：先建到临时目录，全部成功再整体换上去；脚本不许吞构建输出。

2  FAT32 不支持（真机 ESP 大概率是 FAT32）
3  IDE（PATA）驱动还没写
4  fb 控制台行距偏挤（不影响功能）
5  USB HID 还没接进输入后端
6  两套驱动模型并存（src/drv/*.c 的 .EFI 和 SDK 的 TNX），旧的该退役
```

## 一条方法论（这轮学到的，值得留着）

**要观测、又要不改变被测对象**：EDK2 的 MapKey 是「每次内存图变动就自增」，
而 `log_puts` 走固件 SerialIo 协议**会分配内存** —— 在 GetMemoryMap 和
ExitBootServices 之间打印，key 立刻失效。

解法是 `con_puts`（fb 后端，只画像素不分配）打进度标记：

```
[boot_exit] A  取完大小   B  池分配好了   C  拿到最终 key   D  EBS 回来了
```

换成它之后一次全过。**以后遇到"看起来卡住"的地方，先找一个不产生副作用的
观测手段，再动手改代码。**

## 仓库状态

核心 TNDOS、TNDOS-SDK 两个仓库都已提交、干净、**未推送**。
