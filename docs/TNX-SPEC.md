# TNX 可执行格式 —— 标准 v1.1

| | |
|---|---|
| 状态 | **v1.0 已冻结；v1.1 为当前版本** |
| 版本 | v1.1 `TNX_VERSION = 0x00010001`；加载器同时接受 v1.0 `0x00010000` |
| 参考实现 | `src/kernel/tnx.c`（加载器）、`repos/TNDOS-ToolsKit/tnxpack.ps1`（打包器）、`repos/TNDOS-ToolsKit/tnxdump.ps1`（检查器） |
| 权威定义 | `src/include/tnx.h` |

> **v1.1 相对 v1.0 只有一处变化：header 加了一个 `Machine` 字段（架构标识）。**
> 没有别的改动 —— **如果两次版本的 diff 里出现了别的东西，那是错误，不是特性。**

---

## 0. 一句话

> **TNX 不解决「现代可执行文件格式的一切问题」。它只负责把一个已经链接好的
> TNDDOS 程序，用最低成本装进 TNDDOS 的地址空间。**

这一句是后面所有取舍的裁判。任何提案只要让 TNX 更接近 PE，就默认否决。

---

## 1. 为什么存在

### 1.1 不是「比 PE 简单」，是「比自写 PE 加载器便宜」

很多论证把 TNX 的价值说成「比 PE 简单」。**这个论证是弱的**，因为：

> 在 UEFI 底下，**PE 是免费的** —— `LoadImage` 是固件替我们写的加载器。
> 现在加载 kernel、加载驱动，我们一行加载器代码都没写过。

所以 TNX 真正的价值是：

> **当我们不能再调 `LoadImage` 的时候，我们还能加载程序。**

它不是格式项目，是**独立项目**。

### 1.2 那么替代方案是什么

离开 UEFI 之后，真正的选择不是「PE vs TNX」，而是：

| 方案 | 成本 |
|---|---|
| 继续用固件 `LoadImage` | 0，代价是**永远离不开 UEFI** |
| **自己写 PE 加载器** | ~800 行，且全是边界情况：DOS 头 → PE 签名 → COFF → 可选头 → 段表 → RVA 映射 → 基址重定位 → 导入解析 |
| **TNX + tnxpack + 加载器** | 规格 + ~300 行打包器 + ~250 行加载器 |

**这才是 TNX 最硬的论证。** PE 加载器里每一条都是一类 bug；
TNX 加载器是 `memcpy` 加一个跳转，**它不可能有 bug 到哪去**。

---

## 2. 非目标（Anti-goals）

以下东西 TNX **永远不做**。做了就说明在重造 PE。

```
动态链接 / 导入表 / 导出表
TLS
异常元数据（.eh_frame / unwind）
调试信息 / 符号表
重定位
资源目录（注意：不是「资源」，是「目录」）
ABI 自动发现
插件依赖声明
```

### 2.1 扩展规则（治理机制）

光写「不做」是撑不住的。所以配一条硬规则：

> **任何新字段要进 header，必须先回答「加载器拿它干什么」。**
> 答不上来的一律进 `RSRC`。

因为 `RSRC` 加载器不解析，塞什么都不会污染格式。

这条规则已经砍掉了两个我原本想要的字段：

- `Checksum` —— 唯一合法值是 0 的字段不是预留，是死重。它比不存在更糟：
  将来一定有人给它实现一个自己的算法，然后两个实现不兼容。
- `Reserved[4]` —— 有 `HeaderSize` 就够了。Reserved 字段是「没有版本机制的
  版本机制」，最后都会长成没人敢动的地雷。

---

## 3. 磁盘布局

```
+---------------------------+
| TNX_HEADER (v1.0: 48 / v1.1: 56) |
+---------------------------+
| 段表 (40 字节 x SectionCount) |
+---------------------------+
| 段数据（每段起始 16 字节对齐）  |
+---------------------------+
```

段数据在文件里**连续排布**，段表顺序即数据顺序。文件里没有对齐空洞以外的填充。

