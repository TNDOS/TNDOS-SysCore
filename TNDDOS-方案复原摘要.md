# TNDDOS / 2NDDOS 方案 — 摘要 v2（含用户修正）

> v1 = 用户从云盘/历史复原的方案。
> **v2 = v1 + 用户 2026 本轮的口头修正。修正项以 [v2修正] 标出，原 v1 条目保留可追溯。**
> 磁盘现存物标 [磁盘]；冲突标 [冲突]；待定标 [待定]。
>
> **实现状态（2026）**：M0 / M2 / M3 已完成并在 QEMU/OVMF 下实测通过，
> 见 `README.md`。本文只作设计记录，不再随代码更新。

---

## 一、一句话概括

> **不是把传统 DOS 搬到 UEFI，而是重新做一个 UEFI 时代的 DOS。**

保留 DOS 的**交互模型**（简单、直接、命令行优先、`C:\` 盘符），
抛弃 DOS 的**内存模型与历史包袱**（16 位、BIOS、640KB、DOS ABI）。

---

## 二、取舍（10 条，含 v2 修正）

| # | 决定 | 同时排除掉 |
|---|------|-----------|
| 1 | x86-64 + UEFI-only；**ia32 等平台保留** [v2修正] | CSM、Legacy BIOS、16 位实模式 |
| 2 | **`\EFI\BOOT\BOOTX64.EFI` 不是系统入口，而是"初始化器"，地位≈传统 DOS 的 PBR** [v2修正] | 不写复杂 Boot Manager |
| 3 | 产物是 PE32+ EFI Application；Clang + LLD；不依赖 CRT | 不自写 ELF bootloader、不背 C runtime |
| 4 | 启动即 64 位 Long Mode | 没有 A20 Gate、没有 real→protected→long 三段跳 |
| 5 | **UEFI 是启动平台，但现阶段不提前切断与 EFI 的联系** [v2修正]：文件系统、磁盘控制器等驱动仍然借 UEFI 加载，因为驱动模型尚未搭建 | 不把 UEFI 当 OS 使；但也不现在甩开它 |
| 6 | ~~`ExitBootServices()` 是分水岭~~ → **降级为"以后再说"的开关** [v2修正] | — |
| 7 | TNDOS API / 平台抽象层 | 不让 Shell 直接绑死在 UEFI API 上 |
| 8 | 自己做 PMM → VMM → Kernel Heap | 不做 640KB / EMS / XMS / HMA |
| 9 | **可执行格式先用 PE 过渡** [v2修正]，TNX 暂缓 | 不兼容 `.COM`/`.EXE`；不背 DOS ABI |
| 10 | 只保留 DOS 的交互模型 | 其余历史包袱全扔 |

---

## 三、启动链（v2 修正后）

v1 的说法：

```
UEFI Firmware → ESP → \EFI\BOOT\BOOTX64.EFI → TNDOS Loader → Kernel → Shell
```

**v2 修正：BOOTX64.EFI 的角色 ≈ DOS 的 PBR（分区引导记录），是"初始化器"，不是系统本体。**

### 与 MS-DOS 的对应关系（v2 新增）

| TNDDOS | MS-DOS 对应物 | 角色 |
|--------|---------------|------|
| UEFI Firmware | BIOS | 固件 |
| `\EFI\BOOT\BOOTX64.EFI` | **PBR**（分区引导记录） | 初始化器，把系统拉起来 |
| `kernel.efi` | **IO.SYS** | 内核本体 |
| `efidos.sys` | **MSDOS.SYS（DOS 7）** | **系统配置**，安装时部署，**先加载** |
| `config.sys` | CONFIG.SYS | **用户配置**，保持原样 |
| `autoexec.bat` | AUTOEXEC.BAT | **保持原样** |
| Shell | COMMAND.COM | 命令解释器 |

### 加载顺序

```
UEFI Firmware
   → \EFI\BOOT\BOOTX64.EFI      （初始化器 ≈ PBR）
   → kernel.efi                  （内核 ≈ IO.SYS）
   → efidos.sys                  （系统配置，先加载）
   → config.sys                  （用户配置）
   → autoexec.bat                （用户启动脚本）
   → Shell                       （≈ COMMAND.COM）
