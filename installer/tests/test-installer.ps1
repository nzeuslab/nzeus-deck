# Tests for installer\NzeusCard.psm1. No admin rights, no card: fake card IMAGES
# (only sector 0, the FAT32 boot sector and the top folder are real) and plain folders.
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\tests\test-installer.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $here "..\NzeusCard.psm1") -Force
$script:failures = @()
$script:total = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    $script:total++
    if ($ok) { Write-Output "PASS $name" } else { Write-Output "FAIL $name $detail"; $script:failures += $name }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("nzpub-installer-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null

# ---- fixtures ----
function Set-U16($b, $o, $v) { $b[$o] = $v -band 0xFF; $b[$o + 1] = ($v -shr 8) -band 0xFF }
function Set-U32($b, $o, [long]$v) { for ($i = 0; $i -lt 4; $i++) { $b[$o + $i] = ($v -shr (8 * $i)) -band 0xFF } }
# A fake card image: MBR (one FAT32 partition at 2048), FAT32 boot sector, top folder with CUBEGM.
function New-FakeCardImage([string]$path, [switch]$Open, [switch]$NoCubegm, [switch]$TwoPartitions, [switch]$BootCode) {
    # -BootCode: like the real 16 GB stock card - start-up code in sector 0, FAT32 at sector 32
    $start = if ($BootCode) { 32 } else { 2048 }; $count = 65536; $reserved = 32; $fatSize = 512
    $fs = [IO.File]::Open($path, [IO.FileMode]::Create)
    try {
        $fs.SetLength(([long]$start + $count) * 512)
        $mbr = New-Object byte[] 512
        Set-U32 $mbr 440 0x1234ABCD
        $mbr[446 + 4] = 0x0C; Set-U32 $mbr (446 + 8) $start; Set-U32 $mbr (446 + 12) $count
        if ($TwoPartitions) { $mbr[462 + 4] = 0x83; Set-U32 $mbr (462 + 8) 9000; Set-U32 $mbr (462 + 12) 100 }
        $mbr[510] = 0x55; $mbr[511] = 0xAA
        if ($BootCode) { for ($i = 0; $i -lt 440; $i++) { $mbr[$i] = ($i * 7 + 3) -band 0xFF } }
        if (-not $Open) { $mbr = Switch-R36sScramble $mbr }
        $fs.Position = 0; $fs.Write($mbr, 0, 512)
        $boot = New-Object byte[] 512
        Set-U16 $boot 11 512; $boot[13] = 1; Set-U16 $boot 14 $reserved; $boot[16] = 2
        Set-U32 $boot 0x1C $start; Set-U32 $boot 0x20 $count; Set-U32 $boot 36 $fatSize; Set-U32 $boot 44 2
        $t = [Text.Encoding]::ASCII.GetBytes("FAT32   "); [Array]::Copy($t, 0, $boot, 0x52, 8)
        $boot[510] = 0x55; $boot[511] = 0xAA
        $fs.Position = [long]$start * 512; $fs.Write($boot, 0, 512)
        $root = New-Object byte[] 512
        $name = if ($NoCubegm) { "PHOTOS     " } else { "CUBEGM     " }
        $n = [Text.Encoding]::ASCII.GetBytes($name); [Array]::Copy($n, 0, $root, 0, 11); $root[11] = 0x10
        $fs.Position = ([long]$start + $reserved + 2 * $fatSize) * 512; $fs.Write($root, 0, 512)
    } finally { $fs.Close() }
    return $path
}
# What a stock card's FAT32 holds (only what the installer cares about).
function New-StockFolder([string]$path) {
    New-Item -ItemType Directory (Join-Path $path "cubegm") -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $path "cubegm\advapi32.dll"), [Text.Encoding]::ASCII.GetBytes("stock kernel"))
    [IO.File]::WriteAllBytes((Join-Path $path "cubegm\ApplicationFrame.dll"), [Text.Encoding]::ASCII.GetBytes("stock avp"))
    [IO.File]::WriteAllBytes((Join-Path $path "cubegm\Bubbles.scr"), [Text.Encoding]::ASCII.GetBytes("stock dtb"))
    [IO.File]::WriteAllBytes((Join-Path $path "cubegm\xgame-logo.bmp"), [Text.Encoding]::ASCII.GetBytes("stock start-up picture"))
    New-Item -ItemType Directory (Join-Path $path "games") -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $path "games\game.nes"), [byte[]](1, 2, 3))
    return $path
}
# A release's card\ folder (small stand-ins for the real 3.5 GiB / 1 GiB images).
function New-Release([string]$path, [string]$version = "v1") {
    $c = Join-Path $path "card"
    New-Item -ItemType Directory (Join-Path $c "cubegm") -Force | Out-Null
    New-Item -ItemType Directory (Join-Path $c "nzeus") -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $c "cubegm\advapi32.dll"), [Text.Encoding]::ASCII.GetBytes("nzeus kernel $version"))
    [IO.File]::WriteAllBytes((Join-Path $c "cubegm\ApplicationFrame.dll"), [Text.Encoding]::ASCII.GetBytes("nzeus avp stand-in"))
    [IO.File]::WriteAllBytes((Join-Path $c "cubegm\xgame-logo.bmp"), [Text.Encoding]::ASCII.GetBytes("NZEUS start-up picture"))
    $root = New-Object byte[] 1048576; $root[0] = [byte][char]$version[1]
    [IO.File]::WriteAllBytes((Join-Path $c "nzeus\nzroot.img"), $root)
    [IO.File]::WriteAllBytes((Join-Path $c "nzeus\nzdata.img"), (New-Object byte[] 524288))
    Set-Content -Path (Join-Path $c "nzeus\VERSION") -Value $version -NoNewline
    return $path
}
function Hash([string]$p) { (Get-FileHash -Algorithm SHA256 $p).Hash }
function Sector0([string]$img) { $fs = [IO.File]::OpenRead($img); try { $b = New-Object byte[] 512; [void]$fs.Read($b, 0, 512); $b } finally { $fs.Close() } }

