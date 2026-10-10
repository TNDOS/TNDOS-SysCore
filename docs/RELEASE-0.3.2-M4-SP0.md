# TNDDOS 0.3.2-M4-SP0

**控制台从固件手里接过来了。M4 的第一步。**

---

## 这一版做了什么

### 1. 控制台有了后端概念

内核不再写死在 UEFI 的 ConOut 上。`TND_CONSOLE` 是一组可换的后端：

```
uefi   原来的 ConOut，行为一字未改，只是搬到接口后面
fb     **自己往帧缓冲写像素**，自带点阵字体
```

用 `CONSOLE fb` / `CONSOLE uefi` 随时切换。

**为什么重要**：ConOut 是 Boot Services，`ExitBootServices` 之后它和它的字体一起消失；
而帧缓冲那块**内存**还在。**先把输出接管过来，后面才谈得上出局。**

### 2. 有了自己的显示层

- 从 GOP 拿到帧缓冲基址、宽高、每扫描行像素数
- 自己维护一块**影子缓冲**（每格一个字符 + 一个属性），滚动、清屏、定位都在上面做
- 内嵌一份 8x12 点阵字体（从 `vgaoem.fon` 提的 256 个字形，CP437）

### 3. 顺手修的

- **shell 的光标位置读的是 `ConOut->Mode`** —— 切到 fb 之后那是固件的数字，全错。
  给 `TND_CONSOLE` 加了 `GetXY`，以后只问自己的后端
- **`fb_init` 没清屏** —— 固件留下的像素透出来，屏幕是花的

---

## 实测

```
[log] con_fb: grid=160x50 cell=8x16 scale=100%
$ console fb      切到自有帧缓冲，160x50
$ console uefi    切回固件 ConOut
```

---

## 已知限制

- 一次一个程序，ring 0，没有内存隔离
- 没有 `ExitBootServices`
- 只有 ASCII，中文还显示不了（下一版）
- 只在 QEMU + OVMF 上验证过

---

## 下一版

中文上屏：字体生成器、TNDF 字体格式、内核加载器、批量原语。

<details>
<summary>English</summary>

TNDDOS 0.3.2-M4-SP0 -- the console is taken over from the firmware. The first step of M4.

**The console now has the notion of a backend.** The kernel is no longer hard-wired to UEFI's
ConOut; `TND_CONSOLE` is a set of swappable backends -- the original ConOut, and one that writes
its own pixels into the framebuffer with its own bitmap font. Switch with `CONSOLE fb` /
`CONSOLE uefi`.

This matters because ConOut is a Boot Service: it and its font disappear the moment
`ExitBootServices` runs, while the framebuffer's memory is still there. Taking over output first
is what makes leaving UEFI conceivable at all.

Also included: a shadow buffer holding a character and an attribute per cell, so scrolling,
clearing and positioning happen on our side; and an embedded 8x12 CP437 font extracted from
`vgaoem.fon`.

Fixed along the way: the shell read its cursor position from `ConOut->Mode`, which is the
firmware's number and simply wrong once we switch to the framebuffer -- `TND_CONSOLE` gained a
`GetXY` so we only ever ask our own backend. And `fb_init` did not clear the screen, so leftover
firmware pixels showed through.

Still UEFI-only, single-tasking, ring 0, and only ever verified on QEMU + OVMF.

</details>
