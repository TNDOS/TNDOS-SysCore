# 0.3.2-M4-SP2.5 —— 文件系统接手完成

> 一句话：**驱动开发进了 SDK，NVMe 打通了写通路，文件系统从 FatFs 换成自己写的 FAT12/16/32。**

<div>

**从本版本开始，至具备 ISO 构建能力之前，不再以 `.img` 发行 Releases 成品，改为 VHD。**

**何时改回去**：等有了 ISO 构建能力（El Torito + 我们自己的引导链），
发行物就应该是 ISO。届时的三档格式分工是：

```
ISO                   通用发行物 —— 刻盘、虚拟机、Ventoy 都能直接吃
多分区磁盘镜像 .img     Linux / BSD 系下便捷使用 —— 分区表和引导项都由我们写好
VHD                    Windows 下便捷使用 —— 双击挂载，不需要额外工具
```

**VHD 用动态扩展模式**，标称容量 64 MB，实际按占用增长；打包收尾时压缩一次，
把「分配过但全是零」的块还回去：

```
固定模式      67,109,376 字节   64 MB 全部实打实写出来
动态扩展       6,311,936 字节   只分配真正写过的块 —— 小 10.6 倍
```

`compact vdisk` 只对动态扩展的 vdisk 有效，而且必须是分离或只读挂载状态 ——
`tools/make-vhd.ps1` 里第二趟 diskpart 就是干这个的。
下载的人不用为那 58 MB 空白付代价。

</div>

## 这一版做了什么

### 1 驱动开发进入 SDK

TNX 驱动不再是内核里的一段代码，而是 SDK 里的独立产物：

```
repos/TNDOS-SDK/drivers/     每个驱动一个目录、一份源码
repos/TNDOS-SDK/tools/build-tnx.ps1   统一的编译/链接/打包参数
repos/TNDOS-SDK/linker/tnx.ld         TNX 的链接脚本
repos/TNDOS-SDK/third_party/          第三方代码，各自的许可证
```

驱动用 TNX 格式，**不走固件的 LoadImage** —— 这是能活过 ExitBootServices 的前提。

多驱动可以并存：每个驱动有自己的窗口槽（1 MiB 一格，8 格），
同槽重复加载会被内核拒绝。这解决过一个很隐蔽的崩溃：
所有驱动原来都链到同一个基址 `0x2000000`，加载第二个就把第一个整段覆盖，
症状是「调用第一个驱动的回调时跳进一段函数序言」（RIP 里能看到 `55 48 89 E5`）。

### 2 FAT 实现：接入 FatFs，然后把它删了

初始使用 FatFs 作为轮子，但是由它造成的事故调试了很久。

简而言之，就是 TNX 加载器从磁盘中加载时，由于文件大小的问题，
导致大量二进制区域错位 / 漏加载 / 重复写入。因此改用自己写的 FAT 驱动。

取证过程、三个实现坑、以及自研驱动的能力清单，见
[docs/FATFS.md](FATFS.md)。

```
删掉    third_party/fatfs/ 整棵树 + 构建列表里的引用 + 打包脚本里的拷贝
写入    src/kernel/fat.c —— 1064 行，FAT12/16/32
收益    kernel.efi 139776 -> 134656 字节（少 5 KB）
        第三方组件少一个，这条许可义务结束
```

`dir` 的输出照 `cmd` 的格式：

```
 Volume in drive C is TNDDOS
 Volume Serial Number is 0000-0000
 Directory of C:\EFI\TNDOS

2026/10/24  12:03    <DIR>          FONTS
2026/10/24  12:03            27,888 STRESS.TNX
               18 File(s)        246,509 bytes
                3 Dir(s)      60,571,648 bytes free
```

（序列号显示 0000-0000 是因为生成 FAT32 时没写 BPB 偏移 39 —— 纯显示问题。）

### 3 NVMe 实现

参照 [docs/THIRD-PARTY.md](THIRD-PARTY.md) 的「OpenBSD 驱动源码 —— 已获取，**未采用**」。

自研 NVMe 驱动，**读完整个规范里真正用得到的那一部分**：

```
控制器复位 -> 就绪等待 -> Admin 队列 -> Identify Controller / Namespace
-> 建 I/O 完成队列 / 提交队列 -> 读写命令
```