$rel = New-Release (Join-Path $tmp "rel1") "v1"
$rel2 = New-Release (Join-Path $tmp "rel2") "v2"
$big = 100GB

# 1. only real R36S cards are offered
$imgs = @(
    (New-FakeCardImage (Join-Path $tmp "card.img")),
    (New-FakeCardImage (Join-Path $tmp "stick.img") -NoCubegm),
    (New-FakeCardImage (Join-Path $tmp "two.img") -TwoPartitions))
$found = @(Get-R36sCards -DiskImages $imgs)
Check "test_finds_only_r36s_cards" ($found.Count -eq 1 -and $found[0].Image -eq $imgs[0] -and $found[0].State -eq "closed") "found $($found.Count)"
# the real 16 GB stock card (2026-11-18): start-up code in sector 0, FAT32 at sector 32
$real16 = New-FakeCardImage (Join-Path $tmp "real16.img") -BootCode
$found = @(Get-R36sCards -DiskImages @($real16))
Check "test_finds_card_with_boot_code" ($found.Count -eq 1 -and $found[0].State -eq "closed") "found $($found.Count)"

# 2-3. fresh install: layout, backups
$card = New-StockFolder (Join-Path $tmp "fresh")
$before = @{ k = (Hash "$card\cubegm\advapi32.dll"); a = (Hash "$card\cubegm\ApplicationFrame.dll") }
Install-NzeusFiles -Root $card -Release $rel -FreeBytes $big | Out-Null
$want = @("cubegm\advapi32.dll", "cubegm\ApplicationFrame.dll", "nzeus\nzroot.img", "nzeus\nzdata.img", "nzeus\VERSION")
$ok = $true
foreach ($f in $want) { if ((Hash "$card\$f") -ne (Hash "$rel\card\$f")) { $ok = $false } }
Check "test_fresh_install_layout" ($ok -and (Test-Path "$card\nzeus\install-log.txt") -and (Test-Path "$card\games\game.nes"))
Check "test_backup_holds_originals" (((Hash "$card\nzeus\backup\advapi32.dll") -eq $before.k) -and
    ((Hash "$card\nzeus\backup\ApplicationFrame.dll") -eq $before.a) -and (Test-Path "$card\nzeus\backup\SHA256SUMS"))

