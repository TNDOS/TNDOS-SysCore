# TNX v1 Specification

**TNDOS Native eXecutable**

状态：Frozen
版本：TNX v1.0

---

## 1. 设计目标

TNX（TNDOS Native eXecutable）是 TNDOS 原生程序格式。

TNX v1 的目标不是成为通用现代可执行文件格式，而是以尽可能低的加载复杂度，为 TNDOS 提供一种脱离 UEFI `LoadImage()` 后仍可自行加载的原生程序格式。

TNX v1 的核心原则：

* 固定加载地址
* 无重定位
* 无动态链接
* 无导入表
* 无导出表
* 无符号解析
* 无 TLS
* 无异常元数据
* 无调试信息
* 简单、确定性的加载过程

TNX v1 的加载模型应尽可能接近：

```text
allocate
    ↓
copy
    ↓
zero-fill
    ↓
initialize API vector
    ↓
set stack
    ↓
jump
```

---

# 2. 为什么需要 TNX

当前 TNDOS 运行于 UEFI 环境时，可以使用 UEFI `LoadImage()` 加载 PE/COFF 映像。

因此，在仍依赖 UEFI Boot Services 的阶段，TNX 并不会减少实现代码。

TNX 的真正价值出现在 TNDOS 不再依赖 UEFI Boot Services 之后。

此时有两个选择：

### 方案 A：继续使用 PE

TNDOS 必须自行实现 PE loader，包括：

* DOS Header
* PE Signature
* COFF Header
* Optional Header
* Section Table
* RVA 映射
* Section 对齐
* 基址处理
* 重定位
* Import Resolution

即使只实现 TNDOS 实际需要的子集，也需要处理大量 PE 格式边界情况。

### 方案 B：使用 TNX

TNX loader 只需要处理 TNDOS 自己定义的格式。

因此：

> **TNX 的价值不是“现在比 UEFI LoadImage 更省代码”，而是避免 TNDOS 在脱离 UEFI 后重新承担 PE loader 的复杂度。**

PE 负责进入 TNDOS 的 boot world。

TNX 负责 TNDOS 内部的 native world。

---

# 3. TNX v1 的执行模型

TNX v1 不要求分页。

在 v1 的执行模型中：

```text
Physical Address == Virtual Address
```

因此 TNX v1 不提供进程地址空间隔离。

### v1 限制

同一时刻只加载并运行一个 TNX 程序。

TNX 程序之间没有内存保护。

程序可以访问其地址空间内的任意地址。

这不是 TNX v1 的错误，而是有意采用的 DOS 风格执行模型。

> **TNX v1 = 单程序、固定地址、无隔离。**

未来如果 TNDOS 引入分页和进程地址空间，可以在不改变 TNX 文件格式基本模型的情况下，为程序提供独立虚拟地址空间。

---

# 4. 地址空间规划

TNX 固定 ImageBase 必须与当前 PMM 的物理页分配范围隔离。

TNX v1 使用固定程序映像窗口：

```text
0x0000000000000000 - 0x00000000000FFFFF
    保留
    低地址区域 / 历史兼容区域

0x0000000000100000 - 0x0000000000FFFFFF
    PMM 常规分配区

0x0000000001000000 - 0x0000000001FFFFFF
    TNX 程序映像窗口
    固定
    PMM 永久保留

0x0000000002000000 -
    TNDOS 内核目标区域 / 后续规划区域
```

TNX v1 默认：

```text
ImageBase = 0x0000000001000000
```

即：

```text
ImageBase = 16 MiB
```

程序映像必须满足：

```text
ImageBase + ImageSize <= 0x02000000
```

即 TNX v1 映像不得越过 32 MiB 边界。

### PMM 要求

PMM 初始化时必须将：

```text
0x01000000 - 0x01FFFFFF
```

整个区域标记为已占用/Reserved。

该区域不得：

* 分配给 kernel heap
* 分配给其他内核对象
* 分配给驱动
* 分配给普通物理页请求

这样可以保证：

```text
PMM allocation
        ↓
永远不会覆盖 TNX ImageBase
```

TNX loader 不需要与普通 PMM 分配进行运行时竞争。

---

# 5. TNX 文件布局

```text
+---------------------------+
| TNX_HEADER    80 bytes    |
+---------------------------+
| Section Table  48 * N      |
+---------------------------+
| Section Data               |
|                           |
| aligned                   |
+---------------------------+
| ...                        |
+---------------------------+
```