### 3.1 TNX_HEADER（v1.0: 48 字节 / v1.1: 56 字节，小端）

```c
typedef struct {
    tnx_u64 Magic;         /*  0 */
    tnx_u32 Version;       /*  8 */
    tnx_u32 HeaderSize;    /* 12 */
    tnx_u32 Flags;         /* 16 */
    tnx_u32 SectionCount;  /* 20 */
    tnx_u64 EntryRVA;      /* 24 */
    tnx_u64 ImageBase;     /* 32 */
    tnx_u64 ImageSize;     /* 40 */
    /* ---- v1.1 追加 ---- */
    tnx_u64 Machine;       /* 48 */
} TNX_HEADER;
```

**v1.0 的文件在 48 字节处结束。** v1.1 追加 `Machine` 之后头长 56。
加载器**按 `HeaderSize` 定位段表**，所以两种文件用的是同一段解析代码。

| 字段 | 含义 |
|---|---|
| `Magic` | `0x000000001A584E54`，磁盘上就是 `54 4E 58 1A 00 00 00 00`。不得与 PE（`4D 5A`）或 ELF（`7F 45 4C 46`）撞车 |
| `Version` | `0x00010000` = v1.0，`0x00010001` = v1.1。加载器**只接受这两个值**，其余一律拒绝 —— **不做范围判断** |
| `HeaderSize` | v1.0 = 48，v1.1 = 56。**这是唯一的扩展机制**：加字段就把它变大并升 `Version` |
| `Flags` | 见 3.2 |
| `SectionCount` | 1..16 |
| `EntryRVA` | 入口相对 `ImageBase` 的偏移 |
| `ImageBase` | **固定值**，见第 4 节 |
| `ImageSize` | 内存映像总大小（含 BSS），`>= EntryRVA` |
| `Machine` | v1.1 新增，架构标识。见 3.3 |

### 3.2 Flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `TNX_FLAG_CONSOLE` | 程序需要控制台 |
| 其余 | —— | **未定义。出现任何未定义的位，加载器必须拒绝，不许猜** |

### 3.3 Machine（架构标识，v1.1 新增）

**TNX 标的是 CPU，不是固件。** 同一颗 CPU 上换一套固件，二进制不用变。

| 值 | 名称 | 固件 | 状态 |
|---|---|---|---|
| `0x00` | **无效** | —— | v1.1 起**禁止**写这个值，加载器见到一律拒绝。v1.0 的兼容**不由这个值决定**，由 `Version` 决定（见下） |
| `0x01` | AMD64 | EFI (EDK II) | 官方 |
| `0x02` | IA32 | EFI (EDK II) | 官方 |
| `0x03` | ARMv7 | EFI (EDK II) | 官方 |
| `0x04` | AArch64 | EFI (EDK II) | 官方 |
| `0x05` | RISCV32 | EFI (EDK II) | 官方 |
| `0x06` | RISCV64 | EFI (EDK II) | 官方 |
| `0x07` | LOONGARCH32 | EFI (EDK II) | 官方 |
| `0x08` | LOONGARCH64 | EFI (EDK II) | 官方 |
| `0x09` | IA64 | EFI 1.10 (EDK I) | 官方 · **EFI 的原生平台**，但走的是已停更的 EDK I 那条线。**它不是「加个 target 就能编」的移植，是另一份固件绑定** —— EFI 1.10 与 UEFI 2.x 有结构差异 |

| 范围 | 用途 |
|---|---|
| `0x0A`–`0x7F` | 保留 —— **需要就提 Issue**，官方敲定后分配 |
| `0x80`–`0xFF` | 实验 / 私有 —— **永久不登记，官方永不占用** |

#### v1.0 文件按 **AMD64** 处理，不是按"本机"

TNX 至今**只在 AMD64 上产生过二进制**。所以一个没有 `Machine` 字段的 v1.0 文件，
**语义上就是 AMD64** —— 不是"本机"，是 **AMD64**。

这个区别很要紧：写成"本机"的话，一个 LoongArch 内核会去加载一个 AMD64 的 v1.0 文件，
**然后把 x86 机器码按 LoongArch 解，直接崩掉。**

