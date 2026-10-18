# TNX 程序崩溃 —— 完整现场与已排除项

**日期：** 2026-10-18
**被测环境：** QEMU q35 + OVMF（`C:\qemu\share\edk2-x86_64-code.fd`），512 MB
**状态：** 未解决。这是唯一挡住发布的问题。

---

## 一、症状

任何 TNX **程序**（不是驱动）一旦调用 `tnd_printf` 且参数里有 **64 位格式化**
（`%p`、或长数字），就会 page fault：

```
*** CPU EXCEPTION ***
  vector     : 14   page fault
  error code : 0x0000000000000000
  RIP        : 0x0000000002800F30
  CS         : 0x0000000000000038
  RFLAGS     : 0x0000000000010202
  CR2        : 0xFFFFFFFFFFC7EBC4D      <- 每次运行都一模一样
```

**关键点：`CR2` 是常量。** 跨多次重新编译、跨基址改动、跨栈改动，都是同一个值。
这不是随机溢出。

程序屏幕上的最后输出（`hello`，见下）：

```
   Hello from a TNX program (API v2)
  ================================================
  image addr    : 0x00000000028024F0
  fixed base inside the program window -> OK
  [rodata] read-only section is live    -> OK
  data section read/write               -> OK
  bss zero-filled by loader             -> OK
  kernel heap reachable                 -> OK
  argc = 1
  *** CPU EXCEPTION ***
```

**`argc = 1` 是打过补丁之后才出来的**（见第四节）。在那之前，崩在同一个 `argc = 1` 那一行。

---

## 二、崩溃点反汇编

`obj\execom.elf`，地址 `0x2800F30`（`o_uint` 内）：

```asm
2800f0b:  8b 45 ac        movl  -0x54(%rbp), %eax     ; i
2800f0e:  3b 45 e4        cmpl  -0x1c(%rbp), %eax     ; pad
2800f11:  7d 19           jge   0x2800f2c
2800f13:  48 8b 7d f8     movq  -0x8(%rbp), %rdi      ; o
2800f17:  be 30 00 00 00  movl  $0x30, %esi           ; '0'
2800f1c:  e8 0f fe ff ff  callq 0x2800d30 <o_ch>
2800f21:  8b 45 ac        movl  -0x54(%rbp), %eax
2800f24:  83 c0 01        addl  $0x1, %eax
2800f27:  89 45 ac        movl  %eax, -0x54(%rbp)
2800f2a:  eb df           jmp   0x2800f0b
2800f2c:  eb 00           jmp   0x2800f2e
2800f2e:  8b 45 bc        movl  -0x44(%rbp), %eax     ; n     <- 0x2800F30 落在这条
2800f31:  89 c1           movl  %eax, %ecx
2800f33:  83 c1 ff        addl  $-0x1, %ecx
2800f36:  89 4d bc        movl  %ecx, -0x44(%rbp)
2800f39:  83 f8 00        cmpl  $0x0, %eax
2800f3c:  74 14           je    0x2800f52
2800f3e:  48 8b 7d f8     movq  -0x8(%rbp), %rdi
2800f42:  48 63 45 bc     movslq -0x44(%rbp), %rax
2800f46:  0f be 74 05 c0  movsbl -0x40(%rbp,%rax), %esi
2800f4b:  e8 e0 fd ff ff  callq 0x2800d30 <o_ch>
2800f50:  eb dc           jmp   0x2800f2e
2800f52:  48 83 c4 60     addq  $0x60, %rsp
2800f56:  5d              popq  %rbp
2800f57:  c3              retq
```

**`CR2 = 0xFFFFFFFFFFC7EBC4D` 等于 `rbp - 0x44`，所以 `rbp = 0xFFFFFFFFFFC7EBC91`。**
换句话说：**`o_uint` 一进来 `rsp` 就已经是坏的**（`o_uint` 的序言是 `push rbp; mov rbp, rsp`）。