# 4. second run with a newer release = update: system replaced, the user's data kept
[IO.File]::WriteAllBytes("$card\nzeus\nzdata.img", [Text.Encoding]::ASCII.GetBytes("my files"))
Install-NzeusFiles -Root $card -Release $rel2 -FreeBytes $big | Out-Null
Check "test_second_run_updates_keeps_data" (((Hash "$card\nzeus\nzroot.img") -eq (Hash "$rel2\card\nzeus\nzroot.img")) -and
    ([IO.File]::ReadAllText("$card\nzeus\nzdata.img") -eq "my files") -and ((Hash "$card\nzeus\backup\advapi32.dll") -eq $before.k))

# 5-6. uninstall: originals back byte for byte, data kept unless asked
Uninstall-NzeusFiles -Root $card | Out-Null
Check "test_uninstall_restores_byte_identical" (((Hash "$card\cubegm\advapi32.dll") -eq $before.k) -and
    ((Hash "$card\cubegm\ApplicationFrame.dll") -eq $before.a) -and -not (Test-Path "$card\nzeus\nzroot.img") -and -not (Test-Path "$card\nzeus\VERSION"))
Check "test_uninstall_keeps_data_by_default" ([IO.File]::ReadAllText("$card\nzeus\nzdata.img") -eq "my files")

# 7. a folder without cubegm is refused and left alone
$stick = Join-Path $tmp "stickfolder"; New-Item -ItemType Directory $stick | Out-Null
$threw = $false; try { Install-NzeusFiles -Root $stick -Release $rel -FreeBytes $big } catch { $threw = $_.Exception.Message -like "*not an R36S card*" }
Check "test_not_r36s_card_refused" ($threw -and @(Get-ChildItem $stick).Count -eq 0)

# 8. too little space: refused with the exact message, the card closed again, nothing copied
$img = New-FakeCardImage (Join-Path $tmp "full.img")
$full = New-StockFolder (Join-Path $tmp "full")
$msg = Invoke-NzeusInstall -DiskImage $img -Root $full -Release $rel -FreeBytes 1MB
Check "test_refuses_low_space_and_recloses" (($msg -like "Not enough free space: NZEUS needs * GB, the card has * GB free.") -and
    ((Get-R36sCards -DiskImages @($img))[0].State -eq "closed") -and -not (Test-Path "$full\nzeus")) $msg

# 9. interrupted after two files: a rerun finishes, no half files left
$cut = New-StockFolder (Join-Path $tmp "cut")
try { Install-NzeusFiles -Root $cut -Release $rel -FreeBytes $big -FailAfterFiles 2 | Out-Null } catch { }
$midVersion = Test-Path "$cut\nzeus\VERSION"
Install-NzeusFiles -Root $cut -Release $rel -FreeBytes $big | Out-Null
$parts = @(Get-ChildItem $cut -Recurse -Filter "*.part")
Check "test_install_resumes_after_interrupt" ((-not $midVersion) -and $parts.Count -eq 0 -and
    ((Hash "$cut\cubegm\advapi32.dll") -eq (Hash "$rel\card\cubegm\advapi32.dll")) -and
    ((Hash "$cut\nzeus\backup\advapi32.dll") -eq $before.k))

# 10. a card that was left open (old tool, crash) is closed at the end
$openImg = New-FakeCardImage (Join-Path $tmp "open.img") -Open
$openRoot = New-StockFolder (Join-Path $tmp "openroot")
$msg = Invoke-NzeusInstall -DiskImage $openImg -Root $openRoot -Release $rel -FreeBytes $big
Check "test_open_card_is_closed_at_end" (((Get-R36sCards -DiskImages @($openImg))[0].State -eq "closed") -and $msg -like "Done.*") $msg

# 11. Bubbles.scr is never touched by install, update or uninstall
$b = New-StockFolder (Join-Path $tmp "bubbles")
$bh = Hash "$b\cubegm\Bubbles.scr"; $bt = (Get-Item "$b\cubegm\Bubbles.scr").LastWriteTimeUtc
Install-NzeusFiles -Root $b -Release $rel -FreeBytes $big | Out-Null
Install-NzeusFiles -Root $b -Release $rel2 -FreeBytes $big | Out-Null
Uninstall-NzeusFiles -Root $b | Out-Null
Check "test_bubbles_never_touched" (((Hash "$b\cubegm\Bubbles.scr") -eq $bh) -and ((Get-Item "$b\cubegm\Bubbles.scr").LastWriteTimeUtc -eq $bt))