```
Version == 1.0    ->  当作 AMD64
                      只有 AMD64 加载器接受它
                      别的架构一律拒绝，并说明"这是 v1.0，v1.0 只可能是 AMD64"

Version == 1.1    ->  读 Machine，必须等于本机
                      Machine == 0x00        -> 拒绝
                      不认识的号              -> 拒绝
                      0x80-0xFF              -> **也拒绝**（那是"没登记"，不是"什么机器都能跑"）
```

#### 为什么 `Machine` 是 `u64` 而不是 `u8`

因为 `u8` 会在 `Flags` / `SectionCount` 之后留下 3 个字节空洞，而那些空洞
**只能叫 `Reserved`** —— 而 2.1 节的立场是「**`Reserved` 字段是「没有版本机制的
版本机制」，最后都会长成没人敢动的地雷**」。

**宁可贵 7 个字节，也不引入一个 `Reserved`。** 反正头长由 `HeaderSize` 说了算。

#### 32 位和 64 位是**两种架构**，不是一个的兼容模式

`IA32` / `ARMv7` / `RISCV32` / `LOONGARCH32` 都是**独立**的架构值。
**"64 位内核跑 32 位程序"不是格式能决定的事，是一个子系统：**

```
1  CPU 侧的兼容模式
   x86 的 compatibility mode、ARM 的 AArch32 EL0 ……
   而且 **UEFI 不保证固件留下的段描述符能用** —— 那是 Boot Services 之外的东西

2  **一份 32 位版的 API 表**
   我们的 ABI 传的是**函数表**，表里装的是 **64 位指针** —— 32 位程序根本用不了
   这一条是无解的：不是"改个标志位"，是要维护第二套 ABI

3  地址模型也要重看
   ImageBase = 0x1000000 在 32 位下放得下，但窗口（8 MiB）和将来的分页都要重算
```

**所以规范上的态度是：**

| | |
|---|---|
| 格式 | **登记这些值** —— 它们是有意义的架构标识 |
| 实现 | **明确拒绝** —— AMD64 内核不加载 IA32 程序，并说清楚为什么 |
| 将来 | **要做就是一个独立子系统**，不是给 `Machine` 加一位 |

**别让后来的人以为加个标志位就能跑。** 这跟 WOW64 是同一类问题 ——
**而 WOW64 是一整个子系统，不是开关。**

#### 为什么升版本，而不是塞进 `Flags` 的空位

```
塞 Flags   老加载器不认识那些位 -> **忽略** -> 搬进内存 -> 跑错架构的机器码 -> 崩
升版本     老加载器 Version 对不上 -> **直接拒绝** ✓
```

这是「失败要响」在格式层的应用。**老东西明确拒绝，比老东西装作没看见好。**

### 3.4 TNX_SECTION（40 字节，小端）

```c
typedef struct {
    tnx_u32 Tag;           /*  0 */
    tnx_u32 Flags;         /*  4 */
    tnx_u64 FileOffset;    /*  8 */
    tnx_u64 RVA;           /* 16 */
    tnx_u64 FileSize;      /* 24 */
    tnx_u64 MemSize;       /* 32 */
} TNX_SECTION;
```

### 3.5 段 tag

| Tag | 值（小端 u32） | 加载器行为 |
|---|---|---|
| `CODE` | `0x45444F43` | 搬运 + 可执行 |
| `DATA` | `0x41544144` | 搬运 + 可写 |
| `RODT` | `0x54444F52` | 搬运 + 只读 |
| `RSRC` | `0x43525352` | **只搬运，永不解析** |
| `SIGN` | `0x4E474953` | **只记录，永不验证**（v1 没有任何验证器） |

未知 tag → 拒绝。

**为什么是固定 5 个 tag 而不是任意命名？**
因为段表解析代码要能 30 行写完。任意命名意味着字符串比较、哈希表、
或者更糟——一个字符串表段。那就是 PE 的 Section Table 重演。

