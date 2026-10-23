# TNDDOS 代码大审查 —— 骨架与已知蛀虫

**日期：** 2026-10-23
**触发原因：** TNX 程序在 tnd_argc/tnd_argv 上必崩，换了三种写法都不管用；怀疑内核侧也有结构性问题。

> 这份文档严格区分**已证实**和**仅怀疑**。结论部分只放已证实的。
> 前面几轮我多次把怀疑当证据用过（比如把 REJECTED 当成跑通），所以这里先立规矩。

---

## 一、已证实（有实测数据）

| 事实 | 证据 |
|---|---|
| 程序能加载、能进窗口 | 屏幕 image addr = 0x28024E0，落在 0x2800000..0x3000000 内 |
| tnd_printf 与 %p 正常 | fixed %p = 0x1234 打印成功 |
| rodata/data/bss/heap 四项自检全过 | hello 屏幕输出 OK |
| 崩溃点固定 | RIP 0x2800F30，CR2 0xFFFFFFFFC7EBEC4D，每次一样 |
| CR2 是本次缺页的 | 已在汇编桩第一条指令存入全局 gTrapCr2 |
| 0x28025A0 可读 | 探针实测读到 0x1DCC7680 |
| 分页是开着的 | CR0=0x80000033，CR3=0x1F808E00 |
| 程序窗口确实被拿到 | 串口 pmm: reserved 2048 pages @ 0x2800000 (claimed) |
| 换写法不管用 | 屏障 / static inline / 整个删掉，三种都崩 |
| 同特权级 #PF 不压 RSP/SS | x86-64 规范；trap 原来记错了 |

---

## 二、仅怀疑（未证实，按可能性排序）

### 怀疑 1：固件的页表并没有覆盖程序窗口的全部 8 MiB

pmm_reserve 用 AllocatePages(AllocateAddress) 把 0x2800000 的 8 MiB 要了过来。
这**只说明物理内存归我们了**，不说明**固件的页表把这段虚拟地址映射了**。
我们从来没建过自己的页表。

**为什么这条可疑**：CR2 是一个跟 gApi 无关的高地址；如果某个页确实没映射，
访问它就会缺页，而 RIP 报的是触发它的那条指令 —— 但实测两者对不上。

**怎么证**：在 #PF 处理器里手动走一遍 CR3 的四级页表，把 CR2 和
0x28025A0 各自的 PTE 打出来。这是唯一能一劳永逸定性的事。

### 怀疑 2：tnx_call_on_stack 的 ABI 边界不完整

它现在保存 rbx/rdi/rsi，切栈，call，再恢复。
但**没有保存 r12-r15，也没有保存 XMM6-XMM15**。Windows x64 要求这些
在跨调用时保持；SysV 程序可以随意覆盖它们。

**为什么这条可疑**：目前崩溃点不在返回之后，所以不像是直接原因；
但它是一颗明确的雷 —— 一旦被调函数用到 XMM，内核状态就会被踩。

### 怀疑 3：trap_pf 没有独立 IST 栈

现在 #PF 用的是被中断的那个栈。如果那个栈本身已经越界或损坏，
CPU 压异常帧会再触发一次 #PF，形成嵌套 —— 记录下来的信息全部不可信。

**这与最近的观察吻合**：所有基于 RIP 的推断都互相矛盾。

### 怀疑 4：SDK 的包装层设计有问题

约 45 个函数，35 个是 return gApi->x(...)。这种形状在 -O2 下会被编成尾调用。
我们给三个加了屏障，其余没加。**尾调用对这个运行时是否根本不能用，尚未证实。**

---

## 三、审查清单（按性价比排序）

### P0 —— 做完就能定性

1. **在 #PF 处理器里走页表**，打印 CR2 和 0x28025A0 的 PTE（P/W/US 位、物理地址）。
   20 行代码，一次启动。这是所有矛盾的总开关。
2. **给 #PF 配独立 IST 栈**：改 IDT gate 的 IST 索引 + TSS 的 IST 表。
   如果换了 IST 栈之后 RIP/CR2 变得自洽，那前面所有矛盾都是嵌套异常造成的。

