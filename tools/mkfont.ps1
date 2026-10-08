<#
  mkfont.ps1 —— 生成 TNDDOS 控制台点阵字体（TNDF v1）

  用法：
    .\tools\mkfont.ps1                       # 默认字体、默认输出
    .\tools\mkfont.ps1 -Out D:\CJK16.FNT

  ## 为什么用 GDI 而不是自己写光栅化器

  从 TTF 轮廓到点阵要做 hinting / grid-fitting，那是字体引擎的活。
  Windows 的 GDI 在小字号下做得很好（而且会用字体自带的内嵌点阵，如果有）。
  这是**构建期工具**，用 GDI 完全合理；内核侧不需要任何光栅化能力。

  ## 字体与许可

    ASCII   Cascadia Mono     SIL OFL 1.1（微软开源字体）
    CJK     Noto Sans SC      SIL OFL 1.1

  两者都是 OFL，所以**光栅化出来的点阵可以随项目分发**。
  但 OFL 有保留字体名条款：产物不能叫 "Noto Sans SC" / "Cascadia"，
  所以这个格式和产物都叫 TNDF。

  ## 几何（实测定下来的，别随意改）

    ASCII   8x16    Cascadia Mono 12px
    CJK     16x16   Noto Sans SC  16px
    基线    第 14 行（留 1 行给下伸部）

  格子高度必须统一（都是 16）。不统一的话 ASCII 和中文排在同一行会错位。

  ## 踩过的坑（写在这儿，别再犯）

  1. **[int] 是四舍五入不是截断** —— (8+7)/8 得 2 而不是 1，行宽算错，
     整个文件就废了。这正是本项目 PITFALLS 里的那条。改用位运算 -shr。
  2. **矩形要比画布高** —— 基线偏移是负数，矩形高度只给 16 的话字形会被
     矩形裁掉，表现为"画到一半就没了"。
  3. **别对共享位图的子矩形 LockBits** —— 会污染后续绘制状态。
     用两张独立位图（8x16 / 16x16），每次锁整张。
  4. **基线要实测，不要算** —— GetCellAscent 除以 GetEmHeight 算出来的和
     GDI 实际用的不一致（差 2 行）。渲染一个 H，它最下面有像素的那一行
     就是基线。
  5. **PowerShell 里 rd 是 Remove-Item 的别名** —— 函数别叫这个名字。
