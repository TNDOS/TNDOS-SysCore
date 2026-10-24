# ============================================================================
# make-vhd.ps1 -- wrap build/esp.img into the release artifact
#
# From 0.3.2-M4-SP2.5 on, Releases ship a .vhd instead of a raw .img.
#
# The build already produces esp.img, which IS a complete bootable disk:
#
#     esp.img  =  MBR + one type-0xEF partition + FAT16 + \EFI tree
#
# qemu-img -O vpc adds the VHD container and changes nothing else, so the
# partition table, the FAT16 filesystem and the volume serial all survive.
# The result is dynamic: only the used blocks are stored (4 MB, not 32 MB).
#
# **Do not use diskpart here.** An earlier version attached the VHD, let
# Windows partition and format it, and copied the files in. That path needs
# admin rights and fails silently without them -- no output, no drive letter,
# files never copied, and the VM then boots a stale image while looking fine.
# It also reformats as FAT32 and zeroes the volume serial.
#
# Usage:   tools\make-vhd.ps1 [-NoBuild]
# Output:  dist\TNDDOS-<version>-x86_64.vhd
#
# NOTE: this file must stay pure ASCII.
# ============================================================================
param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$Root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Build  = Join-Path $Root 'build'
$Dist   = Join-Path $Root 'dist'
$EspImg = Join-Path $Build 'esp.img'

# version string, read from the kernel header so there is one source of truth
$hdr = Get-Content (Join-Path $Root 'src\include\tnd.h') -Raw
if ($hdr -notmatch '#define\s+TND_VERSION\s+"([^"]+)"') { throw 'cannot read TND_VERSION' }
$ver = $Matches[1]
Write-Host "TNDDOS version $ver"

if (-not $NoBuild) {
    Write-Host '--- build ---'
    $env:TNDDOS_LLVM_BIN = 'E:\LLVM\bin'
    $env:TNDDOS_QEMU     = 'C:\qemu\qemu-system-x86_64.exe'
    $env:TNDDOS_TOOLKIT  = 'E:\TNDOS\repos\TNDOS-ToolsKit'
    $env:TNDDOS_SDK      = 'E:\TNDOS\repos\TNDOS-SDK'
    $env:TNDDOS_SYSAPP   = 'E:\TNDOS\repos\TNDOS-SysAPP'
    & (Join-Path $Root 'tools\build-run.ps1') -NoRun *>&1 | Out-Null
}

if (-not (Test-Path $EspImg)) { throw "missing $EspImg -- run the build first" }

$qi = Get-ChildItem 'C:\qemu' -Recurse -Filter 'qemu-img.exe' -ErrorAction SilentlyContinue |
      Select-Object -First 1
if (-not $qi) { throw 'qemu-img.exe not found under C:\qemu' }

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$out = Join-Path $Dist ("TNDDOS-$ver-x86_64.vhd")
Remove-Item $out -Force -ErrorAction SilentlyContinue

Write-Host '--- qemu-img: raw FAT16 image -> VHD ---'
& $qi.FullName convert -f raw -O vpc "$EspImg" "$out"
if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed' }
if (-not (Test-Path $out)) { throw 'the VHD was not produced' }

Write-Host ''
Write-Host ("input   esp.img  " + (Get-Item $EspImg).Length + ' bytes  (MBR + type-0xEF + FAT16)')
Write-Host ("output  vhd      " + (Get-Item $out).Length + ' bytes  (dynamic)')
Write-Host "artifact: $out"
Write-Host 'sha256:'
(Get-FileHash $out -Algorithm SHA256).Hash | ForEach-Object { Write-Host "  $_" }
