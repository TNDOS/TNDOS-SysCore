# FAT 层：从 FatFs 到自研

> 0.3.2-M4-SP2.5 起，本项目**不再使用 FatFs**。这一页记的是为什么。

## 曾经的安排

`0.3.2-M4-SP2.5` 把 FatFs R0.16 接进了内核：

```
third_party/fatfs/source/ff.c            一个字未改
third_party/fatfs/source/fatfs_glue.c    我们自己写的接合层（MIT）
third_party/fatfs/source/ffconf.h        改了五项配置
```

理由是它 FAT12/16/32 + 子目录 + 长文件名全都有，而自研的 FAT16 读取器只认 FAT16。

## 它坏了，而且坏得很难看出来

症状：TNX 程序**小的能跑、大的必崩**。

```
HELLO.TNX    9588 字节  正常
TREE.TNX     7556 字节  正常
STRESS.TNX  27888 字节  必崩
CJK16.FNT  855624 字节  读不到（中文静默退回 8x12 ASCII 点阵）
```

崩溃的样子是最误导人的那种：**一条不可能出错的指令报了 #GP**。
反汇编看那里明明是 `testq %rax,%rax`，它不会 fault —— 于是怀疑过 trap 帧偏移、
ABI 桥没保存寄存器、定时器还挂着固件的处理程序、尾调用……

**真正的原因是：那段地址里根本不是那条指令。**

## 怎么查出来的

QEMU 的 `pmemsave` 在 `stop` 之后抓程序窗口的快照，和 TNX 映像逐字节比。
然后在加载器里把**源**（从盘上读进来的缓冲区）和**目标**（映像）并排打出来：

```
tnx.src.f30  == tnx.dst.f30       <- 拷贝是忠实的
tnx.src.1f30 == tnx.dst.1f30
```

拷贝没问题，**是源本身就已经错了**。再拿 `src` 和磁盘上的文件比：

```
磁盘 file[0xFE0]:  00 00 48 89 DF E8 F6 08
内存 buf[0xFE0]:   89 45 F0 EB B9 8B 45 BC     <- 不一样
```

`f_read` 返回成功、`br` 等于文件长度、`FRESULT` 是 `FR_OK` —— **内容却是错的**。

## 为什么不修它，而是自己写

问题在移植层里：FAT 的簇链、扇区缓存、跨簇读取三者之间的边界条件。
在那上面继续找，等于在一个我们不打算长期拥有的代码里做取证。

而这一层本来只需要做一件事：**按簇链把扇区读出来**。

```
删掉    ff.c / ff.h / diskio.h / ffconf.h / fatfs_glue.c / fatfs_tnd.h
写入    src/kernel/fat.c —— 1064 行，FAT12/16/32
收益    kernel.efi 139776 -> 134656（少 5 KB）
```

## 自研驱动长什么样

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

## 一路上踩的三个坑（都写进代码注释了）

**① `blk_read` 成功返回 0，不是 1**

```c
int blk_read(...) { return b->Read(...) == 0 ? 0 : -3; }   // 0 = 成功
if (!blk_read(...)) return 0;                              // 一成功就判失败
```

旧 `fat.c` 里那几段探测代码本来就有这个错 —— 它一直靠 FatFs 走 `disk_read`，
从没跑到过，所以谁也没发现。

**② 懒挂载失败一次就再也不试**

块设备是驱动注册的，而驱动是 TNX 程序 —— 比模块初始化晚得多。
第一次问「有没有 FAT 卷」时 `blk_count()` 还是 0，于是失败，然后永远不重试。
所以有了幂等的 `fat_mount_any()`：谁发现没挂就调一次。

**③ 相对路径没有接上当前目录**

```
fat_resolve("HELLO.TXT")  ->  "\HELLO.TXT"       根目录，不是 \EFI\TNDOS\HELLO.TXT
```

而固件那条老路还在兜底一部分，于是表现为「有的程序能跑、有的不能」。

## 现在谁在读文件

```
TNX 程序映像        read_whole       -> fat_read_alloc
中文字体            font_load        -> t_font_read_hook -> fat_read_alloc
config.sys / autoexec.bat            -> fat_read_text
api_stat / api_open                  -> fat_stat / fat_read_alloc（整个读进堆）
api_findfirst / findnext             -> fat_enum
=====================================================================
固件 SimpleFileSystem   只在 FAT 不可用时兜底；ExitBootServices 之后它不存在
```

## 遗留

```
· 只解析 8.3 短名，LFN 项跳过 —— 显示长文件名要另外实现
· 自研盘上的卷序列号显示 0000-0000（生成 FAT32 时没写 BPB 偏移 39）
· 写操作没有掉电一致性 —— 这不是日志式文件系统
```