# ---- final-review fixes ----
# R1. a card installed by hand (backup files, no SHA256SUMS) keeps its stock backup on the next install
$hand = New-StockFolder (Join-Path $tmp "hand")
New-Item -ItemType Directory "$hand\nzeus\backup" -Force | Out-Null
Copy-Item "$hand\cubegm\advapi32.dll" "$hand\nzeus\backup\advapi32.dll"; Copy-Item "$hand\cubegm\ApplicationFrame.dll" "$hand\nzeus\backup\ApplicationFrame.dll"
Copy-Item "$rel\card\cubegm\advapi32.dll" "$hand\cubegm\advapi32.dll" -Force; Copy-Item "$rel\card\cubegm\ApplicationFrame.dll" "$hand\cubegm\ApplicationFrame.dll" -Force
Install-NzeusFiles -Root $hand -Release $rel2 -FreeBytes $big | Out-Null
Check "test_hand_installed_backup_kept" (((Hash "$hand\nzeus\backup\advapi32.dll") -eq $before.k) -and (Test-Path "$hand\nzeus\backup\SHA256SUMS"))

# R2. NZEUS on the card but no backup at all: refuse, change nothing (never back up NZEUS as "stock")
$nob = New-StockFolder (Join-Path $tmp "nobackup")
Copy-Item "$rel\card\cubegm\advapi32.dll" "$nob\cubegm\advapi32.dll" -Force
# (an OLDER NZEUS kernel can't be recognised without the key: it is recognised by what it left in nzeus/)
New-Item -ItemType Directory "$nob\nzeus" -Force | Out-Null; [IO.File]::WriteAllBytes("$nob\nzeus\nzdata.img", [byte[]](1, 2))
$threw = ""; try { Install-NzeusFiles -Root $nob -Release $rel2 -FreeBytes $big | Out-Null } catch { $threw = $_.Exception.Message }
Check "test_refuses_when_nzeus_but_no_backup" ($threw -like "*saved stock files*" -and -not (Test-Path "$nob\nzeus\backup")) $threw

# R3. a stale half copy from a pulled card is cleared before the space check
$st = New-StockFolder (Join-Path $tmp "stale")
New-Item -ItemType Directory "$st\nzeus" -Force | Out-Null
[IO.File]::WriteAllBytes("$st\nzeus\nzroot.img.part", (New-Object byte[] 2097152))
# free space is ~1 MB short UNLESS the 2 MB leftover counts as space NZEUS can take back
$threw = ""; try { Install-NzeusFiles -Root $st -Release $rel -FreeBytes (100MB + 600000) | Out-Null } catch { $threw = $_.Exception.Message }
Check "test_stale_part_removed" ((@(Get-ChildItem $st -Recurse -Filter "*.part").Count -eq 0) -and (Test-Path "$st\nzeus\VERSION")) $threw

# R4. cut off during the copies: NOTHING under the real names changed yet (no mixed versions)
$mix = New-StockFolder (Join-Path $tmp "mix")
try { Install-NzeusFiles -Root $mix -Release $rel -FreeBytes $big -FailAfterFiles 3 | Out-Null } catch { }
Check "test_interrupted_copy_changes_nothing" (((Hash "$mix\cubegm\advapi32.dll") -eq $before.k) -and
    ((Hash "$mix\cubegm\ApplicationFrame.dll") -eq $before.a) -and -not (Test-Path "$mix\nzeus\nzroot.img") -and -not (Test-Path "$mix\nzeus\VERSION"))

# R5. the read-back check catches a file that isn't what the release has
$rb = New-StockFolder (Join-Path $tmp "readback")
Install-NzeusFiles -Root $rb -Release $rel -FreeBytes $big | Out-Null
[IO.File]::WriteAllBytes("$rb\nzeus\nzroot.img", [byte[]](9, 9, 9))
$threw = ""; try { Test-NzeusFiles -Root $rb -Release $rel } catch { $threw = $_.Exception.Message }
Check "test_readback_check_catches_bad_copy" ($threw -like "*nzroot.img*") $threw

