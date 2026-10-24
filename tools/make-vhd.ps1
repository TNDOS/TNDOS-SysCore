# ============================================================================
# make-vhd.ps1 -- build the release artifact as a VHD
#
# From 0.3.2-M4-SP2.5 on, Releases ship a .vhd instead of a raw .img.
# Same contents (GPT + ESP FAT32 + loader + kernel); a VHD is a container
# Windows can mount by double-clicking, while a bare .img needs extra tools.
#
# Usage:   tools\make-vhd.ps1 [-SizeMB 64] [-NoBuild]
# Output:  dist\TNDDOS-<version>-x86_64.vhd
#
# NOTE: this file must stay pure ASCII. A .ps1 that loses its UTF-8 BOM and
# contains non-ASCII text fails to parse, silently, with no useful message.
# ============================================================================
param(
    [int]$SizeMB = 64,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Build = Join-Path $Root 'build'
$Dist  = Join-Path $Root 'dist'
$EspDir = Join-Path $Build 'esp'

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

if (-not (Test-Path $EspDir)) { throw "ESP staging dir missing: $EspDir" }
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$out = Join-Path $Dist ("TNDDOS-$ver-x86_64.vhd")
Remove-Item $out -Force -ErrorAction SilentlyContinue

# --- create an empty fixed VHD, partition it, format FAT32, assign T: ---
$dp1 = Join-Path $Build 'mkhd-create.txt'
@(
    "create vdisk file=`"$out`" maximum=$SizeMB type=fixed"
    "select vdisk file=`"$out`""
    'attach vdisk'
    'create partition primary'
    'format fs=fat32 quick label=TNDDOS'
    'assign letter=T'
    'exit'
) -join "`r`n" | Set-Content -Path $dp1 -Encoding ASCII
Write-Host '--- diskpart: create + format ---'
cmd /c "diskpart /s $dp1" | Out-Null
Start-Sleep -Seconds 2

# --- copy the staged ESP into it ---
if (-not (Test-Path 'T:\')) { throw 'drive T: did not appear -- diskpart failed' }
Remove-Item 'T:\EFI' -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $EspDir 'EFI') 'T:\' -Recurse -Force
Write-Host ('files on T: ' + (Get-ChildItem 'T:\' -Recurse -File | Measure-Object).Count)

# --- detach ---
$dp2 = Join-Path $Build 'mkhd-detach.txt'
@(
    "select vdisk file=`"$out`""
    'detach vdisk'
    'exit'
) -join "`r`n" | Set-Content -Path $dp2 -Encoding ASCII
cmd /c "diskpart /s $dp2" | Out-Null
Start-Sleep -Seconds 2

Remove-Item $dp1, $dp2 -Force -ErrorAction SilentlyContinue

if (-not (Test-Path $out)) { throw 'the VHD was not produced' }
$len = (Get-Item $out).Length
Write-Host ""
Write-Host "release artifact: $out  ($len bytes)"
Write-Host "sha256:"
(Get-FileHash $out -Algorithm SHA256).Hash | ForEach-Object { Write-Host "  $_" }
