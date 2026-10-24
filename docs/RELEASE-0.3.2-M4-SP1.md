# TNDDOS 0.3.2-M4-SP1

**中文上屏了。**

这一版把控制台从固件手里完整接了过来，并第一次让 TNDDOS 显示中文。
仍然是 UEFI-only、单任务、无保护 —— 但屏幕、字体、时间这三样现在是自己的。

前一版本尚未明确开发目的，未能确定成品，实属抱歉
---

## 这一版做了什么

### 1. 控制台有了后端概念

内核不再写死在 UEFI 的 ConOut 上。`TND_CONSOLE` 是一组可换的后端：

- `uefi` —— 原来的 ConOut，行为一字未改，只是搬到接口后面
- `fb` —— **自己往帧缓冲写像素**，自带点阵字体

用 `CONSOLE fb` / `CONSOLE uefi` 随时切换。

为什么重要：ConOut 是 Boot Services，`ExitBootServices` 之后它和它的字体一起消失；
而帧缓冲那块**内存**还在。先把输出接管过来，后面才谈得上出局。

### 2. 中文能显示了

配套四件东西：

- **字体生成器** `tools/mkfont.ps1` —— 用 Windows GDI 把 TTF 光栅化成点阵
  （ASCII 用 Cascadia Mono，中文用 Noto Sans SC，都是 SIL OFL 1.1，可以随项目分发）
- **字体格式 TNDF v1** —— 头 32 字节 + 按码点升序的索引 + 连续字形区
- **内核加载器** `lib/font.c` —— 从 ESP 读，二分查找
- **渲染** —— 8x16 半角 / 16x16 全角，统一格子高度 16

覆盖 21,427 个字形：ASCII、CJK 标点、假名、CJK 基本区、全角。约 836 KB。

### 3. 批量原语（API v2.4）

对照 Win32 控制台的 `WriteConsoleOutput` / `FillConsoleOutputCharacter` /
`ScrollConsoleScreenBuffer` / `GetConsoleScreenBufferInfo`：

```
tnd_write_cells(x, y, w, h, cells, stride)
tnd_fill(x, y, w, h, ch, attr)
tnd_scroll(x, y, w, h, dy, ch, attr)
tnd_screen(&sc)
```

**为什么必须有**：EDIT 这类程序每次重画几百个格子，逐格调用光是开销就吃掉一切。
这不是高级功能，是性能底线。

### 4. 定时器（不需要 IDT）

`ticks()` 现在返回**自启动以来的毫秒**。

关键认识：**读时基和等中断是两件事。** 读 TSC 是几条指令，不调用就零开销；
唯一费 CPU 的做法是"轮询循环"，而没那么做。周期性中断才需要 IDT —— 那是后面的事。

频率没法直接问，用固件的 `Stall` 校：它保证至少延时那么久，TSC 增量除以时间就是频率。

开关写进配置文件，`config.sys` 覆盖 `efidos.sys`：

```
efidos.sys    SET TNDDOS_TIMER=ON
config.sys    SET TNDDOS_TIMER=OFF     ; 关掉就取消注释
```

关掉时 `ticks()` **老实返回 0**，不给假值 —— 假的时间比没有时间更坏。

### 5. 顺手修掉的东西

- **EDIT 全屏错位**：`api_cls` / `api_gotoxy` / `api_cols` / `api_rows`
  绕过控制台服务层直连 ConOut，于是 EDIT 拿到 100x31（固件的网格）而不是 160x50
- **SF 缩放花屏**：字形只做了纵向缩放，横向没做，格子 9 宽而字形 8 宽，
  右边一列永远不写
- **滚动慢**：每滚一行重画全部 8000 格，每格还做一次二分查找。
  改成一次内存搬移 + 只重画最后一行 —— **启动快了一倍**
- **`api_getattr` 读过期值**：同一个坑

---

## 实测

```
[log] font: loaded \EFI\TNDOS\FONTS\CJK16.FNT  cell=16x8/16  glyphs=21427
[log] con_fb: grid=160x50 cell=8x16 scale=100%
[log] timer: TSC 1998812 cycles/ms  (~1998 MHz)
[log] batch.lines = 97
```

```
C:\EFI\TNDOS>edit tndos.txt     <- 正常全屏、中文可显示
C:\EFI\TNDOS>sf 1.5             <- 缩放正常
C:\EFI\TNDOS>time               <- uptime: 12345 ms (12 s)
```

---

## 已知限制（说清楚，不含糊）

- **一次一个程序，ring 0，没有内存隔离** —— 程序崩了系统就崩。这不是 bug，是当前状态
- **没有 `ExitBootServices`** —— 固件的中断和异常处理还在，所以我们崩了还能看到寄存器转储
- **中文只能显示，不能输入**
- **光标不闪** —— 定时器有了，但闪烁需要周期性动作，还没接
- **只在 QEMU + OVMF 上验证过**
- **`autoexec.bat` 每次开机都在真实 ESP 上写** —— 上真机前请注释掉那几行
- **字体美观性** —— 你真指望的上AI吗？？？

---

## 下一版

IDT + 异常处理（第一次真正写架构相关代码，顺带立 `src/arch/`），
然后是自有 FAT 驱动 —— **没有它就别想离开 UEFI**。

工程约定与完整功能清单见仓库里的 `CONTRIBUTING.md` 和 `TNDOS-目标清单.txt`。

<details>
<summary>English</summary>

# TNDDOS 0.3.2-M4-SP1

**Chinese is on screen.**

