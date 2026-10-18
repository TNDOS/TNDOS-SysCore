# FatFs 接入进度

R0.16，源码在 `repos/TNDOS-SDK/third_party/fatfs/source/`。

## 已经做好的

```
✓ 解压 + 保留原始目录结构（documents/ 可以删，source/ 只用四个文件）
✓ ffconf.h 从原始文件改（**不是手写** —— R0.16 的 FFCONF_DEF 是 80386，
  手写会撞 "Wrong configuration file"，而且那个报错完全看不出是版本号问题）
✓ ff.c           编译通过，ff.obj 8878 字节
✓ fatfs_glue.c   编译通过，fatglue.obj 1484 字节
```

## ffconf 改了哪几项（其余保持原样）

```
FF_FS_READONLY   0 -> 1    只读挂载（disk_write 返回 RES_WRPRT）
FF_CODE_PAGE   932 -> 437  日文默认值改 ASCII
FF_FS_RPATH      0 -> 2    要 f_chdir / f_getcwd —— 用户要求 cd 不能被拦
FF_FS_NORTC      0 -> 1    没有 RTC，时间戳给固定值
FF_USE_LFN            0    关掉 —— 打开要 ffunicode.c（2MB Unicode 表）
```

## 胶水层的一个坑（记下来）

`fatfs_glue.c` **故意不 include "tnd.h"**。
那个头会把 `efi.h` 拉进来，而 `efi.h` 里的 `typedef void VOID`
和 Windows SDK 的 `#define VOID void` 撞名，报错是：

```
efi.h:20:28: error: cannot combine with previous 'void' declaration specifier
note: expanded from macro 'VOID'
```

看起来和 FatFs 毫无关系。现在改成自己声明那三个内核函数，不拉任何头。

`memcpy / memset / memcmp` 由胶水层提供同名符号，**ff.c 一个字都不用改** ——
这样以后换 FatFs 版本也不会冲突。

## 还没做（下一步）

```
1  build-run.ps1 把 ff.c / fatfs_glue.c 加进内核编译列表
2  fat.c 的对外接口（fat_mount / fat_report / fat_ready）用 FatFs 重写，
   上层（vfs.c / shell.c）一行都不改
3  mkfat.ps1 支持 FAT32 —— **不然 QEMU 里根本复现不了真机**
   FAT32 要求约 65525 个簇：64MB / 1KB 簇 正好
4  QEMU 验证：FAT32 镜像 + dir 能列目录
5  真机验证
```

**第 3 步是前提。** 现在的 ESP 镜像还是 FAT16，而真机是 FAT32 ——
不先造 FAT32 镜像，FatFs 接进来也验证不了。