所有 section 均由 Section Table 描述。

TNX v1 不定义 Section Name。

Section 通过固定 Tag 识别。

---

# 6. TNX Header

```c
typedef struct {
    UINT64 Magic;
    UINT32 Version;
    UINT32 HeaderSize;
    UINT32 Flags;
    UINT32 SectionCount;

    UINT64 EntryRVA;
    UINT64 ImageBase;
    UINT64 ImageSize;
    UINT64 StackSize;
    UINT64 HeapReserve;
    UINT64 ApiRVA;
} TNX_HEADER;
```

总大小：

```text
80 bytes
```

### Magic

用于识别 TNX 文件并防止 PE/其他文件被误加载。

逻辑值：

```text
"TNX\x1A"
```

实际编码方式必须保证不会与普通 PE/COFF 文件发生误识别。

### Version

TNX 格式版本。

v1：

```text
0x00010000
```

版本发生不兼容变化时必须增加版本号。

### HeaderSize

Header 实际大小。

v1：

```text
80
```

Header 扩展时：

```text
HeaderSize ↑
```

旧字段位置不得改变。

### Flags

描述程序执行属性。

v1 可定义的 flag 包括：

* 权限属性
* 是否需要控制台
* 是否允许作为用户程序运行
* 是否包含 API Vector

未定义的 Flag 必须被 loader 拒绝或按照版本规则处理，不得自行赋予语义。

### SectionCount

Section 数量。

### EntryRVA

程序入口点相对于 `ImageBase` 的偏移。

入口地址：

```text
EntryAddress = ImageBase + EntryRVA
```

### ImageBase

固定映像地址。

TNX v1 不支持重定位。

默认：

```text
0x01000000
```

### ImageSize

整个 TNX 映像所需的内存大小，包括：

* CODE
* DATA
* RODT
* RSRC
* BSS / zero-fill 区域

### StackSize

程序请求的栈大小。

### HeapReserve

程序请求的堆保留空间。

v1 的具体 heap 分配机制由 TNDOS runtime/loader 定义，不属于 TNX 文件解析器职责。

### ApiRVA

API Vector 相对于 `ImageBase` 的地址。

当程序包含 API Vector 时：

```text
ApiAddress = ImageBase + ApiRVA
```

---

# 7. Section Table

```c
typedef struct {
    UINT32 Tag;
    UINT32 Flags;

    UINT64 FileOffset;
    UINT64 RVA;

    UINT64 FileSize;
    UINT64 MemSize;

    UINT32 Alignment;
    UINT32 Reserved;
} TNX_SECTION;
```

大小：

```text
48 bytes
```

---

# 8. Section 类型

TNX v1 定义以下标准 Section：

```text
CODE
DATA
RODT
RSRC
SIGN
```

### CODE

可执行代码。

典型属性：

```text
R | X
```

### DATA

可写初始化数据。

典型属性：

```text
R | W
```

### RODT

只读数据。

典型属性：

```text
R
```

### RSRC

资源数据。

RSRC 是 **opaque blob**。

TNX loader：

> **不得解析 RSRC 内容。**

loader 只负责：

```text
File → Memory
```

RSRC 内部格式完全由上层程序自行定义。

TNX 不定义：

* Resource Directory
* Resource Tree
* Resource ID
* Resource Name
* Resource Language
* Resource Lookup

因此 RSRC 不得演变为 PE 风格资源目录系统。

### SIGN

签名数据。

SIGN 与其他 Section 一样由 Section Table 描述。

TNX loader 根据具体安全策略处理 SIGN。

签名机制本身不属于 TNX v1 基础加载格式。

---

# 9. Section Flags

Section Flags 描述内存属性：

```text
R
W
X
ZERO
```

其中：

```text
ZERO
```

表示该 Section 的部分或全部内存由 loader 清零，而不是从文件复制。

一般情况下：

```text
MemSize > FileSize
```

意味着：

```text
[FileOffset, FileOffset + FileSize)
```

复制到：

```text
[ImageBase + RVA,
 ImageBase + RVA + FileSize)
```

剩余：

```text
[ImageBase + RVA + FileSize,
 ImageBase + RVA + MemSize)
```

必须清零。

这可以表达传统 BSS。

---

# 10. TNX Loader