# R6. any failure half-way: the card is closed and the message says what to do
$fimg = New-FakeCardImage (Join-Path $tmp "fail.img")
$froot = New-StockFolder (Join-Path $tmp "failroot")
$msg = Invoke-NzeusInstall -DiskImage $fimg -Root $froot -Release $rel -FreeBytes $big -FailAfterFiles 2
Check "test_failed_install_message_says_rerun" (($msg -like "*run install.bat again*") -and ((Get-R36sCards -DiskImages @($fimg))[0].State -eq "closed")) $msg

# R7. uninstall that fails (no backup) still closes the card
$uimg = New-FakeCardImage (Join-Path $tmp "uninst.img")
$uroot = New-StockFolder (Join-Path $tmp "uninstroot")
$msg = Invoke-NzeusUninstall -DiskImage $uimg -Root $uroot
Check "test_uninstall_failure_closes_card" (((Get-R36sCards -DiskImages @($uimg))[0].State -eq "closed") -and $msg -like "*No NZEUS backup*") $msg

# ---- a full stock card: the installer removes the games itself (the card is closed, so a
# Windows user can't delete them by hand) ----
function Add-Games([string]$root) {
    New-Item -ItemType Directory "$root\GBA", "$root\MAME\images", "$root\roms", "$root\rootfs", "$root\BGM", "$root\mystuff" -Force | Out-Null
    [IO.File]::WriteAllBytes("$root\GBA\a.zip", (New-Object byte[] 2097152))           # 2 MB of "games"
    [IO.File]::WriteAllBytes("$root\MAME\images\p.png", (New-Object byte[] 1048576))   # 1 MB
    [IO.File]::WriteAllBytes("$root\roms\b.nes", (New-Object byte[] 1048576))          # 1 MB
    [IO.File]::WriteAllBytes("$root\rootfs\sys.bin", [byte[]](1, 2))                   # console files: keep
    [IO.File]::WriteAllBytes("$root\BGM\m.mp3", [byte[]](3, 4))
    [IO.File]::WriteAllBytes("$root\mystuff\f.txt", [byte[]](5, 6))                    # unknown folder: keep
}
# G1. only game folders are emptied; the folders themselves, the console's files and unknown folders stay
$g = New-StockFolder (Join-Path $tmp "games1"); Add-Games $g
$freed = Remove-NzeusGames -Root $g
Check "test_remove_games_keeps_console_files" (($freed -eq 4194304 -or $freed -eq 4194307) -and
    (Test-Path "$g\GBA") -and (Test-Path "$g\MAME") -and (Test-Path "$g\roms") -and
    @(Get-ChildItem "$g\GBA", "$g\MAME", "$g\roms" -Force).Count -eq 0 -and
    (Test-Path "$g\rootfs\sys.bin") -and (Test-Path "$g\BGM\m.mp3") -and (Test-Path "$g\mystuff\f.txt") -and
    ((Hash "$g\cubegm\advapi32.dll") -eq $before.k)) "freed=$freed"

# G2. too little space, -RemoveGames: the stock boot files are saved FIRST, then the games go, then NZEUS goes on
$need = 100MB + (Get-ChildItem "$rel\card" -Recurse -File | Measure-Object Length -Sum).Sum
$g2 = New-StockFolder (Join-Path $tmp "games2"); Add-Games $g2
$threw = ""; try { Install-NzeusFiles -Root $g2 -Release $rel -FreeBytes ($need - 1MB) | Out-Null } catch { $threw = $_.Exception.Message }
$keptGames = Test-Path "$g2\GBA\a.zip"
Install-NzeusFiles -Root $g2 -Release $rel -FreeBytes ($need - 1MB) -RemoveGames | Out-Null
$log = Get-Content "$g2\nzeus\install-log.txt"
$iSaved = [array]::FindIndex([string[]]$log, [Predicate[string]] { param($l) $l -like "*saved the stock*" })
$iGames = [array]::FindIndex([string[]]$log, [Predicate[string]] { param($l) $l -like "*removed the games*" })
Check "test_remove_games_makes_room" (($threw -like "Not enough free space*") -and $keptGames -and
    -not (Test-Path "$g2\GBA\a.zip") -and (Test-Path "$g2\nzeus\VERSION") -and
    ((Hash "$g2\nzeus\backup\advapi32.dll") -eq $before.k) -and $iSaved -ge 0 -and $iGames -gt $iSaved) "$threw | $($log -join ' / ')"