### P1 —— 明确的雷，顺手拆

3. tnx_call_on_stack 补上 r12-r15 和 XMM6-XMM15 的保存。
4. build-run.ps1 改成纯 ASCII（见 DEBUG-REPORT 雷 1）。
5. 构建脚本顶层 try/catch，失败信息写文件 + 最后一行打印状态（雷 2、雷 3）。

### P2 —— 结构整理

6. SDK 重写：去掉 tnd_argc/tnd_argv，其余按最小形状重写。
   **但要等 P0 做完** —— 否则新写的会带着同一个未知的坑。
7. hello 从 autoexec 里彻底移出，改成 selftest.bat（雷 4）。

---

## 四、关于内核是否已有蛀虫

**内核确实有雷，但目前没有证据说它已经坏了。**

已确认能工作的：NVMe 读写、FatFs、VFS、API 表、目录遍历、exitboot、
内存自检（PMM 和 heap 两项 PASS）。

**可疑但未证实的**：页表覆盖（怀疑 1）、异常入口栈（怀疑 3）、跨 ABI 的寄存器保持（怀疑 2）。

**这三条都集中在内存与异常这一层** —— 而这一层正好是没有任何自检的。

建议：**给页表加一个自检**，做法和 PMM/heap 的自检一样：随机取几个
我们声明归自己的地址，确认它们可读写。如果自检不过，怀疑 1 立刻成立。
---

## 五、内存管理层：审查结果与重写规格

**规模：pmm.c 340 行 + heap.c 197 行 = 537 行。重写是完全可行的量级。**

### 5.1 现状：值得保留的部分

pmm.c：
- 位图 + 首次适配，思路直接
- 只把 EfiConventionalMemory 算作可用（保守，安全）
- 位图自身占的页、低 1 MiB 都从可用里扣掉了
- pmm_reserve 返回 claimed，调用方检查（如果只标位图不检查，就是灾难）

heap.c：
- 按地址排序的单链表 + 首次适配。注释写明这是故意选的笨办法
- kfree 检查 magic、拦 double free
- 分配时切块，释放时前向合并
- heap_selftest：三块各写各的模式、互相不串 —— 实测 PASS

### 5.2 现状：必须修的

| 编号 | 问题 | 后果 |
|---|---|---|
| M1 | **heap_grow 不零化新页** | pmm_alloc_pages 给的是脏页；kmalloc 返回的缓冲含上一个人的数据。调用方假定为零就会读到垃圾 |
| M2 | heap_grow 失败路径漏页 | got <= HDR_SIZE 时 return 0，pmm 给的页没还 |
| M3 | list_merge_and_recount 每次 malloc/free 都 O(n) 重算 | 块数一多就是 O(n²)。当前 blocks=1，暂时无害 |
| M4 | HEAP_MIN_CHUNK 固定 64 KiB，无几何增长 | grow 次数一多就碎片化。当前 grow=1 |
| M5 | **没有自己的页表** | 完全依赖固件的恒等映射。这是结构性问题，不是 bug |
| M6 | pmm_reserve 的位图与固件所有权混淆 | claimed=0 时位图仍标死。安全（不发放），但语义不干净 |

**M1 是唯一有现实风险的一条**：如果哪个调用方假定 kmalloc 返回清零内存，
它现在拿到的是脏页。要确认这一点，得翻所有 kmalloc 的调用点。

### 5.3 重写规格（内存管理层）

**PMM 必须自己建页表。** 这是 M5 的解法，也是整个系统最该补的一块。

要求：
1. 从 GetMemoryMap 建恒等映射（2 MiB 大页，覆盖全部 RAM）
2. 用自己的 CR3，在 ExitBootServices **之前**装好（退出之后拿不到内存映射）
3. 提供 map(VA, PA, size) —— 程序窗口和驱动窗口以后靠它，不再依赖固件分配
4. 提供 walk(VA) -> PTE，供自检和调试用
5. 自检：随机取若干我们声明归自己的地址，确认可读写

