# TNDDOS 0.3.2-M4-SP2

**TNX 到 1.1 了；ASCII 换成了真正的点阵字体；盘符能挂多个了。**

---

## 这一版做了什么

### 1. TNX 1.1 —— header 加架构标识

`Machine` 字段进了 header（头长 48 -> 56）。

```
0x01 AMD64     0x02 IA32       0x03 ARMv7       0x04 AArch64
0x05 RISCV32   0x06 RISCV64    0x07 LOONGARCH32 0x08 LOONGARCH64
0x09 IA64
0x0A-0x7F 保留（需要就提 Issue）   0x80-0xFF 实验（永久不登记）
```

**加载器只认本机**，不等于本机的一律拒绝 —— 而且拒绝得有用：

```
REJECTED: this is a LoongArch program; this kernel is AMD64 -- rebuild it for AMD64
```

说清了**这是什么**、**本机是什么**、**该怎么办**。「不支持的架构」那种话让人无从下手。

**v1.0 的文件按 AMD64 处理，不是「按本机」** —— TNX 至今只在 AMD64 上产生过二进制，
别的架构接受它等于接受一个必然跑不了的映像。

### 2. ASCII 换用点阵字体

`v`/`y` 分不清、`O`/`0` 分不清（提示符 TNDOS 看起来像 TND05）—— 现在不会了。

**根因不是字体不好，是用错了一类字体**：Cascadia Mono 是轮廓字体，12px 光栅化到
8x16 本身就会掉细节；而现代字体靠 **OpenType 的可选字形**做消歧（斜零、带尾的 l），
**GDI 这条路应用不了那些特性**。

换成 Unifont —— 它是**为 8x16 画的**点阵字体。它的 `.hex` 每个字形就是 16 字节，
和 TNDF 的半角字形**位宽完全相同**，直接搬，不需要重新光栅化。

**汉字那一半没动** —— 16x16 对汉字够用，而且汉字不靠细节区分。

### 3. 多盘符挂载

新增磁盘会被自动挂成 `D:` `E:` `F:` ……，`VOL` 列出全部，敲 `D:` 直接切换。

```
C:\EFI\TNDOS>vol
  C:  TNDDOS        (UEFI Simple File System / FAT)
  D:  (no label)    (UEFI Simple File System / FAT)
  E:  (no label)    (UEFI Simple File System / FAT)

C:\EFI\TNDOS>d:
D:\>dir
Volume D:   Directory of \
```

**这一层不解析 MBR / GPT** —— 走的是 EFI_SIMPLE_FILE_SYSTEM，分区表是固件解析好的：
能挂载的卷才会以句柄出现，MSR / 未格式化 / 非 FAT 的**根本不会出现**。
所以「MSR 拒绝挂载」是天然成立的。自己解析分区表是换掉固件那次的事。

### 4. 顺手修的

- **TNX 命令的版本打印是三段式**，而编码是两段式 —— `0x00010001` 会被打成 `1.0.1`，而它是 1.1
- **`TND_MAX_DRIVES` 只有 4** —— 一块盘上多个分区就会撑爆
- **SP1 那次把几个源文件的 UTF-8 BOM 弄丢了** —— 补回来

---

## 实测

```
$ tnx hello.tnx        version 1.1 / header 56 bytes / machine 0x01 AMD64
$ badarch.tnx          REJECTED: this is a LoongArch program; ...
$ vol                  C: / D: / E: 三个盘符
$ d: 然后 dir          Volume D:   Directory of \
```

（`BADARCH.TNX` 和 `HELLO.TNX` 一样是 9620 字节 —— 改了机器码**大小没变**。
这正好是 CIH 赖以藏身的那条性质。）

---

## 已知限制（说清楚，不含糊）

- **一次一个程序，ring 0，没有内存隔离** —— 程序崩了系统就崩
- **没有 `ExitBootServices`** —— 固件的中断和异常处理还在
- **中文只能显示，不能输入**；光标不闪
- **中文字形偏糙** —— 16x16 光栅化的水平，抗锯齿留到以后
- **只在 QEMU + OVMF 上验证过**
- **`autoexec.bat` 每次开机都在真实 ESP 上写** —— 上真机前请注释掉

---

## 下一版

IDT + 异常处理（第一次真正写架构相关代码，顺带立 `src/arch/`），
然后是自有 FAT 驱动 —— 那才是 MBR / GPT 真正要自己解析的时候。