TNX loader 的逻辑应保持简单且确定：

```text
Read Header
    ↓
Check Magic
    ↓
Check Version
    ↓
Check HeaderSize
    ↓
Check SectionCount
    ↓
Check ImageBase
    ↓
Check ImageSize
    ↓
Check all Section bounds
    ↓
Reserve TNX window
    ↓
Copy Sections
    ↓
Zero-fill
    ↓
Initialize API Vector
    ↓
Prepare Stack
    ↓
Jump Entry
```

不得在 loader 中增加：

* 符号解析
* Import Resolution
* Export Resolution
* Relocation
* TLS 初始化
* Resource Directory 解析
* Debug Information 解析

TNX loader 的目标是保持在数百行以内，并尽可能接近：

```text
validate
→ memcpy
→ memset
→ initialize
→ jump
```

---

# 11. API Vector

TNX v1 不使用 `SYSCALL` 指令作为用户程序 API 入口。

原因是当前 TNDOS 在 UEFI Boot Services 存活阶段不能安全地接管完整中断/异常环境。

因此 v1 使用 API Vector。

程序映像内预留固定 API Vector 区域。

```text
ImageBase + ApiRVA
        │
        ├── API 0  → function pointer
        ├── API 1  → function pointer
        ├── API 2  → function pointer
        ├── ...
        └── API 63 → function pointer
```

每项：

```text
UINT64
```

API Vector 的具体大小、编号和参数 ABI 必须由 TNDOS ABI 文档冻结。

---

# 12. API ABI

x86-64 ABI 使用寄存器传递参数。

建议冻结：

```text
RAX = API number
RDI = arg1
RSI = arg2
RDX = arg3
R10 = arg4
R8  = arg5
R9  = arg6

RAX = return value
```

但 TNX v1 程序不直接执行：

```asm
SYSCALL
```

SDK 将 API 包装为普通函数调用。

例如：

```c
static inline long tnd_write(
    int fd,
    const void *buf,
    size_t len
)
{
    return api_vector[API_WRITE](fd, buf, len);
}
```

程序源码因此不需要知道底层 API Vector 的实现细节。

---

# 13. API Vector 的未来迁移

API Vector 的目标不是永久替代 `SYSCALL`。

它是当前 UEFI 阶段的 ABI 实现方式。

未来 TNDOS 完成：

```text
ExitBootServices()
        ↓
IDT
        ↓
Exception/Interrupt infrastructure
        ↓
SYSCALL/SYSRET
```

之后，可以将相同 API ABI 实现为：

```text
TNX program
    ↓
SDK
    ↓
SYSCALL stub
    ↓
kernel
```

因此：

> **冻结的是 API 编号和参数 ABI，而不是具体的调用机制。**

这样从 API Vector 迁移到真正 `SYSCALL` 时，用户程序源代码无需修改。

---

# 14. Anti-goals

TNX v1 明确不追求以下功能：

```text
Dynamic Linking
Import Table
Export Table
TLS
Exception Metadata
.eh_frame / unwind metadata
Debug Information
Symbol Table
Relocation
Resource Directory
ABI Auto Discovery
```

这些功能不是 TNX v1 的组成部分。

如果未来确实需要其中某项功能：

> 必须进行新的格式设计和版本决策。

不得偷偷扩展 TNX v1。

---

# 15. 扩展规则

TNX 的扩展必须遵循一个问题：

> **加载器拿它干什么？**

如果新增字段需要 loader：

* 读取
* 验证
* 修改
* 初始化
* 根据其内容改变加载行为

那么它可以成为 Header 的正式字段。

如果 loader 完全不需要理解其内容：

```text
→ RSRC
```

如果功能无法在现有 TNX v1 模型下安全表达：

```text
→ 新版本
```

不得通过增加：

```text
Reserved[]
```

来预留没有明确语义的字段。

---

# 16. 为什么删除 Checksum

早期设计曾包含：

```c
UINT64 Checksum;
```

并定义：

```text
0 = 不校验
```

该设计被否决。

原因：

1. 没有定义算法；
2. 没有定义覆盖范围；
3. 没有定义计算顺序；
4. 没有定义 loader 的验证行为；
5. `0` 作为唯一合法语义没有实际信息价值；
6. 如果未来不同实现自行选择算法，会造成格式兼容问题。

因此：

> **TNX v1 不定义 Checksum。**

