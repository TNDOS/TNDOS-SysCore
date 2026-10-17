# ============================================================================
# TNDDOS -- copy the shared headers from the core into the SDK
#
# **The core owns them.** The kernel implements the API, so the kernel repo is
# the source of truth; the SDK ships a mirror so third-party drivers can build
# against it. The mirror is generated, not hand-edited: hand-editing it is how
# the SDK copy of tnx.h once sat a whole generation behind (v1.0).
#
# ASCII only -- see docs/REPOS.md.
# ============================================================================
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

$SDK = $env:TNDDOS_SDK
if (-not $SDK) {
    foreach ($g in @((Join-Path $Root "repos\TNDOS-SDK"), (Join-Path (Split-Path $Root) "TNDOS-SDK"))) {
        if (Test-Path (Join-Path $g "include\tnd_api.h")) { $SDK = $g; break }
    }
}
if (-not $SDK) { Write-Host "  ** SDK not found -- set TNDDOS_SDK **"; exit 1 }

$copy = 0
foreach ($h in "tnd_api.h", "tnx.h") {
    $a = Join-Path $Root ("src\include\" + $h)
    $b = Join-Path $SDK  ("include\" + $h)
    if (-not (Test-Path $a)) { Write-Host ("  ** missing in the core: " + $h); exit 1 }
    $ha = (Get-FileHash -Algorithm SHA256 $a).Hash
    $hb = if (Test-Path $b) { (Get-FileHash -Algorithm SHA256 $b).Hash } else { "" }
    if ($ha -eq $hb) { Write-Host ("  same   " + $h); continue }
    Copy-Item $a $b -Force
    Write-Host ("  copied " + $h + "  ->  " + $b)
    $copy++
}

if ($copy -eq 0) { Write-Host "  nothing to do -- the SDK mirrors are already current" }
else { Write-Host ("  " + $copy + " header(s) updated; commit BOTH repos") }
exit 0
