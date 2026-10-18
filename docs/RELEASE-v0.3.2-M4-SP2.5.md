# 0.3.2-M4-SP2.5 —— 文件系统接手完成

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
症状是"调用第一个驱动的回调时跳进一段函数序言"（RIP 里能看到 `55 48 89 E5`）。

### 2 FAT 实现

**FatFs R0.16 进内核**，替掉自研的 FAT16 读取器。

- `ff.c` 一个字未改，接合层在 `third_party/fatfs/source/fatfs_glue.c`
- 只改 `ffconf.h` 的六项配置（READONLY / CODE_PAGE / RPATH / NORTC / LFN / LABEL）
- 对外接口不变：`fat_mount` / `fat_report` / `fat_list` / `fat_selftest`
- FAT12/16/32 + 子目录 + `f_chdir` / `f_getcwd` 全部可用

`dir` 的输出照 `cmd` 的格式重写：卷标、卷序列号（BPB 偏移 39 的真实值）、
日期时间（FAT 目录项的原始值）、千分位、右对齐、剩余空间。

```
 Volume in drive C is TNDDOS
 Volume Serial Number is 0B7C-0B7C
 Directory of C:.

2026/10/18  16:19    <DIR>          DRIVERS
2026/10/01  18:35             6,416 ATTRIB.TNX
2026/10/18  16:19           129,024 KERNEL.EFI
              16 File(s)        199,792 bytes
               2 Dir(s)      60,628,480 bytes free
```

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
所以驱动层按 512 字节拆开循环——单个扇区永远在页内。

### 4 写通路

```
NVMe 写命令 0x01
blk_write           只读设备返回 -4（"不能写"和"写失败"是两件事）
TND_BLOCK.Write     API v2.8，可以为 NULL
disk_write          FatFs 接合层 -> blk_write
FF_FS_READONLY=0
```

## 验证过的（QEMU，真 FAT32 卷，Windows diskpart 格式化）

```
退出前：dir 正常列出 16 个文件 + 2 个目录
退出后：同一条 dir 也正常 —— 走的是我们自己的 blk + FatFs，不碰固件

[FATWRITE] create + write + read back + compare
1 create              OK
2 write               53 bytes
3 close               OK
4 read back           53 bytes
5 compare             ** MATCH **
6 directory now: ... TNDTEST.TXT  53
RESULT: WRITE PATH OK
```

**第 5 步的 MATCH 和第 6 步重新列出来的文件，是这一版的核心证据** ——
写进去、读回来、逐字节一样、而且真的进了文件系统而不是只在缓存里。

## 已知问题

### autoexec.bat 开机不执行（**重要**）

开机时 `efidos.sys` 读得到（环境变量都设上了），但 `autoexec.bat`
和 `config.sys` 读不到，屏幕上显示 `(no autoexec.bat, skipped)`。

**影响**：开机脚本不自动跑，用户配置不生效。
**不影响**：内核启动、shell 可用、`dir` / `cd` / `fatwrite` / `exitboot` 全部正常。

现象是"前面几个文件能打开、从某一个开始全失败"，顺序相关。
已加探针：串口里每次打开都打 `[log] file#N open <path>`，失败时打 EFI_STATUS。
下一版从这里查。

### dir 的表头路径

`Directory of C:.` 而不是 `C:\EFI\TNDOS`。
FatFs 的 `f_getcwd` 返回相对当前驱动器的 `.`。**列出的内容是对的**，只是表头。

### AHCI.TNX 加载失败

`LOAD-FAIL AHCI.TNX  0 bytes`。NVMe 路径不受影响。

### IDE(PATA) 未移植

OpenBSD 的 `pciide.c` 已下载并在许可证清单里登记，还没接进来。
这是 QEMU 的刚需，不是实体机的刚需。

## 许可证

主项目 MIT。第三方组件保留各自的许可证，逐条登记在 `docs/THIRD-PARTY.md`：

```
FatFs R0.16        ChaN, BSD 风格
OpenBSD nvme.c     ISC   （已引入，未接入）
OpenBSD ahci.c     ISC   （已引入，未接入）
OpenBSD pciide.c   ISC   （已引入，未接入）
```

版权声明原样保留，一个字未改。

## 相关文档

```
docs/MACHINE.md         目标机器档案（天选6 / Ryzen AI 9 HX 370 / ESP 是 FAT32）
docs/DECISIONS.md       四条架构决定
docs/TRANSITIONAL.md    过渡期代码约定 + 109 处盘点
docs/THIRD-PARTY.md     第三方代码与许可证
docs/I18N.md            写作与本地化约定
docs/FATFS.md           FatFs 接入进度
```