**注意矛盾**：同一个 `o_uint`，`%d`（`pad = 0`，跳过 0x2800F0B..0x2800F2A 那个补零循环）**能跑通**，
而 `%p`（`pad = 16`，要走那个循环）**崩**。也就是说 `rbp` 在补零循环**之后**才变成野值 ——
可那一圈循环里唯一的写操作是 `o_ch` 的 `o->buf[o->len] = c` 和 `o->len++`。

---

## 三、源码

`repos/TNDOS-SDK/lib/tndrt.c`：

```c
typedef struct { char *buf; tnd_i64 cap; tnd_i64 len; } OUT;

static void o_ch(OUT *o, char c) { if (o->len < o->cap - 1) o->buf[o->len] = c; o->len++; }

static void o_uint(OUT *o, tnd_u64 v, int base, int upper, int pad) {
    char tmp[24]; int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    for (int i = n; i < pad; i++) o_ch(o, '0');
    while (n--) o_ch(o, tmp[n]);
}

void tnd_printf(const char *fmt, ...) {
    static char buf[512];       /* 原来是栈上的 char buf[512]，改成 static 是为了排除栈溢出 */
    OUT o;
    va_list ap;
    o.buf = buf; o.cap = (tnd_i64)sizeof(buf); o.len = 0;
    va_start(ap, fmt);
    /* ... %d %u %x %p 分别走 o_int / o_uint ... */
    va_end(ap);
    if (o.len >= o.cap) o.len = o.cap - 1;
    buf[o.len] = 0;
    tnd_puts(buf);
}
```

`o_uint` 的 `tmp[24]` 对 64 位十进制（最多 20 位）和十六进制（最多 16 位）都够。
`o_ch` 有上界检查。**肉眼看不出这个函数有问题。**

---

## 四、已经排除的

| 项 | 怎么排的 | 结论 |
|---|---|---|
| 两份 `tnd_api.h` 不一致 | SHA256 比对 | **完全一致**（14730 字节，同一个 hash） |
| `TNX_ENTRY` 的 ABI 声明错了 | 读 `tnx.c:34` | 是 `typedef int (TND_ABI *TNX_ENTRY)(...)`，正确 |
| 程序基址不可用 | 开机探测 | `0x1000000` taken，`0x2800000` **FREE**，已改用后者 |
| 程序找不到 / 加载失败 | 屏幕与串口 | 加载成功，`image addr: 0x28024F0` |
| 无分页导致地址不可访问 | 设计如此 | 虚拟 == 物理，程序窗口直接可写 |
| `tnd_printf` 的栈缓冲撑爆内核栈 | 改成 `static` | **这一步有效** —— `argc = 1` 从"崩"变成"打印出来" |

**最后一行很重要**：把 `buf[512]` 从栈改到 `.bss` 之后，**崩溃点从 `%d` 移到了 `%p`**。
所以栈布局确实在影响这个 bug，但它不是全部原因。

---

## 五、加载链

`src/kernel/tnx.c`：

```c
TNX_ENTRY entry = (TNX_ENTRY)(UINTN)(imageBase + entryRva);
void *stackTop = (void *)(UINTN)(imageBase + TNX_WINDOW_SIZE - 64);
log_kv_u64("tnx.stack.top", (UINT64)(UINTN)stackTop);
api_setup(gRunArgc, gRunArgv);
rc = tnx_call_on_stack(stackTop, (void *)entry, api_get_table());
```

```c
/* 参数走 MS ABI（内核是 windows-msvc 编的）：rcx=栈顶, rdx=入口, r8=api。 */
__attribute__((naked)) int tnx_call_on_stack(void *sp, void *fn, const void *api) {
    __asm__ __volatile__(
        "pushq %rbx\n"
        "movq  %rsp, %rbx\n"
        "movq  %rcx, %rsp\n"
        "andq  $-16, %rsp\n"
        "movq  %r8,  %rdi\n"
        "callq *%rdx\n"
        "movq  %rbx, %rsp\n"
        "popq  %rbx\n"
        "retq\n"
    );
}
```

