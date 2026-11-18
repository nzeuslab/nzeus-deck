# Full-size check of Install/Uninstall-NzeusFiles with the REAL release files (4.5 GB):
# a plain folder stands in for the card's FAT32 (no admin, no card).
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\tests\test-installer-fullsize.ps1 RELEASE.zip
param([Parameter(Mandatory = $true)][string]$Zip)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $here "..\NzeusCard.psm1") -Force
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("nzpub-fullsize-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null
$fail = 0
function Check([string]$name, [bool]$ok, [string]$d = "") { if ($ok) { "PASS $name" } else { "FAIL $name $d"; $script:fail++ } }
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $Zip).Path, (Join-Path $tmp "rel"))
    $rel = (Get-ChildItem (Join-Path $tmp "rel") -Directory | Select-Object -First 1).FullName
    $card = Join-Path $tmp "card"
    New-Item -ItemType Directory "$card\cubegm" -Force | Out-Null
    [IO.File]::WriteAllBytes("$card\cubegm\advapi32.dll", [Text.Encoding]::ASCII.GetBytes("stock kernel"))
    [IO.File]::WriteAllBytes("$card\cubegm\ApplicationFrame.dll", [Text.Encoding]::ASCII.GetBytes("stock avp"))
    [IO.File]::WriteAllBytes("$card\cubegm\Bubbles.scr", [Text.Encoding]::ASCII.GetBytes("stock dtb"))
    $stock = (Get-FileHash "$card\cubegm\advapi32.dll").Hash

    # the zip's own SHA256SUMS must match what the installer would copy
    $sums = @{}; foreach ($l in Get-Content "$rel\SHA256SUMS") { $h, $n = $l -split "\s+", 2; $sums[$n] = $h }
    $t0 = Get-Date
    $v = Install-NzeusFiles -Root $card -Release $rel -FreeBytes 100GB
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $ok = $true
    foreach ($f in @("cubegm/advapi32.dll", "cubegm/ApplicationFrame.dll", "nzeus/nzroot.img", "nzeus/nzdata.img", "nzeus/VERSION")) {
        if ((Get-FileHash -LiteralPath (Join-Path $card $f)).Hash.ToLower() -ne $sums["card/$f"]) { $ok = $false }
    }
    Check "fullsize_install_matches_release_sums" $ok "($secs s)"
    Check "fullsize_nzroot_is_3_5_GiB" ((Get-Item "$card\nzeus\nzroot.img").Length -eq 3758096384)
    # the space rule: a 4.5 GB install on a card with 3 GB free is refused before anything is copied
    $card2 = Join-Path $tmp "card2"; New-Item -ItemType Directory "$card2\cubegm" -Force | Out-Null
    Copy-Item "$card\nzeus\backup\advapi32.dll" "$card2\cubegm\advapi32.dll"; Copy-Item "$card\cubegm\Bubbles.scr" "$card2\cubegm\Bubbles.scr"
    $msg = ""; try { Install-NzeusFiles -Root $card2 -Release $rel -FreeBytes 3GB | Out-Null } catch { $msg = $_.Exception.Message }
    Check "fullsize_space_rule" ($msg -like "Not enough free space: NZEUS needs 4.6 GB, the card has 3.0 GB free.*" -and -not (Test-Path "$card2\nzeus")) $msg
    Uninstall-NzeusFiles -Root $card | Out-Null
    Check "fullsize_uninstall_restores" (((Get-FileHash "$card\cubegm\advapi32.dll").Hash -eq $stock) -and -not (Test-Path "$card\nzeus\nzroot.img"))
    "install took $secs s for $("{0:N1}" -f ((Get-ChildItem $card -Recurse -File | Measure-Object Length -Sum).Sum / 1GB)) GB left on the fake card"
} finally {
    Remove-Item -Recurse -Force $tmp
}
if ($fail) { exit 1 }
"4/4 passed"
