# ============================================================================
# TNDDOS -- cross-repo consistency check
#
# The kernel repo, the SDK, the ToolsKit and the SysAPP repo have contracts
# between them. Nothing enforces those contracts today -- we have hand-synced
# tnd_api.h / tnx.h more than once, and the SDK copy of tnx.h once sat a whole
# generation behind (v1.0) with no warning at all.
#
# This script is that warning. Run it before a commit that touches a shared
# header or an API version.
#
# ASCII only -- see docs/REPOS.md ("scripts must stay ASCII").
# ============================================================================
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$fail = 0

function Ok($m)   { Write-Host ("  OK    " + $m) }
function Bad($m)  { Write-Host ("  FAIL  " + $m); $script:fail++ }
function Note($m) { Write-Host ("        " + $m) }

# ---------------------------------------------------------------- locate
function Find-Repo($envName, $marker, $guess) {
    $p = [Environment]::GetEnvironmentVariable($envName)
    if ($p -and (Test-Path (Join-Path $p $marker))) { return $p }
    foreach ($g in $guess) { if (Test-Path (Join-Path $g $marker)) { return $g } }
    return $null
}
$SDK  = Find-Repo "TNDDOS_SDK"     "include\tnd_api.h" @((Join-Path $Root "repos\TNDOS-SDK"), (Join-Path (Split-Path $Root) "TNDOS-SDK"))
$TK   = Find-Repo "TNDDOS_TOOLKIT" "tnxpack.ps1"       @((Join-Path $Root "repos\TNDOS-ToolsKit"), (Join-Path (Split-Path $Root) "TNDOS-ToolsKit"))
$APP  = Find-Repo "TNDDOS_SYSAPP"  "src"              @((Join-Path $Root "repos\TNDOS-SysAPP"), (Join-Path (Split-Path $Root) "TNDOS-SysAPP"))

Write-Host ""
Write-Host "=== repos ==="
Write-Host ("  core      " + $Root)
Write-Host ("  SDK       " + $(if ($SDK) { $SDK } else { "** NOT FOUND **" }))
Write-Host ("  ToolsKit  " + $(if ($TK)  { $TK }  else { "** NOT FOUND **" }))
Write-Host ("  SysAPP    " + $(if ($APP) { $APP } else { "** NOT FOUND **" }))
Write-Host ""

# ------------------------------------------------- 1 shared headers must match
Write-Host "=== shared headers (core owns them, the SDK mirrors them) ==="
if (-not $SDK) { Bad "SDK not found -- cannot check shared headers" }
else {
    foreach ($h in "tnd_api.h", "tnx.h") {
        $a = Join-Path $Root ("src\include\" + $h)
        $b = Join-Path $SDK  ("include\" + $h)
        if (-not (Test-Path $a)) { Bad ($h + ": missing in the core"); continue }
        if (-not (Test-Path $b)) { Bad ($h + ": missing in the SDK"); continue }
        $ha = (Get-FileHash -Algorithm SHA256 $a).Hash
        $hb = (Get-FileHash -Algorithm SHA256 $b).Hash
        if ($ha -eq $hb) { Ok ($h + " identical  (" + $ha.Substring(0,8) + ")") }
        else { Bad ($h + " DIFFERS between core and SDK"); Note ("core " + $ha.Substring(0,8) + "  sdk " + $hb.Substring(0,8)); Note "run toolssync-sdk.ps1 to copy the core version over" }
    }
}

# ------------------------------------------------------ 2 API version sanity
Write-Host ""
Write-Host "=== versions ==="
$api = (Select-String -Path (Join-Path $Root "src\include\tnd_api.h") -Pattern "#define TND_API_VERSION\s+(0x[0-9a-fA-F]+)" | Select-Object -First 1)
if ($api) { Ok ("kernel TND_API_VERSION = " + $api.Matches[0].Groups[1].Value) } else { Bad "TND_API_VERSION not found in the core" }
$tnxv = (Select-String -Path (Join-Path $Root "src\include\tnx.h") -Pattern "#define TNX_VERSION\s+TNX_VERSION_([0-9]+)" | Select-Object -First 1)
if ($tnxv) { Ok ("TNX format version       = " + $tnxv.Matches[0].Groups[1].Value) } else { Bad "TNX_VERSION not found" }
$hsz = (Select-String -Path (Join-Path $Root "src\include\tnx.h") -Pattern "#define TNX_HEADER_SIZE_V11\s+(\d+)" | Select-Object -First 1)
if ($hsz) { Ok ("TNX v1.1 header size     = " + $hsz.Matches[0].Groups[1].Value + " bytes") }

# --------------------------------------------- 3 what the build needs exists
Write-Host ""
Write-Host "=== things the build depends on ==="
if ($SDK) {
    foreach ($p in "lib\tndrt.c", "linker\tnx.ld", "tools\build-tnx.ps1") {
        $f = Join-Path $SDK $p
        if (Test-Path $f) { Ok ("SDK   " + $p) } else { Bad ("SDK   " + $p + "  ** MISSING **") }
    }
    $drv = Join-Path $SDK "drivers"
    if (Test-Path $drv) {
        $n = (Get-ChildItem $drv -Directory).Count
        Ok ("SDK   drivers\  (" + $n + " driver folders)")
    } else { Bad "SDK   drivers\  ** MISSING **" }
}
if ($TK) {
    foreach ($p in "tnxpack.ps1", "mkfat.ps1") {
        if (Test-Path (Join-Path $TK $p)) { Ok ("Tools " + $p) } else { Bad ("Tools " + $p + "  ** MISSING **") }
    }
}

# ------------------------------------------------------------ 4 git states
Write-Host ""
Write-Host "=== git (local-only workflow: nothing should be pushed by a build) ==="
$git = "C:\Program Files\Git\cmd\git.exe"
if (-not (Test-Path $git)) { Note "git.exe not found -- skipped" }
else {
    foreach ($r in @(@{ N = "core"; P = $Root }, @{ N = "SDK"; P = $SDK }, @{ N = "ToolsKit"; P = $TK }, @{ N = "SysAPP"; P = $APP })) {
        if (-not $r.P -or -not (Test-Path (Join-Path $r.P ".git"))) { Note ($r.N + ": not a git repo -- skipped"); continue }
        Push-Location $r.P
        $dirty = (& $git status --porcelain) -join ""
        $ahead = & $git rev-list --count origin/main..HEAD 2>$null
        if ($dirty) { Write-Host ("  DIRTY " + $r.N + "  (uncommitted changes)") }
        else { Ok ($r.N.PadRight(9) + " clean, ahead " + $ahead) }
        Pop-Location
    }
}

Write-Host ""
if ($fail -gt 0) { Write-Host ("  " + $fail + " check(s) FAILED"); exit 1 }
Write-Host "  all cross-repo checks passed"
exit 0
