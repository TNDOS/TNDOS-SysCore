# 故障描述（给第二个人看）

写于 2026-10-18。这一份刻意写成"别人接手也能看懂"的形状 ——
因为写的过程中我自己就发现数错了一次。

## 一句话

UEFI 应用通过 EFI_SIMPLE_FILE_SYSTEM 读 ESP 上的文件，**真机上第 6 次打开开始全失败**，
QEMU 上同一个构建、同一个顺序，完全正常。

## 环境

```
平台     x86-64 UEFI
固件     OVMF（QEMU）/ 笔记本厂商固件（真机）
我们的程序  自己的内核，PE32+ UEFI 应用，在 \EFI\BOOT\BOOTX64.EFI
失败路径上**不涉及** ExitBootServices（这一步在更后面）
真机存储    2 × Samsung MZVL81T0HELB（1TB），RST/VMD RAID1
             Windows 只看到一块 953.9GB 的 disk0，第二块完全隐藏
ESP         FAT32, 256MB, 已用约 105MB, GPT
```

## 我们的做法

```c
HandleProtocol(LoadedImage) -> DeviceHandle
HandleProtocol(DeviceHandle, SimpleFileSystem) -> sfs
sfs->OpenVolume(&root)
/* 每个配置文件： */
root->Open(path, EFI_FILE_MODE_READ, 0, &f)
loop f->Read(&want /*512*/, buf) until short
f->Close(f)
```

## 症状（真机；QEMU 不复现）

```
第 1 次  efidos.sys      1864 B   OK
第 2 次  PS2.TNX        10976 B   OK
第 3 次  PS2.TNX        10976 B   OK   （TNX 加载器会再读一次）
第 4 次  NVME.TNX       13696 B   OK
第 5 次  NVME.TNX       13696 B   OK
第 6 次  AHCI.TNX       10560 B   FAIL —— Open 成功但 Read 第一次就返回 0 字节
第 7 次  config.sys       880 B   FAIL —— 同样 0 字节
第 8 次  autoexec.bat    2783 B   FAIL —— 这次 Open 本身就失败
```

**失败的只是"第 6 个"** —— 把顺序换一下，就换个文件倒霉。
文件大小无关：失败的那个 10.5KB，比先成功的 13.7KB 还小。

## 已经排除的

```
路径/文件名   同一个目录、同样的 8.3 名字，前面几个都能开
文件大小      见上
文件本身      在 Windows 资源管理器里逐个核对过，一个不少，大小全对
FAT 布局      同一个卷、同一段目录
权限          Windows 访问 H: 有权限限制，但那是 Windows 侧的；固件读得到
```

## 三个假设（还没分开）

```
A  我们自己泄漏了 EFI_FILE_PROTOCOL 句柄（某个分支没 Close）
B  池/堆耗尽（每次读取都分配，某处没释放）
C  平台的 FAT32 驱动在 N 次打开后撞到内部上限
```

## 已经加的探针

`file.c` 的 `t_read_file` 现在每次都打：

```
[log] file#6 open \EFI\TNDOS\AHCI.TNX
[log] file# open FAILED: ...  status=<EFI_STATUS>
```

**看序号一直涨、第 6 次开始失败 -> A 或 C。**
**看序号正常、失败的次数不一样 -> 和次数无关，是别的。**

## QEMU 为什么不复现

不知道。可能的差别：

```
· QEMU 的 ESP 是 FAT16（我们自己造的 32MB 镜像），真机是 FAT32
  -> 两条完全不同的固件代码路径（FatPkg 内部 FAT12/16 与 FAT32 是分开的）
· 真机的卷上有一百多 MB 的文件，QEMU 上只有 1MB
· 真机是 RST RAID1 卷，QEMU 是裸 NVMe 名字空间
```

**第一条最可疑**：我们的 FAT16 路径从来没在 FAT32 上跑过。