```

### 配置语义（v2 明确）

- `efidos.sys`：**文本，语法与 config.sys 相同**，但它是**安装时就部署好的系统配置**，由系统提供；**先于**用户 config.sys 加载。
- `config.sys`：**完全交给用户**，保持传统语义与语法。
- `autoexec.bat`：保持原样。

---

## 四、路线图（v2 修正后）

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| **M0** | BOOTX64.EFI 能起来，UEFI Console 输出 | 复原文档说 v0.1 已做 |
| **M1** | UEFI Input 接键盘 → Shell（`help/ver/cls/echo/mem/reboot/shutdown`）+ `GetMemoryMap` + `ResetSystem` | **v0.1 在这** |
| **M2** | BOOTX64.EFI 加载 `kernel.efi`；efidos.sys / config.sys / autoexec.bat 解析；借 UEFI Simple File System 读 FAT32 | 规划 |
| **M3** | Kernel 模块化 + 内存管理器（从 GetMemoryMap 建 PMM） | 规划 |
| **M4** | 驱动模型 + VFS + 调度 + Syscall + 用户程序 | 远期 |
| **—** | ~~ExitBootServices~~ **不再作为里程碑**，等驱动模型就绪后再决定何时切换 [v2修正] | 挂起 |

---

## 五、与磁盘现存物的对账（v2 更新）

| 项 | [用户] v2 方案 | [磁盘] `TNDDOS_SPEC.md` / 代码 | 结论 |
|---|---|---|---|
| 平台范围 | ia32 等**保留** | 也要 ia32 / ARM / aarch64 | **一致** |
| `BOOTX64.EFI` 角色 | 初始化器 ≈ PBR | 磁盘文档写成"引导器 / 最终目标原生 EFI 启动器" | **按 v2 更正** |
| 程序格式 | **先用 PE 过渡** | TNX 自定义容器 + 打包器 | **TNX 暂缓**，按 v2 |
| 系统配置 | `efidos.sys`（≈MSDOS.SYS，先加载） | `TNDDOS.sys`（≈config.sys） | **改名 + 补角色**：TNDDOS.sys → efidos.sys |
| 用户配置 | `config.sys` / `autoexec.bat` 原样 | 未提 autoexec.bat | **按 v2 补上** |
| 内核 | `kernel.efi` ≈ IO.SYS | `kernel.sys` | **按 v2** |
| GRUB 引导 | 不用 | 早期可用 GRUB 生成 UEFI ISO | **按 v2，去掉** |
| 内核形态 | BOOTX64.EFI → 加载 kernel.efi | bootx64 初始化完直接加载 kernel | **一致** |
| boot.ini | 未提 | bootx64 据同目录 boot.ini 定位系统 | [待定] |

### 5.2 `E:\UEFIDOS` 代码实际状态（未变）

| 文件 | 实际内容 | 判定 |
|---|---|---|
| `src/uefi/boot.c` | 打印两行 → 尝试读 `\TNDDOS\TNDDOS.sys` → `for(;;)` 挂起 | 占位，无键盘、无 Shell |
| `src/uefi/efi.h` | EFI 结构体 | **偏移错误**：`EFI_SYSTEM_TABLE` 的 `_hdr` 只留 4 字节（真实 24），漏 `StdErrHandle` → `ConOut` 落在 48 而非 64；`EFI_BOOT_SERVICES` 的 `LocateHandleBuffer/LocateProtocol` 偏移也错。**能编译，跑不起来** |
| `src/kernel.c` | 往 `0xB8000` 写 `"Hello from MyOS!"` | 旧 BIOS 文本显存，与 UEFI-only 矛盾 |
| `Makefile` / `boot/grub.cfg` | `i686-elf-gcc` + `nasm` + `grub-mkrescue` + `multiboot` | 旧 BIOS 路线残留，应删 |
| `build/iso/EFI/BOOT/BOOTX64.EFI` | 3584~4608 字节，2026-06-24 | 能编译，预期跑不起来 |

> **结论：复原文档说的 v0.1（Shell / Memory Map / ResetSystem）在磁盘这份代码里找不到。** [待定]

---

## 六、待确认清单

1. `kernel.efi` 放在哪？（`\EFI\BOOT\kernel.efi` / `\TNDDOS\kernel.efi` / ESP 根）
2. `boot.ini` 还要不要？（磁盘版曾用它定位系统）
3. `BOOTX64.EFI` 交棒后自己退出，还是常驻？（DOS 里 PBR 是瞬态的）
4. ia32 保留 = **现在就出 `BOOTIA32.EFI` + 32 位 kernel**，还是先把架构相关代码隔离好、只跑 x64？
5. Shell 叫什么、住哪？（COMMAND.COM 的位置）
6. `efidos.sys` 是最终命名吗？（前身 `TNDDOS.sys`）
7. 云盘上那份 v0.1 代码还在吗？

---

## 七、工程判断（v2）

1. **"不提前切断 UEFI" 这个决定是对的**：在驱动模型建好之前，保留 Boot Services 就等于白嫖 console / 键盘 / 文件系统 / 磁盘 IO —— 这正是搭 kernel 最需要的东西。
2. **但有一个硬约束绕不开**：Boot Services 还活着时，**固件拥有 IDT**。此时如果把 IDT 换成自己的，会打断固件自己的定时器 / USB / 磁盘中断，`LoadImage`、文件读写可能直接崩。所以：
   - GDT 可以自己装（相对安全）；
   - **IDT / 异常处理会被硬件约束自然地推到 `ExitBootServices` 附近** —— 不是想早退，是没法在固件底下抢中断。
   - 可接受的做法：把 IDT 安装 + 异常接管做成 **`ExitBootServices` 那一刻的一次性切换**，而不是提前散着装。
3. **仍然必须尽早建立"能跑的验证回路"**：结构体偏移这类错误编译器一声不吭（`efi.h` 就是活例）。回路 = Clang + QEMU + OVMF `edk2-x86_64-code.fd` + 串口日志。**这台机器上三样都已具备**（`E:\LLVM\bin`、`C:\qemu`），今天就能建。
4. **PE 过渡是对的选择**：Clang 直出 PE32+，配上 `-Wl,/subsystem:efi_application` 就能被 UEFI `LoadImage` 直接加载，省掉一个自研 loader 的全部工作量。