<details>
<summary>English</summary>

TNDDOS 0.3.2-M4-SP2

**TNX has reached 1.1; ASCII now uses a real bitmap font; drive letters have multiplied.**

---

## What this version does

### 1. TNX 1.1 -- an architecture id in the header

A `Machine` field joins the header (48 to 56 bytes).

```
0x01 AMD64     0x02 IA32       0x03 ARMv7       0x04 AArch64
0x05 RISCV32   0x06 RISCV64    0x07 LOONGARCH32 0x08 LOONGARCH64
0x09 IA64
0x0A-0x7F reserved (raise an Issue)   0x80-0xFF experimental (never registered)
```

**The loader accepts the native machine only**, and refuses everything else -- usefully:

```
REJECTED: this is a LoongArch program; this kernel is AMD64 -- rebuild it for AMD64
```

It says **what this is**, **what we are**, and **what to do about it**. "Unsupported
architecture" gives you nowhere to go.

**A v1.0 file is treated as AMD64, not as native** -- TNX has only ever produced AMD64
binaries, and any other kernel accepting one would be accepting an image it cannot run.

### 2. ASCII now uses a bitmap font

You could not tell `v` from `y`, or `O` from `0` (the prompt read as TND05) -- that is over now.

**The cause was not a bad font but the wrong kind of font**: Cascadia Mono is an outline font,
and rasterising it to 8x16 at 12px loses the detail that distinguishes those glyphs. Modern
fonts rely on **OpenType stylistic sets** for disambiguation (slashed zero, tailed l), and
**GDI cannot apply those features on this path**.

So it is now Unifont -- a bitmap font **drawn for 8x16**. Each of its `.hex` glyphs is 16 bytes,
**exactly the width of a TNDF half-width glyph**, so the bitmap is copied directly rather than
rasterised a second time.

**The CJK half is untouched** -- 16x16 is enough for Han characters, and Han characters do not
depend on fine detail to be told apart.

### 3. Multiple drive letters

Newly attached disks are mounted automatically as `D:`, `E:`, `F:` and so on; `VOL` lists them
all, and typing `D:` switches to one.

```
C:\EFI\TNDOS>vol
  C:  TNDDOS        (UEFI Simple File System / FAT)
  D:  (no label)    (UEFI Simple File System / FAT)
  E:  (no label)    (UEFI Simple File System / FAT)

C:\EFI\TNDOS>d:
D:\>dir
Volume D:   Directory of \
```

**This layer does not parse MBR or GPT** -- it goes through EFI_SIMPLE_FILE_SYSTEM, where the
partition table has already been parsed by the firmware: only mountable volumes appear as
handles at all, and MSR, unformatted or non-FAT partitions **never appear**. So "MSR cannot be
mounted" holds by construction. Parsing partition tables ourselves belongs to the release that
replaces the firmware.

### 4. Fixed along the way

- **The TNX command printed versions in three parts** while the encoding has two --
  `0x00010001` displayed as `1.0.1` when it is 1.1
- **`TND_MAX_DRIVES` was only 4** -- a single disk with a few partitions exhausts that
- **The previous release dropped the UTF-8 BOM** from a few source files -- restored

---

## Measured

```
$ tnx hello.tnx        version 1.1 / header 56 bytes / machine 0x01 AMD64
$ badarch.tnx          REJECTED: this is a LoongArch program; ...
$ vol                  C: / D: / E:, three drive letters
$ d: then dir          Volume D:   Directory of \
```

(`BADARCH.TNX` is 9620 bytes, the same as `HELLO.TNX` -- the machine code changed and **the size
did not**. That is precisely the property CIH relied on to hide.)

---

## Known limitations (stated plainly, not hedged)

- **One program at a time, ring 0, no memory isolation** -- a crashing program takes the system down
- **No `ExitBootServices`** -- the firmware's interrupt and exception handling is still in place
- **Chinese can be displayed but not typed**; the cursor does not blink
- **The Chinese glyphs are coarse** -- the level that 16x16 rasterisation gives; antialiasing comes later
- **Only ever verified on QEMU + OVMF**
- **`autoexec.bat` writes to the real ESP on every boot** -- comment those lines out before using real hardware

---

## Next version

IDT and exception handling (the first genuinely architecture-specific code, and a good moment to
establish `src/arch/`), then our own FAT driver -- which is when MBR and GPT genuinely have to
be parsed by us.

</details>