**HEAP 必须在规格里写清三条**：
1. kmalloc **不保证清零**。需要清零的调用方自己 memset —— 或者提供 kzalloc
2. heap_grow 失败必须把页还回去
3. 账目重算允许 O(n)，但要写明这是设计选择，不是疏忽

### 5.4 顺序

```
1  PMM 建页表 + map + walk + 自检        （新的地基）
2  在 #PF 处理器里用 walk 打印 PTE        （用上一步的产物，把崩溃定性）
3  按结果决定 SDK 怎么改
4  HEAP 按上面的规格重写（可选的改进：kzalloc、几何增长）
```

**第 1 步做完，第 2 步就有工具了；第 2 步做完，SDK 的问题才有答案。**
---

## 六、页表走表（P0）已完成 —— 结论与遗留矛盾

**新增 `src/kernel/paging.c`（200 行）**：
- `paging_walk(va, out)`：走**当前** CR3（固件的表），只读。返回 Present/Level/Entry/Phys/PageSize
- `paging_build()`：建我们自己的恒等映射表（覆盖到 4 GiB 以上，含 MMIO），**故意不装**
- `paging_install()`：留了桩，注释写明装之前必须先解决 MMIO 缓存属性

**`trap.c` 同步改了**：CR2 在汇编桩第一条指令存进全局 `gTrapCr2`；`frame[4]/[5]` 不再当 RSP/SS；
桩从 `jmp` 改成真 `call` + 对齐 + shadow space；#PF 时自动走表并打印 CR2 / RIP / 0x28025A0。

### 6.1 走表给出的硬事实

```
CR2 = 0xFFFFFFFFC7EBEC45   PML4 表项 = 0   NOT PRESENT，level 4
RIP = 0x2800F30            PRESENT（2 MiB 页）
0x28025A0                  PRESENT
```

**这条否掉了三个此前的判断**：不是固件页表没覆盖窗口；不是 RIP 不可靠；
不是 gApi 住在只读段（gApi 在 DATA，flags=0x3，可写）。

### 6.2 新发现：0x2800F30 上是一条不可能缺页的指令

```asm
0000000002800f20 <tnd_env>:
   2800f20: movq gApi(%rip), %rax
   2800f27: testq %rax, %rax
   2800f2a: je   0x2800f3d
   2800f2c: movq 0x38(%rax), %rax      <- api->env，读 gApi+0x38
   2800f30: testq %rax, %rax           <- RIP 落在这里
```

**`testq %rax,%rax` 是寄存器测试，不访问内存，不可能缺页。**

**而 `CR2 - 0x38 = 0xFFFFFFFFC7EBEC0D`** —— 正好是 `0x2800F2C` 那条 `movq 0x38(%rax)` 的基址。
也就是说：**`gApi` 里是一个 32 位符号扩展出来的野值 `0xFFFFFFFFC7EBEC0D`**。

### 6.3 状态槽实验：结论互相矛盾，如实记录

在程序窗口最后 8 字节放了一个状态槽（栈从顶端 -64 往下长，这 8 字节不归栈用）。
内核调用前清零、调用后读回；`tnx_entry` 的第一条语句把收到的 `api` 写进去。

```
tnx.entry.expected = 499939552
tnx.entry.received = 499939552     match = 1    <- autoexec 里前 3 个程序
tnx.entry.received = 0             match = 0    <- hello
tnx.stack.present  = 1
tnx.slot.present   = 1
tnx.entry.present  = 1
```

**矛盾**：屏幕上明明打出了 `Hello from a TNX program` 和四项自检 OK，说明 `tnx_entry`
进去了、`tnx_main` 跑了；但状态槽是 0，说明那条写入没生效。
地址核对过：`__tnx_image_base = 0x2800000`，`+ 0x800000 - 8 = 0x2FFFFF8`，和内核读的完全一致。
**这一条目前解释不了，如实留着。**

### 6.4 下一步