**这段 `tnx_call_on_stack` 是本次为了修这个 bug 新加的，但没有任何可观察的效果** ——
加上它之前和之后，`RIP` 和 `CR2` 完全一样。
**我没有验证过它到底有没有被调用**（串口里加的那行 `tnx.stack.top` 在最近一次启动中没有出现，
原因也没查）。这一点必须当成"可能根本没生效"来对待。

窗口常量：`TNX_PROG_BASE = 0x2800000`，`TNX_WINDOW_SIZE = 0x800000`，
`kWindows[] = { TNX_PROG_BASE, TNX_DRIVER_BASE }`。

---

## 六、环境值得注意的几个事实

1. **内核用 MS ABI 编译**（`--target=x86_64-pc-windows-msvc`），**TNX 程序用 SysV**
   （`TND_ABI` = `__attribute__((sysv_abi))`）。跨界调用点都用 `TND_ABI` 的函数指针。
2. **没有分页**：虚拟地址 == 物理地址。
3. **程序窗口 8 MiB，链接在 `0x2800000`**，映像只有几 KB，所以整个窗口大半是空的。
4. **程序跑在"内核调进来"的栈上** —— 本来连自己的栈都没有，`tnx_call_on_stack` 是想给它一条。
5. **`ff.c`（FatFs）和内核在同一批源里一起编**，用的是同一个 `windows-msvc` 目标。

---

## 七、给外部专家的问题

1. **`CR2` 是常量**，意味着 `rbp` 被算成了一个固定值 `0xFFFFFFFFFFC7EBC91`。
   什么情况下会得到"某个值的按位取反 + 小偏移"这种形状？
   （`~0x381436F` —— 但 `0x381436F` 本身不是任何我们认识的常量。）
2. **`%d` 通、`%p` 崩**，两者只差一个补零循环。`o_ch` 里唯一的写是
   `o->buf[o->len] = c`（`buf` 现在是 `.bss` 里的 `static char[512]`）和 `o->len++`。
   `o` 是 `tnd_printf` 栈帧里的 `OUT`，通过指针传进来。**有没有可能是 `o` 本身失效？**
3. **MS ABI 内核 + SysV 程序**，用 `naked` + 内联汇编切栈再 `callq`。
   **这个切换写法在 MS 目标上对不对？** 有没有更可靠的做法？
4. `-O2` + `__attribute__((naked))` 在 clang 的 MS 目标上，序言会不会被插入？
5. 有没有什么工具能在这里帮上忙 —— 比如在 QEMU 里挂 gdb，
   断在 `0x2800F30`，直接看 `rbp` 是谁写进去的？

---

## 八、当前可用的东西

- **`SAY.TNX`**：一个最小诊断驱动，把"驱动模型是否跑通"打印成五行。程序侧没有对应的东西。
- **`TYPE` 命令**：走 FatFs 读文件，能把返回码和长度打出来。
- **开机基址探测**：`pmm.c` 里会挨个试候选地址并报告哪个 FREE（`[probe]` 前缀）。
- **串口日志**：QEMU `-serial file:serial.log`，所有 `[log]` 行都在里面。

**这套观测手段是好的 —— 问题是这次我没用好它们**：我在没确认 `tnx_call_on_stack` 是否生效的情况下，
就基于"栈已经切好了"往下推了两轮。**如果外部专家接手，请先验证这一点。**

---

## 九、我的猜想（按可能性排序，每条附一个**能证伪的测试**）

### 猜想 1：`o` 这个 `OUT*` 指针在补零循环之后失效了

`o_ch` 唯一的写是 `o->buf[o->len]` 和 `o->len++`。
`buf` 现在是 `.bss` 里的静态数组（512 字节），`o` 是 `tnd_printf` 栈帧里的局部变量。
**如果 `o` 指向的栈帧被别的东西踩了**，`o->len++` 就会写到一个随机位置。

- **证伪测试**：把 `OUT` 也改成 `static`（全局），`o_ch` 里不再写栈。
  如果崩点消失 → 猜想成立。

