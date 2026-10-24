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

## 五之二 轮子是脚手架，胶水可能长期存在 Wheels are scaffolding; glue may stay

**2026-10-18 作者更正。** Corrected by the author, 2026-10-18.
这一节我第一版写反了，把"轮子永久、胶水过渡"当成了结论。 I got this backwards in the first version, treating wheels as permanent and glue as temporary.

### 轮子**是过渡期代码** Wheels ARE transitional

```
FatFs R0.16                    -> **已删除**（0.3.2-M4-SP2.5）：它交出错数据，
                                  换成了自研 FAT12/16/32。见 FATFS.md
OpenBSD nvme.c / ahci.c / pciide.c  -> 脚手架，我们理解机制之后自己写
```

搬它们的目的**不是"以后就一直用它"**，是两条： The point of importing them is not "use them forever". It is two things:

```
1  让功能先跑起来，验证整条链是对的
2  **看清人家怎么实现的** —— 然后用自己的代码实现同一件事
```

这和决定四一致："复用的是咋运行的，不是模型复用、代码直接复用，得看人家咋实现的。"
This matches decision four: reuse how it works, not the model and not the code.

**所以轮子是脚手架。脚手架是要拆的。** The wheels are scaffolding. Scaffolding gets taken down.

### 胶水**不一定是过渡期代码** Glue is not necessarily transitional

```
lib/file.c      SimpleFileSystem 读文件      -> 要换，因为退出后要工作
kernel/vfs.c    路径解析走固件                -> 要换，同理
kernel/drv.c    LoadImage 加载 .EFI           -> 要换，同理
kernel/pci.c    ACPI/MCFG 找 ECAM             -> **可能长期保留**
```

**为什么 pci.c 不一样**：UEFI 是平台的一部分，不是我们要甩掉的包袱。 Why is pci.c different: UEFI is part of the platform, not baggage to be thrown off.

只要还有新的标准、新的设备要靠 UEFI 提供（ACPI 表、SMBIOS、RTC、安全启动状态……），
这一层就得在。 As long as new standards and new devices are surfaced through UEFI
(ACPI tables, SMBIOS, RTC, secure boot state...), this layer has to exist.

**它不是"过渡期"，它是我们的接口层。** It is not a transitional layer; it is our interface layer.

### 判断标准 The test

**不是"去掉 UEFI 之后还能不能活"。** Not "does it survive without UEFI".

**是"ExitBootServices 之后它还要不要工作"。** It is "does it still have to work after ExitBootServices".

```
还要工作   -> 必须是我们的，或者至少不依赖固件协议
不再需要   -> 可以是固件给的，也可以是脚手架
```

按这条重排： Re-sorted by that test:

```
退出后必须工作，所以最终必须是我们的：
  blk 层、文件系统、块设备驱动、输入驱动、控制台、内存管理、时基

退出后才开始需要，但入口在退出前：
  ACPI 表解析（pci.c 的 ECAM）-> **保留，可能长期**
  帧缓冲参数（GOP 一次调用，拿到数字）-> 保留

退出后就再也用不到：
  不可用（这个项目里不存在这种东西）
```

**注意：没有"因为用了固件所以必须删"这种规则。** Note: there is no rule that says "it used the firmware, therefore it must go".
规则是"退出之后还要不要工作"。 The rule is "does it still have to work after exit".

## 六 一次具体判断 A concrete judgement

「用 GOP 拿帧缓冲参数」算不算过渡期代码？ Is reading framebuffer parameters via GOP transitional?

**算，但只需要在启动时做一次，而且拿到的是普通数字。** Yes, but it happens once at boot and yields plain numbers.

```
GOP 协议       退出后失效        -> 过渡期
Base/Stride/分辨率   退出后仍有效  -> 不是过渡期，是我们的数据
```

所以：**调用 GOP 的那几行要标记，用完之后的像素写入不标记。** So: tag the lines that call GOP, not the pixel writes that follow.