1. `tnx_entry` 里先写 `0xDEADBEEF` 再写 `api`，看槽里出现哪个值 —— 能区分没执行和被覆盖。
2. 用 `paging_walk` 走 `0xFFFFFFFFC7EBEC0D`，确认它确实表项为 0。
3. `tndrt.c` 单独用 `-O0` 编一次，重做状态槽实验 —— 若 `-O0` 下 `received` 正确，
   就是编译器把那句 volatile 写入挪走了。
---

## 七、压力测试（STRESS.TNX）与 trap 的寄存器抓取

### 7.1 trap 现在会保存全部 15 个通用寄存器

以前只记 RIP / CR2，看对齐类异常时完全不够。现在桩里按固定顺序 push
rax,rbx,rcx,rdx,rsi,rdi,rbp,r8..r15，`trap_common` 收第三个参数 `gprs`
并把它们逐个写进日志。`frame` 仍指向硬件帧（ERR 桩 +120，NOERR 桩 +128）。

### 7.2 压力测试覆盖的第三方代码

| 库 | 来源 | 许可 | 结果 |
|---|---|---|---|
| MD5 | RFC 1321 附录参考实现 | 公有领域 | 两条标准向量都 PASS |
| AES-128 | tiny-AES-c | Unlicense | ECB 加密 PASS、CTR PASS、**ECB 解密 FAIL** |
| inflate | zlib 的 puff.c | zlib licence | rc=0、长度对、内容 MD5 对 —— PASS |

**AES 只有 `InvCipher` 坏**：加密对、CTR 对（CTR 也走 Cipher），
所以密钥扩展、S 盒、Cipher 都没问题，问题在逆 S 盒那一半。

### 7.3 新的未解异常：movdqa 的 #GP

```asm
2801f05: 66 0f 7f 98 f0 a5 80 02    movdqa %xmm3, 0x280a5f0(%rax)
2801f15: 48 83 c0 10                addq   $0x10, %rax
2801f19: 75 d5                      jne    0x2801ef0
```

实测寄存器：`rax=0`、`rbx=400`（load_md5 的 rounds）、`cr2=0`、
`frame_ptr=0x2FFFFC0`（**正好是 stackTop**）。

**两个矛盾**：
1. `rax=0` 时目标地址是 `0x280A5F0`，16 字节对齐 —— `movdqa` 不该报 #GP
2. `frame_ptr` 等于程序栈最顶端，而 `rbx=400` 说明已经深入 `load_md5` ——
   两者不可能同时成立

**和前面几次一样：RIP 与其余现场信息不自洽。**

### 7.4 另一个独立发现：TNX 里没有描述 .bss 的段

`STRESS.TNX` 只有 **一个** 段：

```
CODE  flags=0x5  rva=0  fileOff=0xB0  fileSize=0x5C62  memSize=0x5C62
ImageSize = 0xAA10
```

`ImageSize` 比 `CODE.MemSize` 大 `0x4DAE`，那部分就是 `.bss`。
加载器目前靠 `t_memzero(imageBase, ImageSize)` 清到 `ImageSize`，**碰巧是对的**；
但段表里没有任何东西描述它，`tnxdump` 之类工具会看不懂，
而且一旦有人把 `t_memzero` 改成"按段清"，`.bss` 立刻变脏。

**建议**：`tnxpack` 为 `.bss` 补一个 `BSS` 段（`FileSize=0`、`MemSize>0`），
或者明确扩展 `DATA` 段的 `MemSize` 覆盖到 `ImageSize`。

### 7.5 待查（按优先级）

1. 为什么 RIP 和其余现场信息总是不自洽 —— 给 #PF/#GP 配独立 IST 栈后重测
2. AES 的 `InvCipher` 为什么错（用 CTR 已把范围锁死在逆 S 盒那一半）
3. `tnxpack` 补 BSS 段
4. 控制台偶发丢字符（`decrypt =` 那行少一个十六进制字符）
---

## 八、客户机内存分布（QEMU `info mem`，CR3=0x1F801000）