### 猜想 2：`tnx_call_on_stack` 根本没被调用，或序言破坏了 rsp

**这一条我自己就觉得最可疑** —— 加上它之后 `RIP`/`CR2` 一个字都没变，
而且我**从没验证过它真的执行了**。

- **证伪测试**：在 `tnx_call_on_stack` 的第一条指令之前插一个不可省略的副作用
  （比如写一个全局变量），启动后检查那个变量。或者直接反汇编 `k.efi`，
  找到这个符号，看 `-O2` 有没有给它插序言。

### 猜想 3：`rbp` 是从一个按位取反的值来的

`rbp = 0xFFFFFFFFFFC7EBC91`，`~rbp = 0x381436F`（58,802,031）。
这个数**不是**任何我们认识的常量（不是窗口大小 0x800000，不是映像大小，不是栈顶 0x2FFFFC0）。
但"取反 + 小偏移"这种形状通常来自 **负数索引** 或 **`0 - x`**。

- **证伪测试**：在 QEMU 里挂 gdb，断在 `0x2800F30`，
  用 `watch *(long*)$rbp` 看是谁写的；或者单步进 `o_uint` 的序言看 `rsp` 从哪来。

### 猜想 4：MS ABI 调 SysV 的 `callq` 之后，栈对齐错 8 字节

`o_uint` 里有 `callq o_ch`。SysV 要求调用时 `rsp % 16 == 0`（进入函数时是 8）。
如果切栈之后对齐差 8，某些编译器生成的 `movaps` 会炸 ——
但**这里没有 SSE 指令**，而且报的是 page fault 不是 #GP。**所以我不太信这条。**

### 猜想 5：`hello` 自己在 `tnd_argc()` 之后踩了栈

`argc = 1` 打印出来之后，下一句是 `tnd_printf("   argv ptr = %p", ...)`。
**如果 `tnd_argv(0)` 返回的指针本身是好的，那就是 `tnd_printf` 挂的。**
我改过一版让它先打指针（不解除引用），**结果还是崩在同一个地址** ——
所以**不是**"解引用坏指针"，是格式化本身。

- **证伪测试**：把那条 `%p` 换成一个固定常量 `0x1234` 再跑。
  如果还崩 → 与参数无关，是 `tnd_printf` 自己的问题。

---

## 十、现场数据（完整串口见 `serial.log`）

```
[log] pmm: reserved 2048 pages @ 0x0000000002800000  (claimed from the firmware)
[log] pmm: reserved 2048 pages @ 0x0000000002000000  (claimed from the firmware)
[probe] program-window candidates (8 MiB each):
[probe]   0x0000000001000000  taken        <- 固件占着，这一条是最初的问题
[probe]   0x0000000002800000  taken        <- 现在是我们占了（窗口已经拿到）
[probe]   0x0000000003000000  ** FREE **
[probe]   0x0000000003800000  ** FREE **
[probe]   0x0000000004000000  ** FREE **
[probe]   0x0000000000800000  taken
[probe]   0x0000000006000000  ** FREE **
[probe] done
```

**这张表说明：程序窗口已经成功拿到，崩溃与"窗口不可用"无关。**

---

## 十一、重要更正与新的定位（2026-10-18 晚，外部评审之后）

### 更正 1：我之前抄错了 CR2

**之前写的**：`0xFFFFFFFFFFC7EBC4D`（17 位十六进制，68 位 —— 不可能是 64 位寄存器）

**串口里的原始值**（`log_kv_u64("exc.cr2", cr2)`，十进制）：

```
[log] exc.vector  = 14
[log] exc.errcode = 0
[log] exc.rip     = 41946928        = 0x2800F30
[log] exc.cs      = 56
[log] exc.rflags  = 66118
[log] exc.cr2     = 18446744072768711757 = 0xFFFFFFFFC7EBEC4D
```