### 3.6 段 Flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `TNX_SEC_R` | 可读 |
| 1 | `TNX_SEC_W` | 可写 |
| 2 | `TNX_SEC_X` | 可执行 |
| 3 | `TNX_SEC_ZERO` | 纯 BSS。**置位时 `FileSize` 必须为 0** |

v1 的加载器**不强制**内存保护（没有分页），这些位是给未来的自己和工具看的。

---

## 4. 地址模型

### 4.1 地址图（TNDDOS 全系统）

```
0x0000000000000000 - 0x00000000000FFFFF   保留（实模式 / BIOS 遗迹）
0x0000000000100000 - 0x0000000000FFFFFF   PMM 常规分配区（内核堆在这里长）
0x0000000001000000 - 0x00000000017FFFFF   TNX 程序映像窗口（8 MiB）
0x0000000001800000 - .....................  其余交给 PMM
```

- `TNX_IMAGE_BASE = 0x0000000001000000`（16 MiB）
- `TNX_WINDOW_SIZE = 0x0000000000800000`（8 MiB）

### 4.2 窗口必须被预留

PMM 在初始化时就把窗口标成已用，并真向固件 `AllocatePages`。

**不这么做的话，内核堆的首次适配会先把它吃掉**，之后每次加载 TNX 都失败，
而且失败原因看起来就是「内存不够」——极难查。

### 4.3 一次只能装一个程序

因为固定 `ImageBase` 且没有分页，**物理地址 == 虚拟地址**，
所以同一时刻只能有一个 TNX 映像。

**这不是缺陷，这是 DOS 语义。** 固定段、一次一个、没有隔离、没有保护。

将来有了分页之后，固定 `ImageBase` 不再妨碍多进程 ——
每个进程有自己的地址空间，`0x1000000` 可以重复。

---

## 5. 加载算法（规范性）

加载器**必须**按此顺序：

1. 读入整个文件到内存缓冲
2. 校验头部（见 5.1）—— **含 `Machine` 比对：必须等于本机，或为 `0x00`（v1.0 文件）**
3. 校验段表每一条（见 5.1）
4. **校验全部通过之后**才动内存
5. 把 `[ImageBase, ImageBase + ImageSize)` 整体清零
6. 对每一段：`memcpy` 到 `ImageBase + RVA`，再把 `[FileSize, MemSize)` 清零
7. 构造 API 表（见第 6 节）
8. 调用 `ImageBase + EntryRVA`

**第 4 步不能提前。** 部分加载（校验到一半就开始搬）会产生
「一半是新程序、一半是上一个程序」的状态，比直接拒绝危险得多。

### 5.1 校验规则（逐条，全部必须）

头部：

```
Magic == TNX_MAGIC                     否则拒绝
Version == TNX_VERSION                 否则拒绝（不做范围判断）
HeaderSize >= 48                       否则拒绝
HeaderSize <= 文件大小                  否则拒绝
(Flags & ~TNX_KNOWN_FLAGS) == 0        否则拒绝
SectionCount >= 1                      否则拒绝
SectionCount <= 16                     否则拒绝
ImageBase == TNX_IMAGE_BASE            否则拒绝
ImageSize >= 1                         否则拒绝
ImageSize <= TNX_WINDOW_SIZE           否则拒绝
EntryRVA < ImageSize                   否则拒绝
HeaderSize + SectionCount*40 <= 文件大小 否则拒绝
```

每一段：

```
Tag 是已知的 5 个之一                  否则拒绝
FileSize <= MemSize                    否则拒绝
(ZERO 置位) => FileSize == 0           否则拒绝
FileOffset + FileSize <= 文件大小      否则拒绝
RVA + MemSize <= ImageSize             否则拒绝
RVA % 16 == 0                          否则拒绝
```

**任何一条不过，整体拒绝，绝不部分加载。**
`repos/TNDOS-ToolsKit/tnxdump.ps1 -Validate` 在宿主机上重跑这整套规则，
所以不用启动 QEMU 就能知道一个 TNX 会不会被接受。

---

## 6. ABI

### 6.1 调用约定是 System V

