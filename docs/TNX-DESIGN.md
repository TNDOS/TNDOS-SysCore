# TNX 的设计取舍  > 本文件从 README.md 拆出。最后更新随源码走。 
## TNX 可执行格式（v1）

> **TNX 不解决「现代可执行文件格式的一切问题」，它只负责把一个已经链接好的
> TNDDOS 程序，用最低成本装进 TNDDOS 的地址空间。**

### 为什么不用 PE

PE 今天是**免费的** —— `LoadImage` 是固件替我们写的加载器。所以 TNX 的价值不是
「省代码」，而是：**当我们不能再调 `LoadImage` 的时候，我们还能加载程序。**

它真正要替代的不是 PE，是「离开 UEFI 之后自己写一个 PE 加载器」——那需要处理
DOS 头、PE 签名、COFF、可选头、段表、RVA 映射、基址重定位、导入解析，八百行且全是边界情况。
TNX 加载器是 `memcpy` 加一个跳转，**它不可能有 bug 到哪去**。

### 核心取舍

| 决定 | 省掉的东西 |
|---|---|
| 固定加载地址 | 整个重定位表 |
| 无动态导入 | 符号解析；系统调用走传入的 API 表 |
| 4 个固定段 tag | 段表分支 30 行写完 |
| 文件内连续排布 | 碎片处理 |

### 布局（头部恰好 48 字节，段表项 40 字节）

```
+---------------------------+
| TNX_HEADER   (48 字节)     |  魔数/版本/头长/标志/段数/入口/基址/映像大小
+---------------------------+
| 段表 (40 字节 x N)         |  CODE / DATA / RODT / RSRC / SIGN
+---------------------------+
| 段数据（16 字节对齐）        |
+---------------------------+
```

加载路径：校验魔数 → 校验版本/标志/基址/段表边界 → 逐段校验偏移与长度 →
清零映像窗口 → 逐段搬运 + BSS 置零 → 调入口。**没有重定位、没有导入、没有符号表。**

### 地址图

```
0x0000000000000000 - 0x00000000000FFFFF   保留（实模式 / BIOS 遗迹）
0x0000000000100000 - 0x0000000000FFFFFF   PMM 常规分配区（内核堆在这里长）
0x0000000001000000 - 0x00000000017FFFFF   TNX 程序映像窗口（8 MiB，PMM 永不发放）
```

窗口由 PMM 在初始化时就占住（标位图 + 真向固件 `AllocatePages`），
否则内核堆的首次适配会先把它吃掉，之后每次加载 TNX 都失败。

### 工具链

TNX **不需要自己的编译器，也不需要自己的链接器**：

```
hello.c -> clang -> hello.o -> ld.lld -> hello.elf -> tnxpack -> HELLO.TNX
```

`tools/tnxpack.ps1` 只做最后一步：读 ELF64 的 `PT_LOAD` 段，写 TNX 的段表加段数据。
（用 PowerShell 而非 C 是现实取舍 —— 这台机器上只有 clang，没有 C 运行库，
写 C 版宿主工具反而要手搓文件 IO。等 SDK 成形再重写并自举。）

### 实测

```
C:\EFI\TNDOS>HELLO.TNX          <- 直接敲程序名就执行（DOS 的规矩）

  ================================================
   Hello from a TNX program
  ================================================
  image addr    : 0x0000000001000650
  fixed base    : inside the 16MiB window          OK
  [rodata] read-only section is live
  [data]        wrote+read back: 0x123456789ABCDEF0
  [bss]         zero-filled by the loader          OK
  [heap]        kernel heap reachable from a TNX program
  ticks         : 0   (0 = no timer subsystem yet)
  returning 0

C:\EFI\TNDOS>HELLO               <- 扩展名可选
C:\EFI\TNDOS>NOPE.TNX
  Bad command or file name      <- DOS 的原话

  ================================================
   Hello from a TNX program
  ================================================
  image addr    : 0x0000000001000650
  fixed base    : inside the 16MiB window          OK
  [rodata] read-only section is live
  [data]        wrote+read back: 0x123456789ABCDEF0
  [bss]         zero-filled by the loader          OK
  [heap]        kernel heap reachable from a TNX program
  ticks         : 0   (0 = no timer subsystem yet)
  returning 0

```

### 命令语义

| 输入 | 行为 |
|---|---|
| `HELLO.TNX` / `HELLO` | **直接执行**。加载器不打印任何自己的东西 —— 跑一个程序不该先跟你汇报段表 |
| `TNX <file>` | 打印头部与段表，**不执行** |
| `TNXRUN <file>` | 执行，并打印加载器的完整跟踪（调试用） |

查找顺序和 DOS 一样：当前目录优先，然后依次查 `PATH` 的每一项；
名字里没有 `.` 就自动补 `.TNX`。找不到就是 `Bad command or file name`。

### v1 的边界（写清楚，不含糊）

- **一次只能装一个程序，没有隔离。** 没有分页，物理地址 == 虚拟地址。
  这不是缺陷 —— DOS 就是这样：固定段、一次一个、没有保护。
- **程序跑在内核栈上、ring 0。** 它崩了系统就崩了。真正的答案需要异常处理，
  而异常处理需要自己的 IDT，而 IDT 需要 `ExitBootServices`。
- **`SYSCALL` 现在还实现不了**，所以 API 是「加载器把函数表当参数传给入口」。
  源码级 API 不变，将来换成 syscall 桩，程序一个字节都不用改。

### 非目标（Anti-goals）

以下东西 TNX **永远不做**，做了就说明在重造 PE：

```
动态链接 / 导入表 / 导出表       TLS
异常元数据（.eh_frame / unwind）  调试信息 / 符号表
重定位                            资源目录（不是「资源」，是「目录」）
ABI 自动发现
```

**扩展规则**：任何新字段要进 header，必须先回答「加载器拿它干什么」。
答不上来的一律进 RSRC —— 加载器不解析 RSRC，所以塞什么都不会污染格式。
（`Checksum` 和 `Reserved` 就是被这条规则砍掉的。）