# G3. even the games aren't enough: refused BEFORE anything is deleted
$g3 = New-StockFolder (Join-Path $tmp "games3"); Add-Games $g3
$threw = ""; try { Install-NzeusFiles -Root $g3 -Release $rel -FreeBytes 1MB -RemoveGames | Out-Null } catch { $threw = $_.Exception.Message }
Check "test_remove_games_refused_when_not_enough" (($threw -like "*even without the games*bigger*") -and
    (Test-Path "$g3\GBA\a.zip") -and (Test-Path "$g3\roms\b.nes") -and -not (Test-Path "$g3\nzeus")) $threw

# G4. the whole run with -RemoveGames: done, and the card is closed again
$gimg = New-FakeCardImage (Join-Path $tmp "games4.img")
$g4 = New-StockFolder (Join-Path $tmp "games4"); Add-Games $g4
$msg = Invoke-NzeusInstall -DiskImage $gimg -Root $g4 -Release $rel -FreeBytes ($need - 1MB) -RemoveGames
Check "test_invoke_remove_games" (($msg -like "Done.*") -and ((Get-R36sCards -DiskImages @($gimg))[0].State -eq "closed") -and
    -not (Test-Path "$g4\GBA\a.zip")) $msg

# G5. install.ps1 offers to remove the games when the card is full (asks first, Y only)
$ip = Get-Content (Join-Path (Join-Path $here "..") "install.ps1") -Raw
Check "test_install_script_offers_game_removal" (($ip -match 'Not enough free space') -and ($ip -match '-RemoveGames') -and
    ($ip -match 'Read-Host'))

# H1. real card, 2026-11-18 (twice): after a big copy, writing sector 0 worked but Windows then
# said "Access to the path is denied" when the handle was closed. The card state is judged by
# reading the card back, so the install finishes ("Done.") and the card ends up closed.
$himg = New-FakeCardImage (Join-Path $tmp "closethrows.img")
$hroot = New-StockFolder (Join-Path $tmp "closethrows")
Set-NzeusTestHook -CloseThrows $true
try { $msg = Invoke-NzeusInstall -DiskImage $himg -Root $hroot -Release $rel -FreeBytes $big } finally { Set-NzeusTestHook -CloseThrows $false }
Check "test_close_complaint_after_write_is_ignored" (($msg -like "Done.*") -and
    ((Get-R36sCards -DiskImages @($himg))[0].State -eq "closed")) $msg

# H2. real card, 2026-11-18 19:46: right after closing it, Windows refused the write that opens
# it again ("Flush": Access to the path is denied) while it was still looking at the card's
# changed layout. The sector-0 write is tried again after a pause, so the install finishes.
$wimg = New-FakeCardImage (Join-Path $tmp "writedenied.img")
$wroot = New-StockFolder (Join-Path $tmp "writedenied")
Set-NzeusTestHook -WriteDeniedTimes 2
try { $msg = Invoke-NzeusInstall -DiskImage $wimg -Root $wroot -Release $rel -FreeBytes $big } finally { Set-NzeusTestHook -WriteDeniedTimes 0 }
Check "test_sector0_write_retried_when_windows_refuses" (($msg -like "Done.*") -and
    ((Get-R36sCards -DiskImages @($wimg))[0].State -eq "closed")) $msg
