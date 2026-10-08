# 开发中真抓到的坑  > 本文件从 README.md 拆出。最后更新随源码走。 
## 开发过程中真抓到的坑（都是"编译过、跑起来才现形"）

| # | 症状 | 真因 |
|---|---|---|
| 1 | `HandleProtocol` 找不到文件系统 | Simple File System 的 GUID 抄成了 `0x0964E5B2`，应为 `0x964E5B22`。看似"设备没文件系统"，其实是 GUID 不对 |
| 2 | `LoadImage` 只回一个 `Unsupported` | 读内核的缓冲 cap 16384 < kernel.efi 17920，文件被自己截断 |
| 3 | 内核 `DeviceHandle == 0` | 用内存缓冲调 `LoadImage` 时 EDK2 **不会**把父映像的设备句柄传下去，必须交棒前显式写入 |
| 4 | 日志里每条输出出现两遍 | OVMF 的 console splitter 本就把 ConOut 送到串口，再镜像一遍就是双份 |
| 5 | PMM 报告 268435456 页 / 位图 32 MB | 把 MMIO 空洞也算进了物理内存。QEMU/OVMF 的 MMIO 落在 TB 级地址，必须按 RAM 类型过滤 |
| 6 | 固件认得出分区却找不到 `\EFI\BOOT\BOOTX64.EFI` | 自造 FAT16 时目录簇的 FAT 表项留成了 0 —— 而 0 的含义是"空闲簇"，于是目录被当成空的 |
| 7 | FAT16 数据区整体偏移一个扇区 | PowerShell 的 `[int]` 是**四舍五入**不是截断，根目录扇区数被算成 33 而非 32 |
| 9* | TNX 程序一跑就 `#UD 无效指令`，RIP 还落在内核里 | **两种调用约定撞车**：TNX 用 `x86_64-unknown-none`（System V，第一个参数在 RDI），内核用 `x86_64-pc-windows-msvc`（MS ABI，第一个参数在 RCX）。程序把指针放 RDI，内核去 RCX 拿垃圾。修法：把 TNX 的 ABI 钉死为 SysV，内核那侧用 `sysv_abi` 适配 |
| 8 | 根目录**第二次 DIR 是空的**（第一次正常） | 卷根句柄是复用的，而 EDK2 的 FAT 驱动枚举到末尾后把读位置停在 EOF，第二次 `Read` 直接返回长度 0。非根目录每次新开句柄所以看不出来。修法：枚举前 `SetPosition(0)` |