读写合成一个函数 `nvme_rw(opcode, ...)` —— 读 `0x02`、写 `0x01`，
其余字段完全一样。两个函数各抄一遍的话，改一处忘一处是迟早的事。

**这个驱动是自己写的，没有搬 OpenBSD 的 `nvme.c`。** 那个文件确实被引入过
（`third_party/openbsd/`，ISC，56 KB），但从来没接进构建，也一个字符都没抄：

```
我们的 drivers/nvme/nvme.c    29,240 字节
OpenBSD 的 nvme.c             56,134 字节
两边 40 字节窗口的重合数        0
```

六个驱动（ahci / nvme / ps2 / say / usb / vga）对三个 OpenBSD 文件的
40 字节窗口重合数**全部是 0**。抄了再改写做不到这个结果。
保留那三个文件只是为了读实现思路；源码里一条第三方版权声明都没有，
这本身就是「没用」的证据。

一个必须记住的约束：`nvme_rw` 只填 PRP1（PRP2 恒为 0），
所以一次命令的数据**必须落在同一个 4 KB 页内**。文件系统的缓冲不保证页对齐，
所以驱动层按 512 字节拆开循环 —— 单个扇区永远在页内。

### 4 写通路

```
NVMe 写命令 0x01
blk_write           只读设备返回 -4（「不能写」和「写失败」是两件事）
TND_BLOCK.Write     API v2.8，可以为 NULL
fat_write           自研 FAT 的写路径 -> blk_write
```

### 5 控制台：滚动和写格子做成原子

偶发过一个坏字符：屏幕上 `AUTOEXEC` 被打成 `OUTOEXEC`，而二进制里的字符串是完好的
（已逐字节验证）。机制是定时器中断正好落进「写 shadow → 画格子 → 挪光标」中间。

```c
irqf = t_irq_save();      // pushfq; popq; cli
...写 shadow / 画格子 / x++...
t_irq_restore(irqf);      // 只有原本 IF=1 才 sti
```

`fb_scroll` 也夹住了：`fb_write` 遇到换行会直接调它，绕过 `fb_put_cp`。
**必须保存原来的 IF** —— 这段代码也会在 trap 处理程序里跑，那时候不能把中断开回来。

### 6 字体走自己的文件系统

字体是在 `ExitBootServices` **之后**才加载的（`CONSOLE fb` 在 autoexec 里跑），
那时固件的 SimpleFileSystem 已经没了 —— 于是 836 KB 的中文字体读不到，
中文静默退回内置的 8x12 ASCII 点阵。

`font.c` 留在 LibSrc 里（引导器和内核都要编），不能直接依赖内核的 FAT 驱动，
所以加了一个函数指针 `t_font_read_hook`，内核在 `vfs_init` 里接上 `fat_read_alloc`。

```
[log] font: loaded \EFI\TNDOS\FONTS\CJK16.FNT  cell=16x8/16  glyphs=21427  bytes=855624
```

<details>
<summary>验证过的（QEMU）</summary>

```
C:\EFI\TNDOS>hello
  argc = 1   argv[0] = hello          argv is wired up -> OK
  stat(HELLO.TNX) -> OK               size = 9588
  read 255 bytes from AUTOEXEC.BAT; first line: @echo off
  open/read/close a real file -> OK
  [wide] 中文 = 4 cells (2 wide chars); A = 1 cell
  all checks passed

C:\EFI\TNDOS>stress
  [PASS] md5 of empty string / md5 of abc
  [PASS] aes128 FIPS-197 C.1 / decrypt round-trip / ctr round-trip
  [PASS] inflate no error / output length / content
  [PASS] inflate loop had no failures
  [PASS] heap churn kept contents
  ALL CHECKS PASSED

C:\EFI\TNDOS>constest
  patterned rows: 48   mismatching: 0
  ALL PATTERNED ROWS ARE PIXEL-IDENTICAL

  autoexec.bat 97 行全跑完；全程零 CPU 异常
```

`constest` 是这一版新加的控制台压力工具：用 16 周期花纹（`0123456789ABCDEF`）
刷 1800 行、再压 400 行中文，然后按像素逐行比对 —— 每一行都必须和参考行完全一致。
它不是「看起来没问题」，是**比出来的**。

发行用的 VHD 也直接引导验过一遍：`[batch done, 97 lines]`、字体 21427 字形、零异常。

</details>

## 已知限制