# ... and when Windows never lets go, the message says what to do and the card is not half done
$nimg = New-FakeCardImage (Join-Path $tmp "neverwrites.img")
$nroot = New-StockFolder (Join-Path $tmp "neverwrites")
Set-NzeusTestHook -WriteDeniedTimes 1000
try { $msg = Invoke-NzeusInstall -DiskImage $nimg -Root $nroot -Release $rel -FreeBytes $big } finally { Set-NzeusTestHook -WriteDeniedTimes 0 }
Check "test_sector0_write_refused_for_good_is_reported" (($msg -like "*run install.bat again*") -and
    ((Get-R36sCards -DiskImages @($nimg))[0].State -eq "closed")) $msg

# ---- the start-up picture (cubegm\xgame-logo.bmp): the console shows it at power-on ----
# L1. install: the stock picture is saved first, then the NZEUS one goes on; uninstall puts the stock one back
$lg = New-StockFolder (Join-Path $tmp "logo1")
$stockLogo = Hash "$lg\cubegm\xgame-logo.bmp"
Install-NzeusFiles -Root $lg -Release $rel -FreeBytes $big | Out-Null
$sums = @(Get-Content "$lg\nzeus\backup\SHA256SUMS")
$installed = ((Hash "$lg\cubegm\xgame-logo.bmp") -eq (Hash "$rel\card\cubegm\xgame-logo.bmp")) -and
    ((Hash "$lg\nzeus\backup\xgame-logo.bmp") -eq $stockLogo) -and ($sums -match 'xgame-logo\.bmp$').Count -eq 1
$readback = $true; try { Test-NzeusFiles -Root $lg -Release $rel } catch { $readback = $false }
Uninstall-NzeusFiles -Root $lg | Out-Null
Check "test_logo_saved_installed_and_restored" ($installed -and $readback -and ((Hash "$lg\cubegm\xgame-logo.bmp") -eq $stockLogo)) ($sums -join ' / ')

# L2. a card installed by an older release (backup of 2 files, stock picture still on): the update
# saves the stock picture now, puts the NZEUS one on, and uninstall still brings the stock one back
$lo = New-StockFolder (Join-Path $tmp "logo-old")
$oldStock = Hash "$lo\cubegm\xgame-logo.bmp"
New-Item -ItemType Directory "$lo\nzeus\backup" -Force | Out-Null
foreach ($n in "advapi32.dll", "ApplicationFrame.dll") { Copy-Item "$lo\cubegm\$n" "$lo\nzeus\backup\$n" }
Set-Content "$lo\nzeus\backup\SHA256SUMS" -Value @("advapi32.dll", "ApplicationFrame.dll" | ForEach-Object { "$((Get-FileHash -Algorithm SHA256 "$lo\nzeus\backup\$_").Hash.ToLower())  $_" })
foreach ($f in "advapi32.dll", "ApplicationFrame.dll") { Copy-Item "$rel\card\cubegm\$f" "$lo\cubegm\$f" -Force }
Set-Content "$lo\nzeus\VERSION" -Value "v0" -NoNewline
$threw = ""; try { Install-NzeusFiles -Root $lo -Release $rel2 -FreeBytes $big | Out-Null } catch { $threw = $_.Exception.Message }
$mid = ((Hash "$lo\cubegm\xgame-logo.bmp") -eq (Hash "$rel2\card\cubegm\xgame-logo.bmp")) -and ((Hash "$lo\nzeus\backup\xgame-logo.bmp") -eq $oldStock)
Uninstall-NzeusFiles -Root $lo | Out-Null
Check "test_logo_added_on_update_of_older_card" (-not $threw -and $mid -and ((Hash "$lo\cubegm\xgame-logo.bmp") -eq $oldStock) -and
    ((Hash "$lo\cubegm\advapi32.dll") -eq $before.k)) $threw

# L3. the NZEUS picture is already on the card but no saved stock one (can't know the stock picture):
# install still works and never saves NZEUS's picture as "stock"
$ln = New-StockFolder (Join-Path $tmp "logo-nzeus")
Copy-Item "$rel\card\cubegm\xgame-logo.bmp" "$ln\cubegm\xgame-logo.bmp" -Force
$threw = ""; try { Install-NzeusFiles -Root $ln -Release $rel -FreeBytes $big | Out-Null } catch { $threw = $_.Exception.Message }
Check "test_logo_never_backs_up_nzeus_picture" (-not $threw -and -not (Test-Path "$ln\nzeus\backup\xgame-logo.bmp") -and
    (Test-Path "$ln\nzeus\VERSION")) $threw

