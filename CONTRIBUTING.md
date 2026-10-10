# 约定 / Conventions

## 1. 提交信息必须双语 / Commit messages must be bilingual

**主题行 = 中文 + English，一行。**

```
提交初始代码 Submit the initial code
```

正文可以以中文为主，但**主题行必须有英文**。仓库是公开的，
看不懂中文的人至少要知道这次提交干了什么。

反例（只有中文，公开仓库里等于没写）：

```
TNX 加载器 + 裸程序名执行；工具链拆到独立仓库          <- 不合格
TNX loader + run programs by bare name; split toolchain out   <- 合格
```

完整例子：

```
TNX 加载器 + 裸程序名执行；工具链拆到独立仓库

TNX loader + run programs by bare name; split toolchain out

- 新增 TNX 可执行格式 v1 与加载器
- 敲程序名（扩展名可选）直接执行，TNX <file> 改为查看信息

- add TNX executable format v1 and its loader
- running a program is now just typing its name; TNX <file> inspects
```

---

## 2. 控制台输出一律 ASCII / Console output is ASCII only

UEFI 固件的点阵字体**不保证**带 ASCII 以外的字形。OVMF 实测结果：
中文 codepoint 找不到字形，屏幕上是一片**纯空白**。读不出字的控制台比没有更糟。

所以 `con_*` / `log_*` / `dputs` 的字符串**全部英文**。

源码**注释**保持中文 —— 那是给人读源码的，不经过任何字体。
文档（README / docs/）保持中文为主，按第 1 条的要求配英文主题。

---

## 3. 不写死任何机器相关的路径 / No hardcoded tool paths

所有外部工具从环境变量取，取不到就自动探测，再取不到就**报错并把 `setx` 命令打给用户**。

| 变量 | 用途 |
|---|---|
| `TNDDOS_LLVM_BIN` | 含 `clang.exe` 与 `ld.lld.exe` 的目录 |
| `TNDDOS_QEMU` | `qemu-system-x86_64.exe` 完整路径 |
| `TNDDOS_TOOLKIT` | TNDOS-ToolsKit 仓库根目录 |

---

## 4. PowerShell 脚本必须有 UTF-8 BOM

Windows PowerShell 5.1 没有 BOM 就按 ANSI 读文件，
**中文注释会让整个脚本语法错误** —— 而且报的错和真正的原因毫无关系。

编辑工具会吃掉 BOM，所以每次改完 `.ps1` 都要检查一遍。

---

## 5. 行尾统一 LF

见 `.gitattributes`。`*.ps1` / `*.bat` / `*.cmd` 例外，保持 CRLF。

---

## 6. 失败要响，不许静默 / Fail loud, never silently

这条是两次最贵的教训换来的：

- `t_read_file` 缓冲区满了却不报，内核映像被截断 1.5 KB，
  `LoadImage` 只回了一个没头没脑的 `Unsupported`
- `pmm_reserve` 因为 `gReady` 还没置位而**静默地什么都没做** ——
  位图上看不出来，日志里也没有任何迹象

**任何"可能悄悄没生效"的地方都要有断言或日志。**

---

## 7. 编译器看不见的东西必须埋运行期自检

结构体偏移、ABI、格式布局，编译器一声不吭。所以：

- `LoadedImage.SystemTable == SystemTable` —— 校验 `EFI_LOADED_IMAGE_PROTOCOL` 的偏移
- `FirmwareVendor == "EDK II"` —— 校验 `EFI_SYSTEM_TABLE` 的偏移
- TNX 加载器的 `validate()` —— 每条规则失败都有**具体**理由，绝不部分加载

---

## 8. 每个改动都要能真的跑一遍

不是"看起来对"，是**在 QEMU 里启动过**。
`tools/build-run.ps1` 一键完成：编译 -> 组 ESP -> 造 FAT16 映像 -> QEMU/OVMF 启动 -> 打印串口日志。

`tnxdump -Validate` 可以在不启动 QEMU 的情况下校验一个 TNX —— 构建流水线里优先用它。
---

## 9. 版本号跟着发布走 / Version numbers follow releases

**规则：一条 Release 交出去之后，再往主线加功能就要推进版本号。**

理由很具体：源码归档是按 tag 存的。如果 tag `v0.3.2` 指向的源码里写着
`TND_VERSION "0.3.1-M3"`，那下载 0.3.2 源码的人编译出来会得到一个
**自称 0.3.1 的二进制** —— 二进制和源码对不上，出问题无从查起。

所以顺序是：

```
1. 改 TND_VERSION 为目标版本，提交      <- 先改版本号
2. 打 tag 并推送
3. CI 自动建 Release（.github/workflows/release.yml）
4. 人工把构建产物传到那条 Release 上
```

**不要反过来**（先打 tag、之后再补版本号）。那样 tag 指向的源码是错的，
而且 tag 一旦推上去就不该再动 —— 动它等于悄悄换掉别人已经下载过的东西。

---

## 10. 控制台层之外不许碰 `gEnv.ST->ConOut` / Never touch ConOut outside the console layer

**规矩：需要屏幕状态（尺寸、光标位置、当前属性）就问 `con_*`；需要清屏、定位、设置属性也走 `con_*`。**

这条不是洁癖，是**同一个 bug 已经犯了三次**：

| # | 谁 | 症状 |
|---|---|---|
| 1 | shell 的行编辑读 `ConOut->Mode->CursorColumn` | 切到 fb 后光标位置是过期值，重画落错位置，屏幕上出现 `ddidir` |
| 2 | `api_getattr` 读 `ConOut->Mode->Attribute` | 返回过期属性 |
| 3 | `api_cls` / `api_gotoxy` / `api_cols` / `api_rows` 直连 ConOut | **EDIT 拿到 100x31 而不是 160x50，清屏清的是 UEFI 控制台，定位移的是 UEFI 光标** —— 全屏程序整个错位 |

**根因都一样**：ConOut 是**其中一个后端**，不是唯一的后端。fb 后端自己写像素，
从不通知 ConOut，所以 ConOut 里的状态**从切换那一刻起就是死的**。

**判据**：任何写成 `gEnv.ST->ConOut->...` 的地方，先问一句 ——
**"这句话在 fb 后端下还对吗？"** 答不上来就说明它该走 `con_*`。

后端不支持的操作用 `con_*` 也会得到诚实的答案（比如 UEFI 后端的缩放返回"不支持"），
而绕过服务层只会得到**一个看似合理但过期的值** —— 那比报错难查得多。


---

## 11. 英文不是摘要，是逐节全文直译

**发布说明、README、规范文档里的英文，必须是中文的逐节全文直译。**

不是摘要，不是「要点」，是**一节对一节、一条对一条**。

判断标准只有一条：

> **把中文整段删掉，英文单独拿出来读，信息量应该一样。**

以下这些都是摘要，不是翻译：

```
漏掉整节      实测 / 已知限制 / 下一版 —— 这三节最常被漏
把 N 条压成一句  六条已知限制写成一个短句
跳过表格与脚注  约定表、括号里的那句提醒
省略标题句     开头那句加粗的总结
```

**已经因为这个被纠正过两次。** 第一次是 SP1，第二次是 SP2 —— 所以它现在是规矩，
不再依赖谁记得。

理由：英文版是给不看中文的人看的。它少一节，那个人就少知道一件事 ——
**而他不会知道少了什么。**