### 真实硬件：笔记本厂商自带的硬阵列没有解决方案

**这一版仍然只能在「盘能被固件直接看到」的机器上跑。**

```
能用     NVMe（无 RAID）、AHCI 模式、QEMU 的 nvme/ahci 设备
不能用   笔记本出厂默认的厂商硬阵列（Intel RST / RAID On / 各类 OEM 卷管理器）
症状     固件看得到盘，我们看不到 —— 块设备注册不了，FAT 挂不上
```

原因是那类阵列的管理接口是**厂商私有**的（有些还要求走 UEFI 驱动栈里的特定协议），
没有公开规范可依。现在的做法是不去碰它：把机器切到 AHCI / 直通模式即可。

**这一条不是「待修」，是「暂时没有已知解」** —— 记在这里，免得下次又被当成 bug 查一遍。

### 其他

```
· FAT 只解析 8.3 短名，LFN 项跳过 —— 显示长文件名要另外实现
· 自研盘上卷序列号显示 0000-0000（生成 FAT32 时没写 BPB 偏移 39）
· 写操作没有掉电一致性 —— 这不是日志式文件系统
· AHCI.TNX 加载失败，NVMe 路径不受影响
· IDE(PATA) 未移植（OpenBSD 的 pciide.c 已登记许可证，还没接）
· paging_install() 仍是桩：装自己的 CR3 之前要先处理 MMIO 的 cache 属性
```

## 许可证

参照 [docs/THIRD-PARTY.md](THIRD-PARTY.md)。

要点只有两条：

```
主项目 MIT
OpenBSD 的三个驱动源码随仓库分发，所以 ISC 登记必须留着（原文件头原样保留）——
但它们不参与构建、也没有任何代码被复制；六个驱动都是自己写的
FatFs R0.16 已移除 —— 不再分发，许可义务随之结束
```

<details>
<summary>相关文档</summary>

```
docs/MACHINE.md         目标机器档案（天选6 / Ryzen AI 9 HX 370 / ESP 是 FAT32）
docs/DECISIONS.md       四条架构决定
docs/TRANSITIONAL.md    过渡期代码约定 + 109 处盘点
docs/THIRD-PARTY.md     第三方代码与许可证（含 OpenBSD 未采用的比对数据）
docs/I18N.md            写作与本地化约定
docs/FATFS.md           FAT 层：从 FatFs 到自研（取证过程 + 三个实现坑）
docs/RELEASES.md        各版本发布说明 + 发行格式约定
```

</details>

<details>
<summary>English</summary>

# 0.3.2-M4-SP2.5 -- the file system is ours

> In one line: **driver development moved into the SDK, NVMe gained a write path,
> and the file system went from FatFs to a FAT12/16/32 driver we wrote.**

<div>

**From this version on, and until an ISO build exists, Releases ship a `.vhd`
instead of a raw `.img`.**

**When this is reverted**: once an ISO build exists (El Torito plus our own boot
chain) the release artifact should be an ISO. At that point the three formats
split like this:

```
ISO                   the universal artifact -- burn it, boot it in a VM, feed it
                      to Ventoy
multi-partition .img  the convenient form under Linux / BSD -- we write the
                      partition table and the boot entries ourselves
VHD                   the convenient form under Windows -- double-click to mount,
                      no extra tooling
```

**The VHD is dynamically expanding**: nominal capacity 64 MB, actual size grows
with usage. The packaging step finishes with a compaction pass that returns the
blocks which were allocated but are entirely zero:

```
fixed layout      67,109,376 bytes   64 MB written out in full
dynamically sized  6,311,936 bytes   only the blocks actually written -- 10.6x smaller
```

`compact vdisk` only works on an expandable vdisk, and only while it is detached
or attached read-only -- which is what the second diskpart pass in
`tools/make-vhd.ps1` exists for. Nobody has to download 58 MB of empty space.

</div>

## What this version does

### 1 Driver development moves into the SDK

A TNX driver is no longer a block of code inside the kernel; it is a separate
artifact built by the SDK:

```
repos/TNDOS-SDK/drivers/               one directory and one source file per driver
repos/TNDOS-SDK/tools/build-tnx.ps1    shared compile / link / pack arguments
repos/TNDOS-SDK/linker/tnx.ld          the TNX linker script
repos/TNDOS-SDK/third_party/           third-party code, each with its own licence
```

