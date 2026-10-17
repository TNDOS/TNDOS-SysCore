# 仓库联动契约

TNDDOS 由四个仓库组成。它们各有各的生命周期，但之间有**必须成立的约定**。
这份文档写清楚：**谁拥有什么、什么必须一致、怎么验、怎么修。**

---

## 1 四个仓库各管什么

| 仓库 | 管什么 | 谁会用 |
|---|---|---|
| **TNDOS**（本仓库） | 引导器、内核、shell、内置命令、构建入口 | 终点用户 |
| **TNDOS-SDK** | 驱动 SDK、TNX 运行时 `tndrt`、链接脚本、示例、驱动源码 | 写驱动/程序的人 |
| **TNDOS-ToolsKit** | `tnxpack.ps1`（ELF64→TNX）、`mkfat.ps1`（目录→FAT16） | 构建过程 |
| **TNDOS-SysAPP** | 外部命令（TREE / MORE / FC / FIND / ATTRIB / EDIT） | 终端用户 |

**为什么拆成四个**：它们的节奏不一样。驱动 SDK 的版本号跟着 API 走，
工具链跟着格式走，外部命令跟应用户需求走，而内核要能单独发版。
塞进一个仓库的结果是「改一个外部命令也要连着内核一起发版」。

---

## 2 共享头文件：**内核是单一真相源**

```
src\include\tnd_api.h     <- 内核实现这套 API，所以它拥有定义
src\include\tnx.h         <- TNX 格式由加载器实现，所以它拥有定义

TNDOS-SDK\include\*.h     <- **镜像，由 tools\sync-sdk.ps1 生成**
```

**SDK 里那两份不要手改。** 改镜像的后果不是报错，是**静默分叉** ——
内核对着一套结构体、驱动对着另一套，字段偏移错了不会有人告诉你。

这个坑踩过：SDK 的 `tnx.h` 曾经停在 **v1.0**（48 字节头），而内核已经是
**v1.1**（56 字节头），中间隔了一整代，没有任何提示。

**`efi.h` 是例外，故意不一致**：SDK 那份是**严格子集**（驱动用不到
GOP、配置文件系统、文件协议）。内核那份多出来的部分不要往 SDK 同步。

---

## 3 版本号

```
TND_API_VERSION     在 tnd_api.h 里，**两边各写一份，必须相同**
TNX_VERSION         在 tnx.h 里，由镜像保证一致
TND_VERSION         字符串，只在 core 里（"0.3.2-M4-SP2" 那种）
```

`TND_API_VERSION` 是**表尾追加**的版本：驱动靠它判断 API 表里有没有它要的
那个字段。加字段就加版本，**老驱动仍然能用**（`StructSize` 让它们安全地
不认识新字段）。

---

## 4 构建时的依赖关系

```
core 的 build-run.ps1 需要：
  SDK\lib\tndrt.c            TNX 运行时
  SDK\linker\tnx.ld          链接脚本
  SDK\tools\build-tnx.ps1    **TNX 驱动的构建复用这一份**（不重抄参数）
  Toolkit\tnxpack.ps1         ELF64 -> TNX
  Toolkit\mkfat.ps1           目录 -> FAT16 镜像
  SDK\examples\hello\hello.c  示例程序
  外部命令仓库                 部署用
```

**为什么不把参数抄一份到 core**：抄一份就是两处会漂移的地方。
`build-tnx.ps1` 那套编译/链接/打包参数验证过很多次，直接调它。

仓库位置靠环境变量，找不到再按 `repos\` 和同级目录去猜：

```
TNDDOS_LLVM_BIN   clang / ld.lld 所在目录
TNDDOS_QEMU       qemu-system-x86_64.exe
TNDDOS_TOOLKIT    TNDOS-ToolsKit
TNDDOS_SDK        TNDOS-SDK
TNDDOS_SYSAPP     TNDOS-SysAPP
```

---

## 5 怎么验、怎么修

```
tools\verify-repos.ps1   一次查完所有跨仓库约定，有失败就退出码 1
tools\sync-sdk.ps1       把 core 的共享头文件拷到 SDK（覆盖镜像）
```

**动过共享头文件或者 `TND_API_VERSION` 之后必须跑一次 verify。**
同步之后**两个仓库都要提交** —— 只提交一边，下一个人拿到的就是分叉状态。

---

## 6 一条一直是铁律的约定：`.ps1` 只写 ASCII

**PowerShell 5.1 读没有 BOM 的文件时按本地 ANSI 解析。**
带中文的 `.ps1` 一旦丢了 BOM，报的错是这种东西：

```
表达式或语句中包含意外的标记"}"。
   return ("璇疯缃幆澧冨彉閲?" + $Name + ...)
```

**而 `tools.edit` / `tools.write` 写文件时会抹掉 UTF-8 BOM。**
所以：**改了带中文的 .ps1，改完必须补 BOM**，否则下一次构建直接语法错。

这条踩过不止一次。补 BOM + 语法检查：

```powershell
$p = 'tools\build-run.ps1'
$t = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding $true))
$err = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$null, [ref]$err)
```

**新写的脚本一律 ASCII-only**（`verify-repos.ps1` / `sync-sdk.ps1` 都是），
从源头上不需要 BOM。

---

## 7 提交约定

```
只做本地提交，不推送（除非明确要求）。
动到一个仓库不算完 —— **跨仓库的改动要在每个受影响的仓库里都提交**，
提交信息里写清楚「另一个仓库的哪一条对应这一条」。
```