This release takes the console completely out of the firmware's hands, and for the
first time lets TNDDOS display Chinese. It is still UEFI-only, single-tasking and
unprotected -- but the screen, the font and the clock are now our own.

The previous version had not yet settled on its development goal and could not be
considered a finished product. Our apologies.

---

## What this version does

### 1. The console now has the notion of a backend

The kernel is no longer hard-wired to UEFI's ConOut. `TND_CONSOLE` is a set of
swappable backends:

- `uefi` -- the original ConOut. Not one behaviour changed; it was merely moved
  behind the interface.
- `fb` -- **writes its own pixels into the framebuffer**, with its own bitmap font.

Switch between them at any time with `CONSOLE fb` / `CONSOLE uefi`.

Why it matters: ConOut is a Boot Service, so it and its font disappear the moment
`ExitBootServices` runs -- while the framebuffer's **memory** is still there.
Taking over output first is what makes leaving UEFI conceivable at all.

### 2. Chinese can be displayed

Four pieces go together:

- **Font generator** `tools/mkfont.ps1` -- rasterises a TTF into a bitmap font
  using Windows GDI (Cascadia Mono for ASCII, Noto Sans SC for Chinese; both are
  SIL OFL 1.1, so they can be redistributed with the project).
- **Font format TNDF v1** -- a 32-byte header, an index sorted by codepoint, and a
  contiguous glyph area.
- **Kernel-side loader** `lib/font.c` -- reads it from the ESP and does a binary search.
- **Rendering** -- 8x16 for half-width, 16x16 for full-width, one uniform cell height of 16.

It covers 21,427 glyphs: ASCII, CJK punctuation, kana, the CJK Unified Ideographs block,
and full-width forms. About 836 KB.

### 3. Batch primitives (API v2.4)

Modelled on the Win32 console calls `WriteConsoleOutput` /
`FillConsoleOutputCharacter` / `ScrollConsoleScreenBuffer` /
`GetConsoleScreenBufferInfo`:

```
tnd_write_cells(x, y, w, h, cells, stride)
tnd_fill(x, y, w, h, ch, attr)
tnd_scroll(x, y, w, h, dy, ch, attr)
tnd_screen(&sc)
```

**Why they are mandatory**: a program like EDIT redraws several hundred cells at a time,
and one call per cell spends everything on overhead alone. These are not a luxury
feature; they are the floor for performance.

### 4. Timer (no IDT required)

`ticks()` now returns **milliseconds since boot**.

The key realisation: **reading a time base and waiting for an interrupt are two
different things.** Reading the TSC is a couple of instructions, and costs nothing
at all when it is never called; the only CPU-hungry approach is a polling loop, and
that is not what we do. A periodic interrupt is what needs an IDT -- and that comes later.

The frequency cannot simply be asked for, so it is calibrated against the firmware's
`Stall`: Stall guarantees a delay of at least that long, so the TSC delta divided by
the elapsed time is the frequency.

The switch lives in the configuration files, and `config.sys` overrides `efidos.sys`:

```
efidos.sys    SET TNDDOS_TIMER=ON
config.sys    SET TNDDOS_TIMER=OFF     ; uncomment to turn it off
```

When it is off, `ticks()` **honestly returns 0** rather than a plausible lie -- a fake
clock is worse than no clock.

### 5. Things fixed along the way

- **EDIT's full-screen layout was wrong**: `api_cls` / `api_gotoxy` /
  `api_cols` / `api_rows` bypassed the console service layer and called ConOut
  directly, so EDIT was told 100x31 (the firmware's grid) instead of 160x50.
- **SF scaling produced speckle**: glyphs were scaled vertically but not horizontally,
  so a cell 9 pixels wide got an 8-pixel glyph and the rightmost column was never written.
- **Scrolling was slow**: every scrolled line redrew all 8000 cells, each of them doing
  a binary search. It is now one memory move plus a redraw of the last row only --
  **boot is twice as fast.**
- **`api_getattr` read a stale value**: the same trap.

---

## Measured

```
[log] font: loaded \EFI\TNDOS\FONTS\CJK16.FNT  cell=16x8/16  glyphs=21427
[log] con_fb: grid=160x50 cell=8x16 scale=100%
[log] timer: TSC 1998812 cycles/ms  (~1998 MHz)
[log] batch.lines = 97
```

```
C:\EFI\TNDOS>edit tndos.txt     <- full screen, Chinese displays
C:\EFI\TNDOS>sf 1.5             <- scaling works
C:\EFI\TNDOS>time               <- uptime: 12345 ms (12 s)
```

---

## Known limitations (stated plainly, not hedged)

- **One program at a time, ring 0, no memory isolation** -- a crashing program takes
  the system down. This is not a bug, it is the current state.
- **No `ExitBootServices`** -- the firmware's interrupt and exception handling is still
  in place, which is why a crash still gives us a register dump.
- **Chinese can be displayed but not typed.**
- **The cursor does not blink** -- the timer exists, but blinking needs a periodic
  action and that is not wired up yet.
- **Only ever verified on QEMU + OVMF.**
- **`autoexec.bat` writes to the real ESP on every boot** -- comment those lines out
  before booting on real hardware.
- **Font aesthetics** -- were you really counting on an AI for that???

---

## Next version

IDT and exception handling (the first genuinely architecture-specific code, and a good
moment to establish `src/arch/`), then our own FAT driver -- **without it there is no
leaving UEFI.**

The engineering conventions and the full feature list are in `CONTRIBUTING.md` and
`TNDOS-目标清单.txt` in the repository.

</details>