Drivers use the TNX format and **do not go through the firmware's LoadImage** --
that is the precondition for surviving ExitBootServices.

Several drivers can coexist: each gets its own 1 MiB window slot (eight slots),
and loading into an occupied slot is rejected. This fixed a very quiet crash:
every driver used to link at the same base `0x2000000`, so loading a second one
overwrote the first entirely. The symptom was "calling the first driver's callback
jumps into a function prologue" -- `55 48 89 E5` visible at the RIP.

### 2 FAT: FatFs was adopted, then deleted

FatFs was used as a wheel at first, and the accident it caused took a long time
to debug.

In short: when the TNX loader read an image from disk, the file size mattered --
large parts of the binary ended up shifted, skipped, or written twice. So the FAT
driver was replaced with one of our own.

The evidence trail, the three implementation traps, and the capability list of the
new driver are in [docs/FATFS.md](FATFS.md).

```
removed   the whole third_party/fatfs tree, its entries in the build list, and the
          copy step in the release packaging
added     src/kernel/fat.c -- 1064 lines, FAT12/16/32
effect    kernel.efi 139776 -> 134656 bytes (5 KB smaller)
          one fewer third-party component; its licence obligation ends
```

`dir` follows cmd's layout:

```
 Volume in drive C is TNDDOS
 Volume Serial Number is 0000-0000
 Directory of C:\EFI\TNDOS

2026/10/24  12:03    <DIR>          FONTS
2026/10/24  12:03            27,888 STRESS.TNX
               18 File(s)        246,509 bytes
                3 Dir(s)      60,571,648 bytes free
```

(The serial reads 0000-0000 because the FAT32 image generator does not write BPB
offset 39. It is a display-only issue.)

### 3 NVMe

See "OpenBSD driver sources -- obtained, **not used**" in
[docs/THIRD-PARTY.md](THIRD-PARTY.md).

A driver of our own, built from **the part of the specification that is actually
used**:

```
controller reset -> wait for ready -> admin queues
-> Identify Controller / Namespace
-> create I/O completion and submission queues -> read and write commands
```

Read and write share one function, `nvme_rw(opcode, ...)`: read is `0x02`, write
is `0x01`, every other field is identical. Two copies of that code would drift
the first time one of them is touched.

**This driver is written from scratch; OpenBSD's `nvme.c` was not copied.** That
file was vendored once (`third_party/openbsd/`, ISC, 56 KB) but was never wired
into any build and not a single character was taken from it:

```
our drivers/nvme/nvme.c       29,240 bytes
OpenBSD's nvme.c              56,134 bytes
shared 40-byte windows              0
```

Across all six drivers (ahci / nvme / ps2 / say / usb / vga) the count of shared
40-byte windows against the three OpenBSD files is **zero**. You cannot reach that
result by copying and then rewriting. Those files are kept only to read how the
enumeration is done; there is not one third-party notice anywhere in the driver
sources, which is itself the evidence that nothing was used.

One constraint worth remembering: `nvme_rw` fills PRP1 only (PRP2 is always 0),
so the data of a single command **must stay inside one 4 KB page**. File system
buffers are not page aligned, so the driver splits work into 512-byte chunks --
a single sector is always inside one page.

### 4 The write path

```
NVMe write command 0x01
blk_write           a read-only device returns -4 ("cannot write" and "write
                    failed" are two different answers)
TND_BLOCK.Write     API v2.8, may be NULL
fat_write           our own FAT write path -> blk_write
```

### 5 Console: the scroll and the cell write are now atomic

A garbled character showed up once: `AUTOEXEC` was printed as `OUTOEXEC`, while
the string inside the binary was intact (verified byte by byte). The mechanism is
a timer interrupt landing exactly inside "write the shadow cell -> blit the glyph
-> advance the cursor".

```c
irqf = t_irq_save();      // pushfq; popq; cli
...shadow write / glyph blit / x++...
t_irq_restore(irqf);      // sti only if IF was set on entry
```

`fb_scroll` is guarded too: `fb_write` calls it directly on a newline, bypassing
`fb_put_cp`. **The incoming IF must be preserved** -- this code also runs inside
the trap handler, and re-enabling interrupts there would corrupt the context.

### 6 The font reads through our own file system

The font is loaded **after** `ExitBootServices` (`CONSOLE fb` runs from autoexec),
by which time the firmware's SimpleFileSystem no longer exists -- so the 836 KB
CJK font could not be read, and Chinese silently fell back to the built-in 8x12
ASCII glyphs.