> **这一条最容易踩，而且症状最误导。**

- TNX 程序编译目标 `x86_64-unknown-none` → **System V AMD64 ABI**（第一个参数在 RDI）
- TNDDOS 内核编译目标 `x86_64-pc-windows-msvc` → **Microsoft x64 ABI**（第一个参数在 RCX）

不钉死的话：程序把指针放进 RDI，内核函数去 RCX 拿垃圾。
**报出来的是 `#UD 无效指令`，而且崩溃地址落在内核里，看起来跟参数传递毫无关系。**

已决定：**TNX 的 ABI 就是 System V**（TNX 从 ELF64 来，这是自然选择）。
内核那侧用 `TND_ABI` 宏（展开成 `sysv_abi`）适配。

### 6.2 API 是「传进来的函数表」（v2）

**为什么不是 SYSCALL：**

我们现在还活在 Boot Services 底下，**固件握着 IDT**，装不了自己的中断门。
所以 `SYSCALL`（需要 `IA32_LSTAR`）和 `INT n`（需要 IDT 门）两条路现在都走不通。
而 `ExitBootServices` 要等驱动模型就绪之后才能做。

于是：**加载器把 API 表作为参数传给程序入口。**

```c
typedef int (TND_ABI *TNX_ENTRY)(const TND_API_TABLE *api);
```

```c
```c
/* v2 = 24 个入口，格式的权威定义在 include/tnd_api.h */
typedef struct {
    tnd_u32 StructSize;   /* 内核给了多少，程序据此判断 */
    tnd_u32 Version;      /* v2 = 0x00020000 */

    /* 控制台 */
    void (*TND_ABI puts)(const char *s);
    void (*TND_ABI putc)(int c);
    void (*TND_ABI putu)(tnd_u64 v);
    void (*TND_ABI putx)(tnd_u64 v);

    /* 程序环境：argv 是 DOS 的规矩，argv[0] 是程序名 */
    int         (*TND_ABI argc)(void);
    const char *(*TND_ABI argv)(int i);
    const char *(*TND_ABI env)(const char *name);

    /* 句柄 I/O：0/1/2 = 标准输入/输出/错误 */
    int    (*TND_ABI open)(const char *path, int flags);
    int    (*TND_ABI close)(int fd);
    tnd_i64 (*TND_ABI read)(int fd, void *buf, tnd_i64 count);
    tnd_i64 (*TND_ABI write)(int fd, const void *buf, tnd_i64 count);
    tnd_i64 (*TND_ABI seek)(int fd, tnd_i64 offset, int whence);

    /* 文件系统 + 目录遍历（DOS 式 findfirst/findnext） */
    int (*TND_ABI unlink)(const char *path);
    int (*TND_ABI mkdir)(const char *path);
    int (*TND_ABI rmdir)(const char *path);
    int (*TND_ABI rename)(const char *from, const char *to);
    int (*TND_ABI stat)(const char *path, TND_STAT *st);
    int (*TND_ABI findfirst)(const char *pattern, TND_FIND *out);
    int (*TND_ABI findnext)(int fh, TND_FIND *out);
    int (*TND_ABI findclose)(int fh);

    /* 内存 / 时间 */
    void    *(*TND_ABI alloc)(tnd_size n);
    void     (*TND_ABI free)(void *p);
    tnd_u64  (*TND_ABI ticks)(void);
} TND_API_TABLE;
```

**v1 那张表只有 7 个入口（puts/putc/putu/putx/alloc/free/ticks），什么都写不了** ——
连 MORE 都不行，因为它要读文件而没有文件 API。v2 补上的是 **DOS 的骨架**：
句柄模型、文件 API、目录遍历、argv。

**为什么值句柄模型**：重定向和管道不需要新增 API ——
它们只是「把某个 fd 换成别的东西」，程序一个字都不用改。
这就是 DOS 的 0/1/2 能活四十年的原因。

### 6.3 ABI 的第二个坑：这个头文件里一个 long 都不能有

```c
x86_64-unknown-none        (SysV)      long = 8 字节
x86_64-pc-windows-msvc     (MS ABI)    long = 4 字节
```

同一个结构体在两边尺寸不同 —— **比调用约定还阴，因为结构体偏移会整体错位**，
而且症状同样是崩溃在内核里。所以 API 里全部用定宽类型
（tnd_u32 / tnd_u64 / tnd_i64），**禁用 long**。
```