#>
param(
  [string]$Out       = (Join-Path $PSScriptRoot '..\assets\CJK16.FNT'),
  [string]$AsciiFont = 'Cascadia Mono',
  [double]$AsciiPt   = 12,
  [string]$CjkFont   = 'Noto Sans SC',
  [double]$CjkPt     = 16
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$H = 16; $NW = 8; $WW = 16; $BASE = 14
$pf    = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$flags = [System.Windows.Forms.TextFormatFlags]::NoPadding

function InkBottom([System.Drawing.Font]$f, [int]$w) {
  $b = New-Object System.Drawing.Bitmap($w, 48)
  $g = [System.Drawing.Graphics]::FromImage($b); $g.Clear([System.Drawing.Color]::Black)
  $r = New-Object System.Drawing.Rectangle(0, 0, $w, 48)
  [System.Windows.Forms.TextRenderer]::DrawText($g, 'H', $f, $r, [System.Drawing.Color]::White, $flags)
  $g.Dispose()
  $d = $b.LockBits((New-Object System.Drawing.Rectangle(0, 0, $w, 48)), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $pf)
  $buf = New-Object byte[] ($d.Stride * 48)
  [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length)
  $b.UnlockBits($d); $b.Dispose()
  $last = -1
  for ($y = 0; $y -lt 48; $y++) {
    for ($x = 0; $x -lt $w; $x++) { if ($buf[$y * $d.Stride + $x * 4] -gt 127) { $last = $y; break } }
  }
  return $last
}

$fa = New-Object System.Drawing.Font($AsciiFont, $AsciiPt, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$fw = New-Object System.Drawing.Font($CjkFont,   $CjkPt,   [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$ya = $BASE - (InkBottom $fa $NW)
$yw = $BASE - (InkBottom $fw $WW)
Write-Host ("  [font] $AsciiFont $AsciiPt px -> 8x16    rectY=$ya")
Write-Host ("  [font] $CjkFont $CjkPt px -> 16x16   rectY=$yw")

$bmpN = New-Object System.Drawing.Bitmap($NW, $H)
$bmpW = New-Object System.Drawing.Bitmap($WW, $H)
$gN   = [System.Drawing.Graphics]::FromImage($bmpN)
$gW   = [System.Drawing.Graphics]::FromImage($bmpW)

function Grab($bmp, $g, $font, $cp, $w, $ry) {
  $rb = ($w + 7) -shr 3
  $g.Clear([System.Drawing.Color]::Black)
  $rect = New-Object System.Drawing.Rectangle(0, $ry, $w, ($H + 16))
  [System.Windows.Forms.TextRenderer]::DrawText($g, [string][char]$cp, $font, $rect, [System.Drawing.Color]::White, $flags)
  $d = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $pf)
  $buf = New-Object byte[] ($d.Stride * $H)
  [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length)
  $bmp.UnlockBits($d)
  $gl = New-Object byte[] ($rb * $H); $any = $false
  for ($y = 0; $y -lt $H; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
      if ($buf[$y * $d.Stride + $x * 4] -gt 127) {
        $bi = $y * $rb + ($x -shr 3)
        $gl[$bi] = $gl[$bi] -bor (0x80 -shr ($x -band 7)); $any = $true
      }
    }
  }
  , @($any, $gl)
}

$ranges = @()
$ranges += , @(0x0020, 0x007E, $false)
$ranges += , @(0x3000, 0x303F, $true)
$ranges += , @(0x3040, 0x30FF, $true)
$ranges += , @(0x4E00, 0x9FFF, $true)
$ranges += , @(0xFF01, 0xFF5E, $true)

$cps   = New-Object System.Collections.Generic.List[int]
$bytes = New-Object System.Collections.Generic.List[byte[]]
$t0    = Get-Date
foreach ($r in $ranges) {
  $nar  = -not $r[2]
  $w    = if ($nar) { $NW } else { $WW }
  $font = if ($nar) { $fa } else { $fw }
  $bmp  = if ($nar) { $bmpN } else { $bmpW }
  $g    = if ($nar) { $gN }   else { $gW }
  $ry   = if ($nar) { $ya }   else { $yw }
  $n0 = $cps.Count
  for ($cp = $r[0]; $cp -le $r[1]; $cp++) {
    if ($cp -ge 0xD800 -and $cp -le 0xDFFF) { continue }
    $res = Grab $bmp $g $font $cp $w $ry
    if ($res[0]) { $cps.Add($cp); $bytes.Add($res[1]) }
  }
  Write-Host ("  [font] 0x{0:X4}-0x{1:X4}  +{2}   total {3}" -f $r[0], $r[1], ($cps.Count - $n0), $cps.Count)
}
$gN.Dispose(); $gW.Dispose(); $bmpN.Dispose(); $bmpW.Dispose(); $fa.Dispose(); $fw.Dispose()
Write-Host ("  [font] rendered in {0:N1} s" -f ((Get-Date) - $t0).TotalSeconds)

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([char[]]'TNDF'); $bw.Write([uint16]1); $bw.Write([uint16]$H)
$bw.Write([uint16]$NW);    $bw.Write([uint16]$WW); $bw.Write([uint16]1); $bw.Write([uint16]0)
$n = $cps.Count; $go = 32 + $n * 8
$bw.Write([uint32]32); $bw.Write([uint32]$n); $bw.Write([uint32]$go); $bw.Write([uint32]0)
$off = 0
for ($i = 0; $i -lt $n; $i++) { $bw.Write([uint32]$cps[$i]); $bw.Write([uint32]$off); $off += $bytes[$i].Length }
foreach ($b in $bytes) { $bw.Write($b) }
$bw.Flush()
$dir = Split-Path $Out -Parent
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[System.IO.File]::WriteAllBytes($Out, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()
Write-Host ("  [font] {0}  {1} glyphs, {2} KB" -f $Out, $n, [Math]::Round((Get-Item $Out).Length / 1KB))
