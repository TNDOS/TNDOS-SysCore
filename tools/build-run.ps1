# ============================================================================
# TNDDOS 构建 + 启动验证
#
#   .\tools\build-run.ps1                     编译 -> 组 ESP -> 造 FAT16 盘 -> QEMU/OVMF -> 打印串口日志
#   .\tools\build-run.ps1 -NoRun              只编译，不启动
#   .\tools\build-run.ps1 -ShowEnv            只打印工具链定位结果，退出
#   .\tools\build-run.ps1 -Seconds 60 -Keys 'h,e,l,p,ret'
#   .\tools\build-run.ps1 -Fat vvfat          退回 QEMU 的 vvfat（写操作会崩，仅供对照）
#
# 工具链一律从环境变量取，脚本里不写死任何机器相关的路径。
# 取不到就先自动探测（PATH，以及 QEMU 自带的 share 目录），再取不到就报错，
# 并且直接把「该设哪个变量、怎么设」打印出来。
#
#   TNDDOS_LLVM_BIN    含 clang.exe 的目录
#   TNDDOS_QEMU        qemu-system-x86_64.exe 完整路径
#   TNDDOS_OVMF_CODE   OVMF 代码固件 (*.fd)
#   TNDDOS_OVMF_VARS   OVMF 变量存储模板 (*.fd)
# ============================================================================
param(
    [int]$Seconds = 30,
    [switch]$NoRun,
    [string]$Keys = '',
    [int]$Warmup = 10,
    [ValidateSet('image','vvfat')][string]$Fat = 'image',
    [switch]$ShowEnv
)
$ErrorActionPreference = 'Stop'

# 编译器/linker 的 warning 走 stderr。PowerShell 把原生程序的 stderr 变成 ErrorRecord，
# 在 'Stop' 模式下这会让**整个构建因为一个 warning 而中止** —— 而且报错看起来
# 像是编译失败，跟真正的原因毫无关系。
# 我们只认退出码，所以原生工具一律走这个包装。
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments
    $script:NativeExit = $LASTEXITCODE
    $ErrorActionPreference = $prev
}
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# ------------------------------------------------------------ 定位辅助函数

function Get-EnvPath([string]$Name) {
    $v = [Environment]::GetEnvironmentVariable($Name)
    if (-not $v) { return $null }
    if (-not (Test-Path -LiteralPath $v)) {
        throw ("环境变量 " + $Name + " 指向的路径不存在：`n    " + $v + "`n请修正它，或删掉这个变量改用自动探测。")
    }
    return (Get-Item -LiteralPath $v).FullName
}

function How-To([string]$Name, [string]$Example) {
    return ("请设置环境变量 " + $Name + "，然后重开一个终端：`n" +
            "    setx " + $Name + ' "' + $Example + '"')
}

# ------------------------------------------------------------------ clang
# TNDDOS_LLVM_BIN 给的是目录（含 clang.exe），不是 exe 本身
$LLVM_BIN = Get-EnvPath 'TNDDOS_LLVM_BIN'
if ($LLVM_BIN) {
    if (-not (Test-Path -LiteralPath (Join-Path $LLVM_BIN 'clang.exe'))) {
        throw ("TNDDOS_LLVM_BIN 里没有 clang.exe：`n" + $LLVM_BIN)
    }
} else {
    $c = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($c) {
        $LLVM_BIN = Split-Path -Parent $c.Source
    } else {
        throw ("找不到 clang.exe（TNDDOS_LLVM_BIN 未设置，PATH 上也没有）。`n" +
               (How-To 'TNDDOS_LLVM_BIN' 'D:\LLVM\bin'))
    }
}

# ------------------------------------------------------------------- QEMU
$QEMU = Get-EnvPath 'TNDDOS_QEMU'
if ($QEMU) {
    if ((Split-Path -Leaf $QEMU) -notmatch '^qemu-system-') {
        Write-Host ("  提示：TNDDOS_QEMU 指向的不是 qemu-system-*.exe：" + $QEMU)
    }
} else {
    $q = Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue
    if ($q) {
        $QEMU = $q.Source
    } else {
        throw ("找不到 qemu-system-x86_64.exe（TNDDOS_QEMU 未设置，PATH 上也没有）。`n" +
               (How-To 'TNDDOS_QEMU' 'D:\qemu\qemu-system-x86_64.exe'))
    }
}

