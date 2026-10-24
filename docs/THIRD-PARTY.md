# 第三方代码清单 Third-party components

本文件记录 TNDDOS 中所有非原创代码。 This file records every piece of non-original code in TNDDOS.

**规则 Rules**

```
· 主项目是 MIT，但**第三方组件保留它自己的许可证**
  The project is MIT, but **third-party components keep their own licence**
· 每引入一个组件，必须在本文件登记：来源、版本、许可证、改动、许可证原文位置
  Every component is registered here: origin, version, licence, modifications, where the licence text lives
· **不删原作者版权声明，不把别人的代码标成 MIT**
  Never strip the original copyright notice, never relabel someone else's code as MIT
· 许可证宽松不等于自动通过 —— 还要逐文件看声明、依赖、附加条款
  A permissive licence is not an automatic pass -- check per-file notices, dependencies and extra terms
· GPL / LGPL 不作为默认候选，除非专门做过兼容性评估
  GPL / LGPL are not default candidates without a dedicated compatibility review
```

## 已引入 Components in use

### 已移除 Removed: FatFs R0.16

| 项目 | 内容 |
|---|---|
| 来源 | FatFs（ChaN）R0.16 |
| 曾位于 | repos/TNDOS-SDK/third_party/fatfs/ |
| 移除版本 | 0.3.2-M4-SP2.5 |
| 原因 | 移植层在读取跨多个簇的文件时交出了错误数据，导致 TNX 程序映像被写坏。详见 docs/FATFS.md |
| 现状 | 源码已从仓库删除；构建列表里不再有 ff.c / fatfs_glue.c；发布包里也不会再出现 |

**许可证义务随之结束** —— 我们不再分发它。
自研的 FAT12/16/32 驱动是本项目自己的代码，仍按 MIT 发布。

## 计划引入 Planned

### NVMe —— 优先 OpenBSD 的 nvme.c

```
许可证  ISC 风格宽松（文件头直接列出条件）
理由    纯 C、通用、不绑厂商，而且许可证是我们要的
用法    **搬枚举和命令提交的逻辑**，不搬它的 OS 抽象层
        （bus_dma / mutex / 中断这些要换成我们自己的）
状态    待获取
```

### AHCI —— 优先 OpenBSD 的 ahci.c

```
同上。同许可证系列，同一套移植策略。
状态    待获取
```

### IDE(PATA) —— 优先 OpenBSD 的 wdc.c / pciide.c

```
**这是 QEMU 的刚需，不是实体机的刚需。**
（-machine pc 用的是 PIIX IDE；q35 是 AHCI）
作者的原话：「IDE 也是 VM 刚需，不是实体机刚需」
状态    待获取
```

### EDK2 —— 只做参考，不搬代码

```
许可证  BSD-2-Clause-Patent（宽松）
用法    **看它怎么枚举设备、怎么和硬件通信**，把能搬的**逻辑**搬过来，
        但不是逐行复制 —— 因为它和 UEFI Boot Services 深度耦合
        （AllocatePool / PCI IO 协议 / 事件回调），退出之后它自己就死了
状态    参考
```

## 显示 Display

```
**不做驱动。** No driver.

GOP 协议退出之后失效，但**帧缓冲那块内存还在** ——
我们拿到的 Base / Stride / 分辨率都是普通数字，照像素继续写就行。
唯一改不了的是分辨率、像素格式（那些要在退出前调 SetMode）。

所以显示这一块：退出前用 GOP 记参数，退出后自己往内存写。没有第三方代码。
```

## 登记模板 Template

```
### <组件名> <版本>

来源    <URL / 作者>
版本    <版本号, 日期>
位置    <目录>
许可证  <许可证名>；原文见 <文件>
改动    <逐条列出改了什么；没改就写"未修改">
状态    <已接入 / 编译通过但未接入 / 待获取 / 仅参考>
```
---

## 压力测试用的第三方代码（`repos/TNDOS-SDK/examples/stress/src/`）

这三份都是原样引入，没有改算法，只在外面套了 `compat/` 里的 freestanding 头文件。

| 文件 | 来源 | 许可 | 用途 |
|---|---|---|---|
| `puff.c` / `puff.h` | zlib 官方仓库 `contrib/puff` | zlib licence | 参考 inflate 实现，验证真实 deflate 流 |
| `aes.c` / `aes.h` | `kokke/tiny-AES-c` | Unlicense（公有领域） | AES-128 ECB / CBC / CTR |
| `md5.c` / `md5.h` | RFC 1321 附录参考实现 | 公有领域 | MD5，含 RFC 附录 A.5 的标准向量 |

**获取方式**：GitHub 系域名在本机被墙（TLS 重置），改从 gitee 镜像取：

```
puff.c   gitee.com/mirrors/zlib/raw/master/contrib/puff/puff.c
aes.c    gitee.com/mirrors/tiny-AES-c/raw/master/aes.c
md5.c    按 RFC 1321 附录实现（公有领域，未从网络取）
```

### 为了 freestanding 做的适配

`compat/string.h`、`compat/setjmp.h` 是自写的声明头（freestanding target 没有它们）；
`shim.c` 提供 `memcpy/memset/memmove/memcmp/strlen/strcmp/strncmp/strcpy/strncpy`
以及用汇编实现的 `setjmp/longjmp`（puff.c 用它们做错误处理）。

**本项目的 MIT 许可不覆盖上面这三份第三方代码**，它们各自保留原许可。