**正确的 CR2 是 `0xFFFFFFFFC7EBEC4D`。** 外部评审说"这个数值需要重新核对"是对的，
是我从截图抄错了（多一个 F、E 抄成 C）。

### 更正 2：我之前反汇编的是**别的程序**

**崩的是 `hello`，我反汇编的是 `execom.elf`。** 两个程序都装在 `0x2800000`，
但函数布局完全不同 —— 拿 A 的地址解释 B 的崩溃是无效推理。

**用 `build/tnx/hello.elf` 重新反汇编 `0x2800F30`：**

```asm
0000000002800f10 <tnd_argc>:
 2800f10: 55                      pushq %rbp
 2800f11: 48 89 e5                movq  %rsp, %rbp
 2800f14: 48 8b 05 25 16 00 00    movq  0x1625(%rip), %rax   # 0x2802540 <gApi>
 2800f1b: 48 85 c0                testq %rax, %rax
 2800f1e: 74 0c                   je    0x2800f2c
 2800f20: 48 8b 40 28             movq  0x28(%rax), %rax
 2800f24: 48 85 c0                testq %rax, %rax
 2800f27: 74 03                   je    0x2800f2c
 2800f29: 5d                      popq  %rbp
 2800f2a: ff e0                   jmpq  *%rax              # 尾调用
 2800f2c: 31 c0                   xorl  %eax, %eax
 2800f2e: 5d                      popq  %rbp
 2800f2f: c3                      retq

0000000002800f30 <tnd_argv>:            <-- RIP = 0x2800F30 就是这里
 2800f30: 55                      pushq %rbp              <-- 崩在这条
 2800f31: 48 89 e5                movq  %rsp, %rbp
 2800f34: 48 8b 05 05 16 00 00    movq  0x1605(%rip), %rax   # 0x2802540 <gApi>
 ...
```

**结论（这一条是新的、硬的）：**

`pushq %rbp` 会写 `rsp - 8`。`CR2 = 0xFFFFFFFFC7EBEC4D`，所以
**进入 `tnd_argv` 时 `rsp` = `0xFFFFFFFFC7EBEC55`** —— 一个野值。

**而 `tnd_argc`（`0x2800F10`）刚刚才成功返回**（它是尾调用，`popq %rbp` 之后
`jmpq *%rax`，返回地址直接回到调用者）。

**两者之间只有一句 `tnd_printf("  argc = %d", ac)`。**

**所以：`tnd_printf` 返回时 `rsp` 已经坏了。**

### 更正 3：`exc.rsp` / `exc.rbp` 是处理器的，不是现场的

我新加了这两项日志，但**读数位置错了**：

```c
__asm__ __volatile__("movq %%rsp, %0" : "=r"(rsp));   /* 在 trap_common 里 */
```

读到的是**异常处理器自己的栈**（`0x3000098`，在内核栈上），不是出错那一刻的。
原因：`trap.c` 压栈的 frame 只有 `errcode / RIP / CS / RFLAGS` 四项，
**根本没有保存 `rsp` / `rbp`**。

要拿到现场栈指针，必须**在汇编入口里把 `rsp`/`rbp` 一起压进 frame**，
不能在 C 里读。**这一条已经确认是设计缺陷，不是读错了寄存器。**

### 现在的最强假设

`tnd_printf` 的序言是：

```asm
00000000028000d0 <tnd_printf>:
 28000d0: 55             pushq %rbp
 28000d1: 48 89 e5       movq  %rsp, %rbp
 28000d4: 41 57          pushq %r15
 28000d6: 41 56          pushq %r14
 28000d8: 41 55          pushq %r13
 28000da: 41 54          pushq %r12
```

它保存了 `rbp` 和 `r12..r15`。**如果它有一条提前 `ret` 绕过了收尾**，
`rsp` 就会停在半路 —— 而且**偏差会是固定的**（与"CR2 是常量"吻合）。

**下一步就是把这个函数的完整反汇编读完，逐条找有没有不配对的 `addq $..., %rsp` 或提前 `ret`。**

---