`font.c` lives in LibSrc (both the bootloader and the kernel compile it), so it
cannot depend on the kernel's FAT driver directly. It takes a function pointer,
`t_font_read_hook`, which the kernel points at `fat_read_alloc` from `vfs_init`.

```
[log] font: loaded \EFI\TNDOS\FONTS\CJK16.FNT  cell=16x8/16  glyphs=21427  bytes=855624
```

<details>
<summary>Verified (QEMU)</summary>

```
C:\EFI\TNDOS>hello
  argc = 1   argv[0] = hello          argv is wired up -> OK
  stat(HELLO.TNX) -> OK               size = 9588
  read 255 bytes from AUTOEXEC.BAT; first line: @echo off
  open/read/close a real file -> OK
  [wide] 4 cells for two wide characters; A = 1 cell
  all checks passed

C:\EFI\TNDOS>stress
  [PASS] md5 of empty string / md5 of abc
  [PASS] aes128 FIPS-197 C.1 / decrypt round-trip / ctr round-trip
  [PASS] inflate no error / output length / content
  [PASS] inflate loop had no failures
  [PASS] heap churn kept contents
  ALL CHECKS PASSED

C:\EFI\TNDOS>constest
  patterned rows: 48   mismatching: 0
  ALL PATTERNED ROWS ARE PIXEL-IDENTICAL

  autoexec.bat completes all 97 lines; zero CPU exceptions throughout
```

`constest` is the console stress tool added in this version: it floods the screen
with a period-16 pattern (`0123456789ABCDEF`) for 1800 lines, then 400 lines of
CJK, and finally compares every visible row pixel by pixel -- each one must match
the reference row exactly. It does not say "looks fine", it **measures**.

The release VHD was booted as well: `[batch done, 97 lines]`, 21427 glyphs of font
loaded, zero exceptions.

</details>

## Known limitations

### Real hardware: a laptop vendor's hardware RAID has no solution

**This version still only runs on machines where the firmware can see the disk
directly.**

```
works     NVMe without RAID, AHCI mode, QEMU's nvme/ahci devices
fails     the vendor hardware RAID most laptops ship with by default
          (Intel RST, RAID On, various OEM volume managers)
symptom   the firmware sees the disk, we do not -- no block device registers and
          the FAT volume never mounts
```

The management interface of those arrays is **proprietary** (some also require a
specific protocol inside the UEFI driver stack), with no public specification to
follow. The answer today is to leave them alone: switch the machine to AHCI or
passthrough mode.

**This is not a pending fix, it is "no known solution for now"** -- recorded here
so the next person does not spend another night treating it as a bug.

### Other

```
- FAT parses 8.3 short names only; LFN entries are skipped, so long file names
  would need separate work
- the volume serial shows 0000-0000 on our own images (the FAT32 generator does
  not write BPB offset 39)
- writes have no power-loss consistency; this is not a journalling file system
- AHCI.TNX fails to load; the NVMe path is unaffected
- IDE (PATA) is not ported (OpenBSD's pciide.c is registered in the licence
  inventory but not wired in)
- paging_install() is still a stub: installing our own CR3 needs MMIO cache
  attribute handling first
```

## Licences

See [docs/THIRD-PARTY.md](THIRD-PARTY.md).

Only two points matter:

```
the project itself is MIT
the three OpenBSD driver sources ship with the repository, so their ISC entry has
to stay (original headers untouched) -- but they are not built and no code was
copied from them; all six drivers are ours
FatFs R0.16 was removed -- no longer distributed, so its licence obligation ends
```

<details>
<summary>Related documents</summary>

```
docs/MACHINE.md         target machine profile (ASUS TUF A16 / Ryzen AI 9 HX 370
                        / the ESP is FAT32)
docs/DECISIONS.md       the four architectural decisions
docs/TRANSITIONAL.md    transitional-code rules and the 109-item inventory
docs/THIRD-PARTY.md     third-party code and licences (including the OpenBSD
                        comparison numbers)
docs/I18N.md            writing and localisation rules
docs/FATFS.md           the FAT layer: from FatFs to our own (evidence trail and
                        the three implementation traps)
docs/RELEASES.md        per-version release notes and the artifact format rule
```

</details>

</details>