```
虚拟地址范围                      大小        权限
0x00000000 - 0x1E800000       488 MB      rw     <- 主体：RAM 恒等映射
0x1E800000 - 0x1EA00000         2 MB      r-
0x1EA00000 - 0x1F6AD000       ~12 MB      rw
0x1F6AD000 - 0x1F6B0000        12 KB      r-     <- OVMF 私有页
0x1F6B0000 - 0x1F6EB000      ~250 KB      交错 r/rw
0x1F6EB000 - 0x1F800000      ~1.3 MB      rw
0x1F800000 - 0x1FE00000         6 MB      r-     <- 页表自己
0x1FE00000 - 0x10000000000      1 TB      rw     <- 兜底映射
```

**这张表一次性排掉了三条猜测**：

1. 程序窗口 `0x2800000..0x3000000` 落在第一段 rw 里 —— **全部映射、可读写**
2. 所以 `0x280A5F0` 上的 `#GP` **不可能是缺页**（`#PF` 这条路彻底关闭）
3. 那 `#GP` 只剩对齐；而 `0x280A5F0 + 29 = 0x280A60D` 确实不 16 对齐 —— 吻合

## 九、剩下的唯一矛盾，以及一步分辨的办法

反汇编（内存里读到的，和 ELF 一致）：

```asm
0x02801ed9: 48 c7 c0 00 f0 ff ff    movq $-4096, %rax     <- 循环起点
0x02801ef0: 66 0f 6f d8              movdqa %xmm0, %xmm3   <- 循环体
0x02801f05: 66 0f 7f 98 f0 a5 80 02  movdqa %xmm3, 0x280a5f0(%rax)
0x02801f15: 48 83 c0 10              addq $0x10, %rax      <- 每次 +16
0x02801f19: 75 d5                    jne  0x2801ef0
```

**`%rax` 只能是 `-4096 + 16k`，永远不是 29** —— 而实测 `exc.rax = 29/31/0`。

**两种可能，用一步就能分辨**：

```
A  控制流从别处跳进了 0x2801F05 这条指令中间
B  trap 报的 RIP 仍然不对（udtest 只验过 #UD/NOERR 桩，#GP/ERR 桩没单独验）
```

**分辨办法：QEMU 加 `-d int`**。它把每一次异常按顺序打进日志，
**数据来自 QEMU 的 CPU 模型，不经过我们的 trap** —— 独立第三方证据。

```
日志里的 RIP == trap 报的 RIP   ->  B 不成立，就是 A
日志里的 RIP != trap 报的 RIP   ->  B 成立，ERR 桩还有偏移 bug
```

### 如果是 A（控制流跑飞）

嫌疑按可能性排：
1. `tnx_call_on_stack` 的切栈/返回路径（naked asm，栈顶切到 `0x2FFFFC0`）
2. `setjmp`/`longjmp`（puff 用它做错误处理，已经踩过一次坑）
3. `md5_update` 里 `p` 指针的推进

验证：把 stress 拆成三个小程序，一个一个跑，看哪个先崩。

### 如果是 B（ERR 桩仍有 bug）

补一个 udtest2：**主动触发对齐 #GP**（不用 `ud2`），已知寄存器，再比一次。

## 十、今天新增的工具

| 工具 | 用途 |
|---|---|
| `UDTEST.TNX` | 设已知寄存器再 `ud2`，验证 trap 报告是否可信 |
| `STRESS.TNX` | MD5 / AES / inflate 三个真实开源库的压力测试 |
| QEMU `xp /Ni` | 直接反汇编客户机内存 |
| QEMU `xp /Nxb` | 直接 dump 客户机内存 |
| QEMU `info registers` | 当前寄存器（含 XMM、CR0/CR3/CR4/EFER） |
| QEMU `info mem` | 客户机页表映射区间 |
| QEMU `stop` | 暂停客户机，让内存状态稳定再读 |

**Visual Studio 用不了**：它只能调宿主进程或走调试传输调另一台机器，
没有 GDB remote 客户端；而 QEMU 只对外提供 GDB remote。
`lldb.exe` 也起不来 —— 缺 `python310.dll`（本机 Python 是 3.12，名字对不上）。
