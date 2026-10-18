# 过渡期代码 Transitional code

2026-10-18 作者裁定。 Author's ruling.

## 一 什么是过渡期代码 What counts as transitional

**所有依赖 UEFI Boot Services 的代码，都是过渡期代码。** Every piece of code that depends on UEFI Boot Services is transitional.

理由：这些东西在最终形态里**必须消失**，因为它们的生死由固件决定。 Reason: they must disappear in the final shape, because the firmware decides their lifetime.

```
· 用 SimpleFileSystem 读写文件         -> 换成我们自己的 FAT
· 用 LoadImage 加载 .EFI 驱动          -> 换成 TNX 加载器
· 用 HandleProtocol 找协议             -> 换成我们自己的设备表
· 用 AllocatePool / AllocatePages      -> 已经换成 pmm / 堆（这一条已完成）
· 用 GetMemoryMap / ExitBootServices   -> 保持，但只在启动的最后一步
```

## 二 怎么标记 How to mark it

**统一前缀，能 grep。** A single prefix, greppable.

```c
/* TND-TRANSITIONAL: <这块在干什么> —— 去掉的条件：<什么做完就能删> */
```

`TND-TRANSITIONAL` 是固定字符串，**不要改写法**。 `TND-TRANSITIONAL` is a fixed string; never vary it.

例： For example:

```c
/* TND-TRANSITIONAL: 用固件的 SimpleFileSystem 读配置文件。
 * 去掉的条件：vfs 全部走 blk + FatFs，不再有固件路径。 */
s = gEnv.Root->Open(gEnv.Root, &f, wname, EFI_FILE_MODE_READ, 0);
```

## 三 查有多少 How to count

```
grep -c TND-TRANSITIONAL src/**/*.c
```

**这个数字应该只能变小。** The number should only ever go down.

如果某次提交让它变大，提交信息里必须说明为什么。 If a commit increases it, the commit message must say why.

## 四 现在有多少 Current count

（每次改动后更新；数字由 `grep -c` 得出，不手填估计值）
(Updated after each change; the number comes from `grep -c`, never estimated by hand)

**2026-10-18 首次盘点的结果：** First audit, 2026-10-18:

```
UEFI 依赖点（gEnv.BS-> / gEnv.Root-> / LoadImage / HandleProtocol / Allocate*）：
  boot\bootx64.c      14
  kernel\kernel.c     18
  kernel\drv.c        17
  kernel\vfs.c        12
  lib\file.c          12
  kernel\pmm.c        10
  kernel\shell.c       6
  kernel\pci.c         5
  kernel\api.c         2
  kernel\tnx.c         2
  lib\con_fb.c         4
  lib\log.c            2
  lib\time.c           2
  lib\util.c           2
  kernel\conf.c        1
  ------------------------
  合计 109 处

已加 TND-TRANSITIONAL 标记：**5 处**（都在 lib/file.c）
```

**109 是起点，这个数字只能变小。** 109 is the starting point; it may only go down.

为什么先标 `lib/file.c`：它是最干净的一处 —— 整个文件都靠 SimpleFileSystem，
换掉之后**整个文件删掉**，不需要改造成别的东西。先拿它把标记的写法立起来。
(Why `lib/file.c` first: it is the cleanest target -- the whole file depends on
SimpleFileSystem, so when it is replaced the file is deleted outright rather than
reworked. It establishes the marking style.)

## 五 移除的顺序 Removal order

**从最底层的开始删，删一层验一层。** Start from the lowest layer, verify after each removal.

```
1  LoadImage / .EFI 驱动        <- 换成 TNX（src/drv 退役时一起走）
2  HandleProtocol 找协议         <- 换成中央协议表
3  SimpleFileSystem 读写文件     <- 换成 blk + FatFs
4  ExitBootServices 本身         <- **保留**，它不是过渡期代码，是最终形态的一部分
```

**注意第 4 条**：`ExitBootServices` 不在移除清单里。 Note item 4: ExitBootServices is not on the removal list.
它恰恰是这个项目存在的理由 —— 交还 Boot Services 之后机器还能用。 It is the reason this project exists: the machine still works after handing Boot Services back.

## 五之二 第三方轮子**不是**过渡期代码 Third-party wheels are NOT transitional

这一条要单独写清楚，否则会误伤。 This deserves its own section, or it will cause the wrong code to be deleted.

```
FatFs R0.16（third_party/fatfs/）           永久保留
OpenBSD nvme.c / ahci.c / pciide.c          永久保留

它们不依赖 UEFI Boot Services，退出前后都能用。
它们是我们特意搬来的轮子 —— **搬来就是为了不用再删**。
```

**过渡期的是"胶水"，不是"轮子"。** What is transitional is the glue, not the wheel.

```
过渡期：我们自己写的、绕不过固件的那几层
        lib/file.c        用 SimpleFileSystem 读文件
        kernel/vfs.c      文件路径解析走固件
        kernel/drv.c      LoadImage 加载 .EFI
        kernel/pci.c      用 ACPI/MCFG 找 ECAM

永久：  轮子本身
        FatFs            文件系统
        OpenBSD 驱动      块设备
        我们自己的 blk   块设备抽象（不是胶水，是我们的层）
        我们自己的 pmm / 堆
```

**判断标准**：去掉 UEFI 之后它还能不能活。 The test is simple: does it still work once UEFI is gone?

能活的，是轮子，留着。 If it survives, it is a wheel -- keep it.
不能活的，是胶水，迟早删。 If it does not, it is glue -- scheduled for removal.

## 六 一次具体判断 A concrete judgement

「用 GOP 拿帧缓冲参数」算不算过渡期代码？ Is reading framebuffer parameters via GOP transitional?

**算，但只需要在启动时做一次，而且拿到的是普通数字。** Yes, but it happens once at boot and yields plain numbers.

```
GOP 协议       退出后失效        -> 过渡期
Base/Stride/分辨率   退出后仍有效  -> 不是过渡期，是我们的数据
```

所以：**调用 GOP 的那几行要标记，用完之后的像素写入不标记。** So: tag the lines that call GOP, not the pixel writes that follow.