# ------------------------------------------------------------------- OVMF
# 取不到就去 QEMU 自己的 share\ 目录里翻 —— 发行版通常把固件放那儿
function Find-Ovmf([string]$EnvName, [string]$FileName) {
    $v = Get-EnvPath $EnvName
    if ($v) { return $v }
    $dir = Split-Path -Parent $QEMU
    $cands = @(
        (Join-Path $dir ('share\' + $FileName)),
        (Join-Path $dir $FileName),
        (Join-Path (Split-Path -Parent $dir) ('share\' + $FileName))
    )
    foreach ($cand in $cands) {
        if (Test-Path -LiteralPath $cand) { return (Get-Item -LiteralPath $cand).FullName }
    }
    throw ("在 QEMU 目录里也没找到 OVMF 固件 " + $FileName + "。`n" +
           (How-To $EnvName ((Join-Path $dir ('share\' + $FileName)))))
}

$OVMF_CODE     = Find-Ovmf 'TNDDOS_OVMF_CODE' 'edk2-x86_64-code.fd'
$OVMF_VARS_SRC = Find-Ovmf 'TNDDOS_OVMF_VARS' 'edk2-i386-vars.fd'

# --------------------------------------------------------- TNDOS-ToolsKit
# tnxpack（ELF64 -> TNX）和 mkfat（目录 -> FAT16 映像）住在 ToolKit 仓库里，
# 不放在本仓库 —— 它们和操作系统的生命周期不同，而且 SDK 也要用同一份。
$TOOLKIT = Get-EnvPath 'TNDDOS_TOOLKIT'
if ($TOOLKIT) {
    foreach ($t in 'tnxpack.ps1','mkfat.ps1') {
        if (-not (Test-Path -LiteralPath (Join-Path $TOOLKIT $t))) {
            throw ("TNDDOS_TOOLKIT 里没有 " + $t + "：" + $TOOLKIT)
        }
    }
} else {
    throw ("找不到 TNDOS-ToolsKit（TNDDOS_TOOLKIT 未设置）。" + [char]10 +
           (How-To 'TNDDOS_TOOLKIT' 'D:\TNDOS-ToolsKit') + [char]10 +
           "它提供 tnxpack.ps1 / mkfat.ps1 —— TNX 打包器和 FAT16 映像生成器。")
}

# --------------------------------------------------------------- 目录结构
$Root = Split-Path -Parent $PSScriptRoot          # 脚本在 <root>\tools\ 下
$Build   = Join-Path $Root 'build'
$Esp     = Join-Path $Build 'esp'
$BootDir = Join-Path $Esp 'EFI\BOOT'
$TndDir  = Join-Path $Esp 'EFI\TNDOS'
$Vars    = Join-Path $Build 'OVMF_VARS.fd'
$Serial  = Join-Path $Build 'serial.log'
$QemuErr = Join-Path $Build 'qemu_err.txt'
$EspImg  = Join-Path $Build 'esp.img'
$Inc     = Join-Path $Root 'src\include'

if ($ShowEnv) {
    Write-Host '=== TNDDOS 工具链定位结果 ==='
    Write-Host ("  clang      " + (Join-Path $LLVM_BIN 'clang.exe'))
    Write-Host ("  qemu       " + $QEMU)
    Write-Host ("  OVMF code  " + $OVMF_CODE)
    Write-Host ("  OVMF vars  " + $OVMF_VARS_SRC)
    Write-Host ("  ToolKit    " + $TOOLKIT)
    Write-Host ("  仓库根     " + $Root)
    Write-Host ("  构建输出   " + $Build)
    exit 0
}

Write-Host '=== TNDDOS 工具链 ==='
Write-Host ("  clang      " + $LLVM_BIN)
Write-Host ("  qemu       " + $QEMU)
Write-Host ("  OVMF       " + (Split-Path -Parent $OVMF_CODE))
Write-Host ("  ToolKit    " + $TOOLKIT)

if (Test-Path $Esp) { Remove-Item $Esp -Recurse -Force }
New-Item -ItemType Directory -Force -Path $BootDir, $TndDir | Out-Null
# 每次都用全新的 VARS，不要"存在就留着"。
#
# OVMF 会把引导顺序存回 NVRAM。一旦它曾经没找到可引导的东西、回落到内置的
# UEFI Shell，**那个选择会被持久化** —— 之后每次启动都起 Shell，
# 而不是我们的 ESP。症状是屏幕上出现 "UEFI Interactive Shell v2.2"，
# 看起来像 ESP 坏了，其实是 NVRAM 记着上次的结果。
# 构建脚本要的是确定性，不是记住上一次。
Copy-Item $OVMF_VARS_SRC $Vars -Force

function Build-Pe([string[]]$Src, [string]$Out) {
    $S = @($Src | Where-Object { Test-Path $_ })
    if ($S.Count -eq 0) { Write-Host ("  [skip] " + (Split-Path $Out -Leaf) + "  (无源文件)"); return }
    Write-Host ("  [ cc ] " + (Split-Path $Out -Leaf) + "   <- " + (($S | ForEach-Object { Split-Path $_ -Leaf }) -join ', '))
    $a = @('-target','x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-fshort-wchar','-nostdlib',
           '-fno-stack-protector','-mno-red-zone','-Wall',
           '-Wl,/subsystem:efi_application,/entry:efi_main','-Wl,/machine:x64',
           '-I', $Inc) + $S + @('-o', $Out)
    Invoke-Native (Join-Path $LLVM_BIN 'clang.exe') $a
    if ($script:NativeExit -ne 0) { throw ("clang 编译失败: " + $Out) }
    Write-Host ("         -> " + (Get-Item $Out).Length + " bytes")
}

$LibSrc = @('src\lib\log.c','src\lib\util.c','src\lib\utf8.c','src\lib\uni.c','src\lib\status.c','src\lib\file.c','src\lib\guid.c',
              'src\lib\con_uefi.c','src\lib\con_fb.c','src\lib\fontvga.c') |
          ForEach-Object { Join-Path $Root $_ }

Write-Host '=== TNDDOS build ==='
Build-Pe (@((Join-Path $Root 'src\boot\bootx64.c')) + $LibSrc) (Join-Path $BootDir 'BOOTX64.EFI')
Build-Pe (@((Join-Path $Root 'src\kernel\kernel.c'),
            (Join-Path $Root 'src\kernel\module.c'),
            (Join-Path $Root 'src\kernel\vfs.c'),
            (Join-Path $Root 'src\kernel\pmm.c'),
            (Join-Path $Root 'src\kernel\heap.c'),
            (Join-Path $Root 'src\kernel\drv.c'),
            (Join-Path $Root 'src\kernel\tnx.c'),
            (Join-Path $Root 'src\kernel\api.c'),
            (Join-Path $Root 'src\kernel\conf.c'),
            (Join-Path $Root 'src\kernel\shell.c')) + $LibSrc) (Join-Path $TndDir 'kernel.efi')

# 驱动：每个都是独立的 UEFI 映像，落到 \EFI\TNDOS\DRIVERS\
$DrvDir = Join-Path $TndDir 'DRIVERS'
New-Item -ItemType Directory -Force -Path $DrvDir | Out-Null
foreach ($d in 'vga','kbd') {
    Build-Pe @((Join-Path $Root ("src\drv\" + $d + ".c")),
               (Join-Path $Root 'src\drv\drvlib.c'),
               (Join-Path $Root 'src\lib\utf8.c')) (Join-Path $DrvDir ($d.ToUpper() + '.EFI'))
}

# ---------------------------------------------------------------------------
# TNX 示例程序：clang -> ELF64 -> ld.lld -> tnxpack -> TNX
# TNX 不需要自己的编译器和链接器，工具链还是 clang / lld，
# tnxpack 只负责最后一步（ELF64 -> TNX）。
#
# **源在 SDK 里，本仓库不留副本。**
# 以前 src\tnx\ 下有一份 tndrt/hello/tnx.ld 的拷贝，构建用的是那份，
# SDK 里那份只是镜像 —— 两边静默分叉之后，改 SDK 不进构建，而且毫无提示。
# 一个会漂移的镜像比一个明确的依赖更糟，所以这里直接依赖 SDK。
# ---------------------------------------------------------------------------
$SDK = Get-EnvPath 'TNDDOS_SDK'
if (-not $SDK) {
    foreach ($guess in @((Join-Path $Root 'repos\TNDOS-SDK'),
                         (Join-Path (Split-Path $Root) 'TNDOS-SDK'))) {
        if (Test-Path (Join-Path $guess 'lib\tndrt.c')) { $SDK = (Get-Item $guess).FullName; break }
    }
}
if (-not $SDK -or -not (Test-Path (Join-Path $SDK 'lib\tndrt.c'))) {
    throw ("找不到 TNDOS-SDK（TNDDOS_SDK 未设置）。" + [char]10 +
           "TNX 的运行时 tndrt.c、示例程序和链接脚本都住在 SDK 里，" + [char]10 +
           "构建需要它：  " + '$' + "env:TNDDOS_SDK = '<TNDOS-SDK 的路径>'")
}
Write-Host ("  SDK        " + $SDK)

$TnxOut = Join-Path $Build 'tnx'
New-Item -ItemType Directory -Force -Path $TnxOut | Out-Null

$TnxUnits = @(
    @{ Name = 'tndrt'; Src = (Join-Path $SDK 'lib\tndrt.c') },
    @{ Name = 'hello'; Src = (Join-Path $SDK 'examples\hello\hello.c') }
)
foreach ($u in $TnxUnits) {
    Write-Host ("  [tnx] " + $u.Name + ".c   -> " + $u.Name + ".o")
    $ta = @('-target','x86_64-unknown-none','-ffreestanding','-fno-builtin','-fno-stack-protector',
            '-mno-red-zone','-nostdlib','-Wall','-I',$Inc,'-I',(Join-Path $SDK 'lib'),
            '-c',$u.Src,'-o',(Join-Path $TnxOut ($u.Name + '.o')))
    Invoke-Native (Join-Path $LLVM_BIN 'clang.exe') $ta
    if ($script:NativeExit -ne 0) { throw ("clang 编译 TNX 程序失败: " + $u.Src) }
}

$lld = Join-Path $LLVM_BIN 'ld.lld.exe'
if (-not (Test-Path $lld)) { throw ("找不到 ld.lld.exe：" + $lld) }
Write-Host '  [tnx] ld.lld      -> hello.elf'
$la = @('-m','elf_x86_64','-T',(Join-Path $SDK 'linker\tnx.ld'),'-o',(Join-Path $TnxOut 'hello.elf'),
        (Join-Path $TnxOut 'tndrt.o'),(Join-Path $TnxOut 'hello.o'))
Invoke-Native $lld $la
if ($script:NativeExit -ne 0) { throw 'ld.lld 链接 TNX 程序失败' }

& (Join-Path $TOOLKIT 'tnxpack.ps1') -In (Join-Path $TnxOut 'hello.elf') -Out (Join-Path $TndDir 'HELLO.TNX')

# ---------------------------------------------------------------------------
# 外部命令（TNDOS-SysAPP）—— 工具是 TNX 程序，已经构建好了，这里只负责部署
# ---------------------------------------------------------------------------
$CMDS = Get-EnvPath 'TNDDOS_SYSAPP'
if (-not $CMDS) {
    foreach ($guess in @((Join-Path $Root 'repos\TNDOS-SysAPP'), (Join-Path (Split-Path -Parent $Root) 'TNDOS-SysAPP'))) {
        if (Test-Path $guess) { $CMDS = $guess; break }
    }
}
if ($CMDS -and (Test-Path (Join-Path $CMDS 'bin'))) {
    $n = 0
    Get-ChildItem (Join-Path $CMDS 'bin') -Filter *.TNX | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $TndDir $_.Name) -Force
        $n++
    }
    Write-Host ("  [cmd] " + $n + " external command(s)   <- " + (Split-Path $CMDS -Leaf))
} else {
    Write-Host "  [cmd] TNDOS-SysAPP 未找到，跳过外部命令（设 TNDDOS_SYSAPP 指定）"
}

# ---------------------------------------------------------------------------
# 两个样本，用来演示 EXECOM。
# 不签入仓库 —— 构建时现造，顺便说明它们的结构。
#   PEDEMO.EXE : MZ 存根 + e_lfanew 指向 PE\0\0  -> PE 映像
#   DOSDEMO.EXE: 纯 MZ，e_lfanew 为 0             -> 16 位 DOS 程序
# ---------------------------------------------------------------------------
& {
    $pe = New-Object byte[] 256
    $pe[0] = 0x4D; $pe[1] = 0x5A                    # "MZ"
    $pe[0x3C] = 0x40                                # e_lfanew = 0x40
    $pe[0x40] = 0x50; $pe[0x41] = 0x45              # "PE\0\0"
    [System.IO.File]::WriteAllBytes((Join-Path $TndDir 'PEDEMO.EXE'), $pe)

    $dos = New-Object byte[] 256
    $dos[0] = 0x4D; $dos[1] = 0x5A                  # 只有 MZ，e_lfanew 保持 0
    [System.IO.File]::WriteAllBytes((Join-Path $TndDir 'DOSDEMO.EXE'), $dos)
    Write-Host "  [cmd] PEDEMO.EXE / DOSDEMO.EXE  (EXECOM samples)"
}

foreach ($f in 'efidos.sys','config.sys','autoexec.bat','HELLO.TXT','TNDOS.TXT') {
    $s = Join-Path $Root (Join-Path 'boot' $f)
    if (Test-Path $s) { Copy-Item $s (Join-Path $TndDir $f) -Force; Write-Host ("  [ cp ] " + $f) }
}
Write-Host ("  ESP  -> " + $Esp)

# 生成真实 FAT16 磁盘映像（QEMU 的 vvfat 在写回时会崩，见 mkfat.ps1 顶部）
if ($Fat -eq 'image') {
    & (Join-Path $TOOLKIT 'mkfat.ps1') -Source $Esp -Out $EspImg
}

if ($NoRun) { Write-Host '=== -NoRun: 跳过启动 ==='; exit 0 }

Remove-Item $Serial, $QemuErr -ErrorAction SilentlyContinue

Write-Host ("=== QEMU / OVMF 启动 (headless, " + $Seconds + "s) ===")
$a = @('-machine','q35','-m','256','-display','none',
       '-serial',("file:" + $Serial),
       '-monitor','tcp:127.0.0.1:5557,server,nowait',
       '-drive',('if=pflash,format=raw,readonly=on,file=' + ($OVMF_CODE -replace '\\','/')),
       '-drive',('if=pflash,format=raw,file=' + ($Vars -replace '\\','/')),
       '-drive',$(if ($Fat -eq 'image') { 'format=raw,file=' + ($EspImg -replace '\\','/') } else { 'format=raw,file=fat:rw:' + ($Esp -replace '\\','/') }),
       '-no-reboot')
$p = Start-Process -FilePath $QEMU -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardError $QemuErr

if ($Keys) {
    Write-Host ("  等待 " + $Warmup + "s 到提示符，然后注入按键: " + $Keys)
    Start-Sleep -Seconds $Warmup
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', 5557)
        $w = New-Object System.IO.StreamWriter($c.GetStream())
        $w.AutoFlush = $true
        foreach ($k in ($Keys -split ',')) {
            $w.WriteLine('sendkey ' + $k.Trim())
            Start-Sleep -Milliseconds 150
        }
        Start-Sleep -Seconds 3
        $w.Close(); $c.Close()
        Write-Host '  按键注入完成'
    } catch { Write-Host ('  按键注入失败: ' + $_.Exception.Message) }
    $rest = $Seconds - $Warmup - 4
    if ($rest -lt 1) { $rest = 1 }
    Start-Sleep -Seconds $rest
} else {
    Start-Sleep -Seconds $Seconds
}

if (-not $p.HasExited) { $p.Kill(); Write-Host '  (到时，已终止 QEMU)' }
Start-Sleep -Milliseconds 500

Write-Host '=== 串口日志 (build\serial.log) ==='
if (Test-Path $Serial) {
    $t = Get-Content $Serial -Raw -Encoding UTF8
    $t = $t -replace "\x1b\[[0-9;]*[A-Za-z]",""
    $t = $t -replace "\x1b\[[0-9;]*=",""
    $t
} else { Write-Host '(没有产生 serial.log)' }

if ((Test-Path $QemuErr) -and (Get-Item $QemuErr).Length -gt 0) {
    Write-Host '=== QEMU stderr ==='
    Get-Content $QemuErr -Raw
}