未来如果需要完整性验证，必须先定义完整算法与语义，再通过新的格式版本加入。

完整性验证与数字签名也不得混为一谈。

---

# 17. 为什么删除 Reserved

早期 Header 曾包含：

```c
UINT64 Reserved[4];
```

该设计被否决。

TNX 已经拥有：

```text
Version
HeaderSize
```

因此未来扩展可以采用：

```text
旧字段保持不变
        ↓
新增 Header 字段
        ↓
HeaderSize 增大
        ↓
Version 更新
```

无语义的 Reserved 字段只会成为未来兼容性的负担。

因此：

> **TNX v1 不保留无明确用途的 Reserved Header 空间。**

Section 中已有的 `Reserved` 字段仅用于保持当前 Section Table 的固定布局；它不具有未来语义，必须写入零值，未来若需要修改其含义必须通过版本机制处理。

---

# 18. 工具链

TNX 不要求新的编译器或链接器。

推荐流程：

```text
clang
  ↓
ELF64 object
  ↓
lld
  ↓
ELF64 executable
  ↓
tnxpack
  ↓
TNX
```

例如：

```text
clang -target x86_64-unknown-none -ffreestanding -c foo.c
        ↓
foo.o

lld -m elf_x86_64 -T tnddos.ld
        ↓
foo.elf

tnxpack foo.elf
        ↓
FOO.TNX
```

`tnxpack` 负责：

* 读取 ELF64
* 获取程序映像信息
* 获取必要 Section/Segment
* 建立 TNX Section Table
* 生成 TNX Header
* 写入 TNX 文件

TNX 不重新实现编译器。

---

# 19. SDK

完整 TNDOS SDK 应最终包含：

```text
TNDOS-SDK/
├── include/
│   ├── tnd.h
│   ├── tnx.h
│   └── drv.h
│
├── lib/
│   └── drvlib.c
│
├── crt/
│   └── _start.c
│
├── linker/
│   └── tnddos.ld
│
├── tools/
│   └── tnxpack/
│
└── examples/
```

SDK 的目的之一是让第三方程序和驱动能够：

```text
树外构建
```

而不需要克隆 TNDOS kernel 源码。

驱动 SDK 独立化是 TNDOS 驱动模型成熟度的重要标志。

但 SDK 的完整建设不属于 TNX loader 的前置条件。

---

# 20. 实施顺序

TNX v1 的实施顺序冻结为：

```text
① 冻结 TNX v1 Specification
   + API ABI
   + 地址空间

        ↓

② tnxpack
   Windows 独立验证
   不依赖 TNDOS kernel

        ↓

③ PMM 增加 TNX 固定窗口保留
   +
   TNDOS TNX loader

        ↓

④ 第一个 Hello World TNX

        ↓

⑤ 建立完整 TNDOS-SDK

        ↓

⑥ 驱动 SDK 独立化
   + 树外构建样例驱动
```

其中：

> **TNX v1 Specification + ABI 是唯一关键路径上的设计阻塞项。**

其余部分均可在规格冻结后并行推进。

---

# 21. 已否决方案

以下方案不属于 TNX v1：

| 方案                    | 结论            |
| --------------------- | ------------- |
| 动态 Import             | 否             |
| Export Table          | 否             |
| Relocation            | 否             |
| TLS                   | 否             |
| Resource Directory    | 否             |
| Debug/Symbol metadata | 否             |
| Checksum 占位字段         | 否             |
| Reserved Header 空间    | 否             |
| 为 SYSCALL 提前退出 UEFI   | 否             |
| 现在立即建设完整 SDK          | 否             |
| 为了省当前代码而继续依赖 PE       | 不作为 TNX 的成立理由 |

---

# 22. 冻结原则

TNX v1 的核心不是功能数量，而是边界。

如果某项功能导致：

```text
loader 开始解析复杂结构
```

或者：

```text
loader 开始寻找外部符号
```

或者：

```text
loader 开始处理动态链接关系
```

或者：

```text
loader 开始理解 RSRC 内部格式
```

那么该功能已经开始违背 TNX v1 的设计目标。

TNX v1 应始终保持：

```text
固定地址
    ↓
固定映像
    ↓
简单 Section
    ↓
API Vector
    ↓
直接执行
```

**TNX 的复杂度应当留在工具链和 SDK，而不是 loader。**
