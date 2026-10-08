# 驱动模型（DEVICE=）  > 本文件从 README.md 拆出。最后更新随源码走。 
## 驱动（DEVICE=）

语义按方案：**DEVICE=<文件> 约等于 UEFI Shell 的 load fs0:\<文件>**。
内核把该文件当 UEFI 映像读进来，LoadImage 之后 StartImage 调它的入口点。

```
── efidos.sys  系统配置（安装期部署，先加载）
    DEVICE    = VGA.EFI
[log] drv: \EFI\TNDOS\DRIVERS\VGA.EFI  4096 bytes
[VGA.EFI] TNDDOS 控制台驱动
          文本模式尺寸: 80 列 x 25 行
          模式号 当前/最大: 2 / 3   属性 0x0000000000000007
          [属性切换测试] 这一行是 0x0F 白字黑底
[VGA.EFI] 初始化完成 -> EFI_SUCCESS
          已加载 4096 字节，入口返回 EFI_SUCCESS

── config.sys  用户配置
    DEVICE    = KBD.EFI
[log] drv: \EFI\TNDOS\DRIVERS\KBD.EFI  3584 bytes
[KBD.EFI] TNDDOS 键盘驱动
          ConsoleInHandle: 0x000000000E7A7818
          WaitForKey 事件: 有效
          清空输入缓冲区: 完成
[KBD.EFI] 初始化完成 -> EFI_SUCCESS

  驱动 (2 个：成功 2，失败 0)
    OK        VGA.EFI   4096 字节
    OK        KBD.EFI   3584 字节
```

细节约定：

- 路径走 **VFS**，所以 DOS 路径语义（`\` / `.` / `..` / 驱动器字母）都能用。
- 只写文件名时（`DEVICE=VGA.EFI`），先去 `\EFI\TNDOS\DRIVERS\` 找，找不到再当相对路径算。
- 驱动加载**失败绝不中断启动**，只记一笔，`DRIVERS` 命令里能看到失败项。
- 用内存缓冲 `LoadImage` 时固件不会把设备句柄传下去，必须显式补上（M0 踩过一次）。

写一个驱动只要实现 `efi_main(ImageHandle, SystemTable)`，链接 `drvlib.c` + `utf8.c`，
再用 `-Wl,/subsystem:efi_application,/entry:efi_main` 编译即可 —— 见 `src/drv/vga.c`。

```
C:\EFI\TNDOS>drivers

  驱动 (2 个：成功 2，失败 0)
    OK        VGA.EFI   4096 字节
    OK        KBD.EFI   3584 字节

C:\EFI\TNDOS>load vga.efi
[VGA.EFI] TNDDOS 控制台驱动
  ...
  OK
```
