# 0.3.2-M4-SP2.5 —— 文件系统接手完成

> 一句话：**驱动开发进了 SDK，NVMe 打通了写通路，文件系统从 FatFs 换成自己写的 FAT12/16/32。**

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

这一版的 FAT 层走过完整的一段路，最终形态是**自研驱动**。

**第一步：接 FatFs R0.16 进内核**，替掉只认 FAT16 的旧读取器 ——
它 FAT12/16/32 + 子目录 + 长文件名全都有，看起来是笔划算的买卖。

**第二步：发现它在这个用途下不可靠。** 症状是 TNX 程序小的能跑、大的必崩：

```
HELLO.TNX    9588 字节  正常
TREE.TNX     7556 字节  正常
STRESS.TNX  27888 字节  必崩
CJK16.FNT  855624 字节  读不到（中文静默退回 8x12 ASCII 点阵）
```

崩溃的样子是最误导人的那种：**一条不可能出错的指令报了 #GP**。
反汇编看那里明明是 `testq %rax,%rax`，它不会 fault。

取证靠的是 QEMU 的 `pmemsave`：`stop` 之后抓程序窗口的快照，和 TNX 映像逐字节比；
再在加载器里把**源**（从盘上读进来的缓冲区）和**目标**（映像）并排打出来：

```
tnx.src.f30  == tnx.dst.f30       <- 拷贝是忠实的
tnx.src.1f30 == tnx.dst.1f30
```

拷贝没问题，**是源本身就已经错了**。再拿源和磁盘上的文件比：

```
磁盘 file[0xFE0]:  00 00 48 89 DF E8 F6 08
内存 buf[0xFE0]:   89 45 F0 EB B9 8B 45 BC     <- 不一样
```

`f_read` 返回成功、`br` 等于文件长度、`FRESULT` 是 `FR_OK` —— **内容却是错的**。

**第三步：删掉，自己写。** 问题在移植层的簇链/扇区缓存/跨簇读取边界上，
而这一层本来只需要做一件事：按簇链把扇区读出来。

```
删掉    third_party/fatfs/ 整棵树（ff.c / ff.h / diskio.h / ffconf.h）
        third_party/fatfs/source/fatfs_glue.c / fatfs_tnd.h
        构建列表里的 ff.c / fatfs_glue.c
        发布打包脚本里的 FatFs 拷贝
写入    src/kernel/fat.c —— 1064 行，FAT12/16/32
收益    kernel.efi 139776 -> 134656 字节（少 5 KB）
        第三方组件少一个，THIRD-PARTY.md 里那条许可义务结束
```

自研驱动覆盖的能力和之前等价：

```
挂载      GPT -> MBR -> 探测；解析 BPB；类型由簇数决定（微软规范）
FAT 表    12 位紧密打包（含跨扇区）、16 位、32 位；读和写都支持
目录      簇链遍历、8.3 短名、LFN 项跳过、卷标项
读        fat_read_alloc / fat_read_text
写        fat_mkdir / fat_unlink / fat_rename / fat_copy
列表      复刻 cmd 的 dir 格式（卷标/序列号/日期/千位逗号/File(s)/Dir(s)）
枚举      fat_enum —— 给 api.c 的 findfirst/findnext 用
自检      fat_selftest：建 / 写 / 读回 / 比对 / 删
```

**只支持 512 字节扇区**，别的尺寸直接拒绝而不是猜测 —— 悄悄按 512 处理
会读出错位的数据，那比拒绝难查得多。

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

自研 NVMe 驱动，**读完整个规范里真正用得到的那一部分**：

```
控制器复位 -> 就绪等待 -> Admin 队列 -> Identify Controller / Namespace
-> 建 I/O 完成队列 / 提交队列 -> 读写命令
```

读写合成一个函数 `nvme_rw(opcode, ...)` —— 读 `0x02`、写 `0x01`，
其余字段完全一样。两个函数各抄一遍的话，改一处忘一处是迟早的事。

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

### 7 发行格式：img -> VHD

**从本版本开始，至具备 ISO 构建能力之前，不再以 `.img` 发行 Releases 成品，改为 VHD。**

```
之前    TNDDOS-<version>-x86_64.img    裸盘镜像，要 dd 到盘上或挂成回环
现在    TNDDOS-<version>-x86_64.vhd    VHD，Windows 双击就能挂载/卸载
```

内容一模一样（GPT + ESP + FAT），只是外面多了一层容器。
裸 img 在 Windows 上要额外的挂载工具，而 VHD 是系统自带就能处理的格式。

**什么时候改回去**：等有了 ISO 构建能力（El Torito + 我们自己的引导链），
发行物就应该是 ISO —— 那时候 VHD 只作为「装到盘上」的补充格式保留。

## 验证过的（QEMU）

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

**这一条不是「待修」，是「没有已知解」** —— 记在这里，免得下次又被当成 bug 查一遍。

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

主项目 MIT。第三方组件保留各自的许可证，逐条登记在 `docs/THIRD-PARTY.md`：

```
OpenBSD nvme.c     ISC   （已引入，未接入）
OpenBSD ahci.c     ISC   （已引入，未接入）
OpenBSD pciide.c   ISC   （已引入，未接入）
=====================================================================
FatFs R0.16        已从本版本移除 —— 不再分发，许可义务随之结束
```

版权声明原样保留，一个字未改。

## 相关文档

```
docs/MACHINE.md         目标机器档案（天选6 / Ryzen AI 9 HX 370 / ESP 是 FAT32）
docs/DECISIONS.md       四条架构决定
docs/TRANSITIONAL.md    过渡期代码约定 + 109 处盘点
docs/THIRD-PARTY.md     第三方代码与许可证
docs/I18N.md            写作与本地化约定
docs/FATFS.md           FAT 层：从 FatFs 到自研（取证过程 + 三个实现坑）
```

<details>
<summary>English</summary>

## What changed

**Driver development moved into the SDK.** Each driver is its own TNX artifact with
its own 1 MiB window slot; same-slot double loads are rejected.

**FatFs R0.16 was adopted, then removed within this version.** The port returned
wrong data for files spanning several clusters: `f_read` reported success, `br`
matched the file length, `FRESULT` was `FR_OK`, and the buffer still did not hold
the file. A 27 KB TNX image arrived corrupted, which is why a `testq %rax,%rax`
appeared to raise `#GP` -- the instruction was not there. The same fault hid a
836 KB CJK font. It is replaced by our own FAT12/16/32 driver
(`src/kernel/fat.c`, 1064 lines). `kernel.efi` drops from 139776 to 134656 bytes
and one third-party licence obligation ends.

**NVMe** has a complete read/write path: one `nvme_rw` serves both opcodes, only
PRP1 is filled, so each command stays inside one 4 KB page and the driver splits
work by 512 bytes.

**The console shadow update and scroll are atomic** (`cli`/`sti`, preserving the
incoming IF so trap context stays safe).

**The CJK font is read through our own FAT driver** via `t_font_read_hook`, because
the font loads after `ExitBootServices`, when the firmware file system is gone.

**Releases now ship as VHD, not img** -- from this version until we can build an ISO.

## Known limitation

Machines whose laptop vendor puts the disk behind a proprietary hardware RAID
(Intel RST, RAID On, OEM volume managers) are not supported -- there is no public
specification for those controllers. Switch the machine to AHCI / passthrough.
This is not a pending fix; it has no known solution.

</details>