# ---- collect-logs.bat: NZEUS's logs off a closed card, for a bug report ----
# C1. the logs are copied to a new folder, the card's files stay exactly as they were, the card is closed
$cimg = New-FakeCardImage (Join-Path $tmp "logs.img")
$croot = New-StockFolder (Join-Path $tmp "logsroot")
New-Item -ItemType Directory "$croot\nzeus" -Force | Out-Null
Set-Content "$croot\nzeus\boot-log.txt" -Value "== NZEUS boot log, part 1 ==" -NoNewline
Set-Content "$croot\nzeus\boot-log-previous.txt" -Value "the start before" -NoNewline
Set-Content "$croot\nzeus\install-log.txt" -Value "installed NZEUS Deck v1" -NoNewline
Set-Content "$croot\nzeus\VERSION" -Value "v1" -NoNewline
$beforeHashes = @(Get-ChildItem "$croot\nzeus" -File | ForEach-Object { Hash $_.FullName })
$dest = Join-Path $tmp "collected"
$msg = Invoke-NzeusCollectLogs -DiskImage $cimg -Root $croot -Dest $dest
$got = @(Get-ChildItem $dest -File -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object)
$afterHashes = @(Get-ChildItem "$croot\nzeus" -File | ForEach-Object { Hash $_.FullName })
$want = @("boot-log.txt", "boot-log-previous.txt", "install-log.txt", "VERSION" | Sort-Object)
Check "test_collect_logs_copies_and_closes" (($got -join ",") -eq ($want -join ",") -and
    ([IO.File]::ReadAllText("$dest\boot-log.txt") -eq "== NZEUS boot log, part 1 ==") -and
    (($beforeHashes -join ",") -eq ($afterHashes -join ",")) -and
    ((Get-R36sCards -DiskImages @($cimg))[0].State -eq "closed") -and $msg -like "*$dest*") "$msg | $($got -join ',')"

# C2. a card without NZEUS logs: says so, copies nothing, card closed
$nimg2 = New-FakeCardImage (Join-Path $tmp "nologs.img")
$nroot2 = New-StockFolder (Join-Path $tmp "nologsroot")
$dest2 = Join-Path $tmp "collected2"
$msg = Invoke-NzeusCollectLogs -DiskImage $nimg2 -Root $nroot2 -Dest $dest2
Check "test_collect_logs_none_found" (($msg -like "*No NZEUS logs*") -and -not (Test-Path $dest2) -and
    ((Get-R36sCards -DiskImages @($nimg2))[0].State -eq "closed")) $msg

# C3. something fails half-way (the destination can't be made): the card is still closed again
$fimg2 = New-FakeCardImage (Join-Path $tmp "logsfail.img")
$froot2 = New-StockFolder (Join-Path $tmp "logsfailroot")
New-Item -ItemType Directory "$froot2\nzeus" -Force | Out-Null
Set-Content "$froot2\nzeus\boot-log.txt" -Value "x" -NoNewline
Set-Content (Join-Path $tmp "blocker") -Value "a file, not a folder" -NoNewline
$msg = Invoke-NzeusCollectLogs -DiskImage $fimg2 -Root $froot2 -Dest (Join-Path $tmp "blocker\sub")
Check "test_collect_logs_failure_closes_card" (((Get-R36sCards -DiskImages @($fimg2))[0].State -eq "closed") -and
    $msg -like "*collect-logs.bat again*") $msg

# 12. the scripts people double-click parse cleanly
$errs = @()
foreach ($f in @("install.ps1", "uninstall.ps1", "collect-logs.ps1")) {
    $e = $null; [void][Management.Automation.Language.Parser]::ParseFile((Join-Path (Join-Path $here "..") $f), [ref]$null, [ref]$e); $errs += $e
}
Check "test_scripts_parse" ($errs.Count -eq 0) ($errs -join "; ")

Remove-Item -Recurse -Force $tmp
if ($script:failures.Count) { Write-Output "FAILED: $($script:failures -join ', ')"; exit 1 }
Write-Output "$($script:total)/$($script:total) passed"
exit 0