**关键性质：源码级 API 不变。** 程序永远写 `tnd_puts(...)`。
将来换成真 syscall，只是加载器填的指针换成 syscall 桩，**程序一个字节都不用改**。

`StructSize` 允许内核将来只填前半段 —— 老程序读 `StructSize` 就知道哪些能用。

### 6.4 入口签名

程序**必须**导出 `tnx_entry`，签名如上。SDK 的 `lib/tndrt.c` 提供它：
存下 API 表，再调用户写的 `tnx_main`。用户代码因此不需要知道表从哪来。

### 6.5 返回

入口返回 `int`，0 表示正常。加载器把返回值原样报出去。
**v1 没有 exit / 没有进程模型** —— 返回就是从程序回到内核。

---

## 7. 版本与兼容

- `Version` **精确匹配**，不做「向后兼容」。
  加载器只认 v1.0；v1.1 的文件会被 v1.0 加载器拒绝，反之亦然。
- 加字段：`HeaderSize` 变大 + `Version` 升级。
  新加载器读老文件时，`HeaderSize` 小于预期 → 尾部字段取默认值。
- 去掉字段：**不允许**。改成保留位并永远填默认值。
- TNX 格式的权威定义在 `src/include/tnx.h`。
  它在 SysCore / SDK / ToolKit 各有一份副本 —— 所以**任何改动都必须先升版本号**，
  那时同步是必然动作，不会漏。

---

## 8. 工具链

```
hello.c -> clang -> hello.o -> ld.lld -> hello.elf -> tnxpack -> HELLO.TNX
```

**TNX v1 不需要自己的编译器，也不需要自己的链接器。**
真正的工具链还是 clang / lld，tnxpack 只负责最后一步。

打包器从 ELF64 的 `PT_LOAD` 程序头取段：
`p_vaddr` 定 RVA，`p_memsz` 定 MemSize，`p_flags` 推 tag（X→CODE，W→DATA，否则 RODT）。
`ImageBase` 取所有 `PT_LOAD` 的最小 `p_vaddr`。

链接脚本（`repos/TNDOS-SDK/linker/tnx.ld`）负责把 `.` 设成 `0x1000000`，
并 `/DISCARD/` 掉 `.eh_frame` / `.comment` / `.note*` —— 那些正是非目标清单里点名不要的。

---

## 9. v1 的已知边界（写清楚，不含糊）

- **一次只能装一个程序，没有隔离。**
- **程序跑在内核栈上、ring 0。它崩了系统就崩了。**
  真正的答案需要异常处理，而异常处理需要自己的 IDT，而 IDT 需要 `ExitBootServices`。
- **`SIGN` 段没有任何验证器。** 它现在只是一个占位，加载器只记录不验证。
- **没有 `StackSize`。** 想过要加，按扩展规则问「加载器拿它干什么」——答不上来，
  因为 v1 共用内核栈。等有进程模型时再加，那时它就有用了。

---

## 10. 待定问题

1. **多程序**：什么时候引入分页？分页之后 `ImageBase` 固定是否仍然合适？
2. **驱动改用 TNX**：现在驱动走 `LoadImage`（PE）。TNX 加载器能跑程序之后，
   驱动要不要也换成 TNX？换了之后 `DEVICE=` 就不再需要固件的加载器了。
3. **`RSRC` 的实际用途**：现在没有用例。第一个真实用例出现之前不动它。
4. **签名**：谁来验？内核自己，还是启动时加载的验证模块？
5. **`tnxpack` 重写成 C**：现在用 PowerShell 是因为这台机器上只有 clang、
   没有 C 运行库。等 TNDDOS 能自举，它应该变成 TNDDOS 原生工具。
