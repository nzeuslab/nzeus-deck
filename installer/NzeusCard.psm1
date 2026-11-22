# NzeusCard.psm1 - installs NZEUS Deck onto an R36S (HiChip E3100) SD card from Windows.
#
# A stock R36S card keeps its partition table (sector 0) scrambled: every 4 bytes
# XOR EF BE AD DE, so Windows sees an empty disk. Installing therefore means:
#   open the card (unscramble sector 0) -> copy files to its FAT32 -> close it again.
# Only sector 0 is ever written raw; everything else is normal file copying.
# Same card rules as tools/r36s_card.py and the console's nz-fatroot.
#
# Test seams (no admin, no card): -DiskImage / -DiskImages (a card image file
# stands in for the disk), -Root (a folder stands in for the drive),
# -FreeBytes, -FailAfterFiles.

$Script:Key = [byte[]](0xEF, 0xBE, 0xAD, 0xDE)
$Script:Margin = 100MB                       # free space kept on top of what NZEUS needs
$Script:TestCloseThrows = $false             # test seam: closing the card's handle complains
$Script:TestWriteDenied = 0                  # test seam: the next N sector-0 writes are refused
$Script:Sector0Tries = 10                    # sector-0 write: tries, one second apart
$Script:Sector0PauseMs = 1000

# Tests only: make the card's handle behave like Windows did with the real card.
function Set-NzeusTestHook([bool]$CloseThrows = $false, [int]$WriteDeniedTimes = 0) {
    $Script:TestCloseThrows = $CloseThrows
    $Script:TestWriteDenied = $WriteDeniedTimes
}

# ---------------- sector 0 ----------------

function Switch-R36sScramble([byte[]]$sector) {
    # The factory scramble. Doing it twice gives the original back.
    $out = New-Object byte[] 512
    for ($i = 0; $i -lt 512; $i++) { $out[$i] = $sector[$i] -bxor $Script:Key[$i % 4] }
    return , $out
}

function Read-At($stream, [long]$offset, [int]$count) {
    $buf = New-Object byte[] $count
    $stream.Position = $offset
    $got = 0
    while ($got -lt $count) { $n = $stream.Read($buf, $got, $count - $got); if ($n -le 0) { break }; $got += $n }
    if ($got -ne $count) { throw "could not read $count bytes at $offset" }
    return , $buf
}
function U16($b, $o) { return [BitConverter]::ToUInt16($b, $o) }
function U32($b, $o) { return [BitConverter]::ToUInt32($b, $o) }

# Is this (plain) sector 0 an R36S card's table? Returns "" or the reason it isn't.
function Test-R36sTable([byte[]]$mbr, $stream, [long]$size) {
    if ($mbr[510] -ne 0x55 -or $mbr[511] -ne 0xAA) { return "no partition-table mark" }
    # (bytes 0-439 may hold start-up code: the 16 GB stock card has some, the 64 GB one doesn't)
    $type = $mbr[446 + 4]; $start = U32 $mbr (446 + 8); $count = U32 $mbr (446 + 12)
    if ($type -ne 0x0B -and $type -ne 0x0C) { return "the first partition is not FAT32" }
    for ($i = 446 + 16; $i -lt 510; $i++) { if ($mbr[$i] -ne 0) { return "more than one partition" } }
    if ($start -lt 1 -or $count -lt 1) { return "an empty partition" }
    if (([long]$start + $count) * 512 -gt $size) { return "the partition is bigger than the card" }
    $boot = Read-At $stream ([long]$start * 512) 512
    if ($boot[510] -ne 0x55 -or $boot[511] -ne 0xAA -or [Text.Encoding]::ASCII.GetString($boot, 0x52, 8) -ne "FAT32   ") {
        return "no FAT32 file system where the partition starts"
    }
    if ((U32 $boot 0x1C) -ne $start -or (U32 $boot 0x20) -ne $count) { return "the file system doesn't match the partition table" }
    $bps = U16 $boot 11; $spc = $boot[13]; $reserved = U16 $boot 14; $fats = $boot[16]
    $fatSize = U32 $boot 36; $rootCluster = U32 $boot 44
    if ($bps -ne 512 -or $spc -lt 1 -or $rootCluster -lt 2) { return "an unusual FAT32 layout" }
    $rootAt = ([long]$start + $reserved + [long]$fats * $fatSize + ([long]$rootCluster - 2) * $spc) * 512
    $root = Read-At $stream $rootAt ($spc * 512)
    for ($e = 0; $e -lt $root.Length; $e += 32) {
        if ($root[$e] -eq 0) { break }
        if ([Text.Encoding]::ASCII.GetString($root, $e, 11) -eq "CUBEGM     " -and ($root[$e + 11] -band 0x10)) { return "" }
    }
    return "no CUBEGM folder (not an R36S card)"
}

function Get-CardPath([int]$Number, [string]$DiskImage) {
    if ($DiskImage) { return $DiskImage }
    return "\\.\PhysicalDrive$Number"
}

# open | closed | unknown, for a disk or an image file
function Get-R36sState([string]$path, [long]$size) {
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $s0 = Read-At $fs 0 512
        if (-not (Test-R36sTable $s0 $fs $size)) { return "open" }
        if (-not (Test-R36sTable (Switch-R36sScramble $s0) $fs $size)) { return "closed" }
        return "unknown"
    } finally { $fs.Close() }
}

# The removable disks (or test images) that are R36S cards.
function Get-R36sCards([string[]]$DiskImages) {
    $cards = @()
    if ($DiskImages) {
        foreach ($img in $DiskImages) {
            $st = Get-R36sState $img (Get-Item $img).Length
            if ($st -ne "unknown") { $cards += [pscustomobject]@{ Number = -1; Image = $img; FriendlyName = (Split-Path -Leaf $img); SizeBytes = (Get-Item $img).Length; State = $st } }
        }
        return $cards
    }
    foreach ($d in (Get-Disk | Where-Object { $_.BusType -in @("SD", "USB", "MMC") -and -not $_.IsBoot -and -not $_.IsSystem })) {
        try { $st = Get-R36sState (Get-CardPath $d.Number) ([long]$d.Size) } catch { continue }
        if ($st -ne "unknown") { $cards += [pscustomobject]@{ Number = $d.Number; Image = ""; FriendlyName = $d.FriendlyName; SizeBytes = [long]$d.Size; State = $st } }
    }
    return $cards
}

# Writes sector 0 in the wanted state and reads it back. No-op if already so.
# Right after the card's layout changed, Windows refuses raw writes for a few seconds while it
# looks at the "new" disk (real card, 2026-11-18 19:46: "Access to the path is denied" on the
# write that opens it again). So: up to Sector0Tries tries, a pause between them. Every try
# reads the card first - a write Windows complained about may still have landed.
function Set-R36sState([string]$path, [long]$size, [string]$want) {
    for ($try = 1; ; $try++) {
        $st = Get-R36sState $path $size
        if ($st -eq $want) { return }
        if ($st -eq "unknown") { throw "this disk is not an R36S card. Nothing was changed." }
        try {
            Switch-Sector0 $path
        } catch {
            if ($try -ge $Script:Sector0Tries) { throw }
            Start-Sleep -Milliseconds $Script:Sector0PauseMs
            continue
        }
        if ((Get-R36sState $path $size) -ne $want) { throw "the card is not $want after writing sector 0" }
        return
    }
}

# One try: flip sector 0 between open and closed (the same XOR both ways), read it back.
function Switch-Sector0([string]$path) {
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        if ($Script:TestWriteDenied -gt 0) {
            $Script:TestWriteDenied--
            throw (New-Object UnauthorizedAccessException "test: Access to the path is denied.")
        }
        $new = Switch-R36sScramble (Read-At $fs 0 512)
        $fs.Position = 0; $fs.Write($new, 0, 512); $fs.Flush()
        $back = Read-At $fs 0 512
        if ([Convert]::ToBase64String($back) -ne [Convert]::ToBase64String($new)) { throw "sector 0 read back differently: the card may be failing" }
    } finally {
        # After a big copy, Windows sometimes answers "Access to the path is denied" when this
        # handle is closed, although the write is done (real card, 2026-11-18, twice). The card
        # itself decides: it is read again just below.
        try {
            $fs.Close()
            if ($Script:TestCloseThrows) { throw "test: Access to the path is denied." }
        } catch { }
    }
}

# Opens the card and returns its drive root (for example "E:\"). Images return "".
function Open-R36sCard([int]$Number = -1, [string]$DiskImage) {
    $path = Get-CardPath $Number $DiskImage
    $size = if ($DiskImage) { (Get-Item $DiskImage).Length } else { [long](Get-Disk -Number $Number).Size }
    Set-R36sState $path $size "open"
    if ($DiskImage) { return "" }
    Update-Disk -Number $Number
    for ($i = 0; $i -lt 30; $i++) {                  # Windows needs a moment to see the FAT32
        $part = Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($part) {
            if (-not $part.DriveLetter -or $part.DriveLetter -eq [char]0) {
                $part | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue
                $part = Get-Partition -DiskNumber $Number | Select-Object -First 1
            }
            if ($part.DriveLetter -and $part.DriveLetter -ne [char]0 -and (Test-Path "$($part.DriveLetter):\")) { return "$($part.DriveLetter):\" }
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Windows did not show the card as a drive. Take the card out, put it back and run me again."
}

# Flushes and hides the drive, then scrambles sector 0 again (as the console expects).
function Close-R36sCard([int]$Number = -1, [string]$DiskImage) {
    $path = Get-CardPath $Number $DiskImage
    if (-not $DiskImage) {
        $part = Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($part -and $part.DriveLetter -and $part.DriveLetter -ne [char]0) {
            Write-VolumeCache -DriveLetter $part.DriveLetter -ErrorAction SilentlyContinue
            $part | Remove-PartitionAccessPath -AccessPath "$($part.DriveLetter):\" -ErrorAction SilentlyContinue
        }
    }
    $size = if ($DiskImage) { (Get-Item $DiskImage).Length } else { [long](Get-Disk -Number $Number).Size }
    Set-R36sState $path $size "closed"
    if (-not $DiskImage) { Update-Disk -Number $Number }
}

# ---------------- new (empty) cards ----------------
#
# The console's bootloader only reads a card whose sector 0 is scrambled AND carries the
# bootloader's "scrambled" flag: bytes 444-445 = 69 96 on the card, which is 86 28 before
# scrambling (factory bootloader code at 0x7c334: if the flag is there, it XORs the sector with
# DEADBEEF; if not, it reads garbage and shows "Please insert TF card"). Every stock card has the
# flag. Proven on the console 2026-11-22: the same card boots with the flag and not without it.
#
# It also loads cubegm\Bubbles.scr (the console's own board file) and stops with "TF card is
# damaged" if it is missing. That file is the console maker's, so NZEUS never ships it: it is
# copied from the user's own original card (Save-BoardFile). Nothing else from the stock card is needed.

$Script:BoardFileMagic = [byte[]](0xD0, 0x0D, 0xFE, 0xED)   # a device tree file starts with this
$Script:NewCardMarker = "nzeus\new-card"                     # on cards set up by install-new-card.bat

# FAT32 layout for a new card of SizeBytes: partition at 1 MB (as the stock 64 GB card),
# 16 KB clusters (as the stock cards), data area starting on a cluster boundary.
function Get-NewCardLayout([long]$SizeBytes) {
    $start = 2048; $spc = 32; $fats = 2
    $count = [long][math]::Floor($SizeBytes / 512) - $start
    $count -= $count % 2048                                   # whole megabytes
    if ($count -gt [uint32]::MaxValue) { throw "this card is bigger than 2 TB, which FAT32 can't use. Nothing was changed." }
    $reserved = 32
    # Microsoft's FAT32 size formula (fatgen103): always big enough
    $fatSize = [long][math]::Ceiling(($count - $reserved) / ((256 * $spc + $fats) / 2))
    $reserved += ($spc - (($start + $reserved + $fats * $fatSize) % $spc)) % $spc
    $clusters = [long][math]::Floor(($count - $reserved - $fats * $fatSize) / $spc)
    if ($clusters -lt 65525) { throw "this card is too small. Nothing was changed." }
    return [pscustomobject]@{ Start = $start; Count = $count; Reserved = $reserved; Fats = $fats
        FatSize = $fatSize; SectorsPerCluster = $spc; Clusters = $clusters }
}

function Set-Le([byte[]]$b, [int]$o, [long]$v, [int]$n) { for ($i = 0; $i -lt $n; $i++) { $b[$o + $i] = [byte](($v -shr (8 * $i)) -band 0xFF) } }

# A FAT directory entry (name = 11 characters, 8.3 style).
function New-DirEntry([string]$name, [byte]$attr, [long]$cluster) {
    $e = New-Object byte[] 32
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes($name), 0, $e, 0, 11)
    $e[11] = $attr
    $now = Get-Date
    $time = ($now.Hour -shl 11) -bor ($now.Minute -shl 5) -bor [int]($now.Second / 2)
    $date = (($now.Year - 1980) -shl 9) -bor ($now.Month -shl 5) -bor $now.Day
    foreach ($o in @(14, 22)) { Set-Le $e $o $time 2 }
    foreach ($o in @(16, 18, 24)) { Set-Le $e $o $date 2 }
    Set-Le $e 20 ($cluster -shr 16) 2; Set-Le $e 26 ($cluster -band 0xFFFF) 2
    return , $e
}

# Writes a complete, empty R36S card: scrambled + flagged partition table, FAT32 with one empty
# CUBEGM folder (what makes it an R36S card for Get-R36sCards / Open-R36sCard). Reads it back.
function Write-NewCardLayout([string]$path, $L) {
    $mbr = New-Object byte[] 512
    $sig = New-Object byte[] 4; (New-Object Random).NextBytes($sig); [Array]::Copy($sig, 0, $mbr, 440, 4)
    $mbr[444] = 0x86; $mbr[445] = 0x28                       # the bootloader's flag (69 96 once scrambled)
    [Array]::Copy([byte[]](0x00, 0x20, 0x21, 0x00, 0x0C, 0xFE, 0xFF, 0xFF), 0, $mbr, 446, 8)   # as the stock 64 GB card
    Set-Le $mbr 454 $L.Start 4; Set-Le $mbr 458 $L.Count 4
    $mbr[510] = 0x55; $mbr[511] = 0xAA
    $mbr = Switch-R36sScramble $mbr

    $boot = New-Object byte[] 512
    [Array]::Copy([byte[]](0xEB, 0x58, 0x90), 0, $boot, 0, 3)
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes("MSDOS5.0"), 0, $boot, 3, 8)
    Set-Le $boot 11 512 2; $boot[13] = $L.SectorsPerCluster; Set-Le $boot 14 $L.Reserved 2; $boot[16] = $L.Fats
    $boot[21] = 0xF8; Set-Le $boot 24 63 2; Set-Le $boot 26 255 2
    Set-Le $boot 28 $L.Start 4; Set-Le $boot 32 $L.Count 4; Set-Le $boot 36 $L.FatSize 4
    Set-Le $boot 44 2 4; Set-Le $boot 48 1 2; Set-Le $boot 50 6 2      # root folder cluster 2, FSInfo 1, backup 6
    $boot[64] = 0x80; $boot[66] = 0x29
    $serial = New-Object byte[] 4; (New-Object Random).NextBytes($serial); [Array]::Copy($serial, 0, $boot, 67, 4)
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes("NO NAME    FAT32   "), 0, $boot, 71, 19)
    $boot[510] = 0x55; $boot[511] = 0xAA
    $info = New-Object byte[] 512
    Set-Le $info 0 0x41615252 4; Set-Le $info 484 0x61417272 4
    Set-Le $info 488 ($L.Clusters - 2) 4; Set-Le $info 492 4 4; Set-Le $info 508 0xAA550000 4
    $fat = New-Object byte[] 512                              # clusters 2 (top folder) and 3 (CUBEGM): one cluster each
    Set-Le $fat 0 0x0FFFFFF8 4; Set-Le $fat 4 0x0FFFFFFF 4; Set-Le $fat 8 0x0FFFFFFF 4; Set-Le $fat 12 0x0FFFFFFF 4
    $clusterBytes = $L.SectorsPerCluster * 512
    $root = New-Object byte[] $clusterBytes; [Array]::Copy((New-DirEntry "CUBEGM     " 0x10 3), 0, $root, 0, 32)
    $cubegm = New-Object byte[] $clusterBytes
    [Array]::Copy((New-DirEntry ".          " 0x10 3), 0, $cubegm, 0, 32)
    [Array]::Copy((New-DirEntry "..         " 0x10 0), 0, $cubegm, 32, 32)

    $p = [long]$L.Start * 512
    $fatAt = $p + [long]$L.Reserved * 512
    $dataAt = $fatAt + [long]$L.Fats * $L.FatSize * 512
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        # wipe everything from sector 0 to the end of the two folders, 1 MB at a time
        $zero = New-Object byte[] 1MB
        $end = $dataAt + 2 * $clusterBytes
        $fs.Position = 0
        while ($fs.Position -lt $end) { $fs.Write($zero, 0, [int][math]::Min($zero.Length, $end - $fs.Position)) }
        # (positions in brackets: in PowerShell the comma binds tighter than +)
        $writes = @(@(0, $mbr), @($p, $boot), @(($p + 512), $info), @(($p + 6 * 512), $boot), @(($p + 7 * 512), $info),
            @($fatAt, $fat), @(($fatAt + [long]$L.FatSize * 512), $fat), @($dataAt, $root), @(($dataAt + $clusterBytes), $cubegm))
        foreach ($w in $writes) { $fs.Position = $w[0]; $fs.Write($w[1], 0, $w[1].Length) }
        $fs.Flush()
        foreach ($w in $writes) {
            if ([Convert]::ToBase64String((Read-At $fs $w[0] $w[1].Length)) -ne [Convert]::ToBase64String($w[1])) {
                throw "the new card read back differently: it may be failing or fake. Try another card."
            }
        }
    } finally { $fs.Close() }
}

# Is this file a device tree (what Bubbles.scr is)? "" or the reason it isn't.
function Test-BoardFile([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "the file is missing" }
    $len = (Get-Item -LiteralPath $path).Length
    if ($len -lt 64 -or $len -gt 1MB) { return "it has the wrong size" }
    $b = [IO.File]::ReadAllBytes($path)
    for ($i = 0; $i -lt 4; $i++) { if ($b[$i] -ne $Script:BoardFileMagic[$i]) { return "it is not a board file" } }
    return ""
}

# Removable disks a new card could be (any card reader disk that isn't Windows' own).
function Get-NewCardDisks([string[]]$DiskImages) {
    $out = @()
    if ($DiskImages) {
        foreach ($img in $DiskImages) {
            $st = Get-R36sState $img (Get-Item $img).Length
            $out += [pscustomobject]@{ Number = -1; Image = $img; FriendlyName = (Split-Path -Leaf $img); SizeBytes = (Get-Item $img).Length; IsR36s = ($st -ne "unknown") }
        }
        return $out
    }
    foreach ($d in (Get-Disk | Where-Object { $_.BusType -in @("SD", "USB", "MMC") -and -not $_.IsBoot -and -not $_.IsSystem -and [long]$_.Size -gt 0 })) {
        $isR36s = $false
        try { $isR36s = (Get-R36sState (Get-CardPath $d.Number) ([long]$d.Size)) -ne "unknown" } catch { }
        $out += [pscustomobject]@{ Number = $d.Number; Image = ""; FriendlyName = $d.FriendlyName; SizeBytes = [long]$d.Size; IsR36s = $isR36s }
    }
    return $out
}

# install-new-card.bat, step 1: copies cubegm\Bubbles.scr off the user's ORIGINAL card to Dest.
# The card is only opened, read and closed again. Returns the message for the user.
function Invoke-NzeusSaveBoardFile([int]$Number = -1, [string]$DiskImage, [string]$Root, [string]$Dest) {
    try {
        $drive = Open-R36sCard -Number $Number -DiskImage $DiskImage
        if (-not $Root) { $Root = $drive }
        $from = Join-Path $Root "cubegm\Bubbles.scr"
        $why = Test-BoardFile $from
        if ($why) { throw "This card has no usable console board file (cubegm\Bubbles.scr: $why). Use the card that came with the console." }
        New-Item -ItemType Directory -Force (Split-Path -Parent $Dest) | Out-Null
        Copy-Item -LiteralPath $from -Destination $Dest -Force
        if ((Get-Sha $Dest) -ne (Get-Sha $from)) { Remove-Item -LiteralPath $Dest -Force; throw "the board file did not copy correctly." }
        $msg = "Saved the console's board file from your original card. Nothing on that card was changed."
    } catch {
        $msg = "Could not read your original card: $($_.Exception.Message)"
    } finally {
        $msg += Close-IfOpen $Number $DiskImage "install-new-card.bat"
    }
    return $msg
}

# install-new-card.bat, step 2: ERASES the disk, writes a fresh R36S card layout, puts the board
# file and NZEUS on it, reads everything back from a fresh mount and ALWAYS closes the card.
function Invoke-NzeusNewCard([int]$Number = -1, [string]$DiskImage, [string]$Release, [string]$BoardFile, [string]$Root, [long]$FreeBytes = -1) {
    Set-StayAwake $true
    $msg = ""
    try {
        $why = Test-BoardFile $BoardFile
        if ($why) { throw "The saved board file can't be used ($why). Delete $BoardFile and run install-new-card.bat again. Nothing was changed." }
        if ($DiskImage) {
            $size = (Get-Item $DiskImage).Length
        } else {
            $disk = Get-Disk -Number $Number
            if ($disk.IsBoot -or $disk.IsSystem) { throw "disk $Number is one of Windows' own disks. Nothing was changed." }
            $size = [long]$disk.Size
        }
        $layout = Get-NewCardLayout $size
        if (-not $DiskImage -and $disk.PartitionStyle -ne "RAW") {
            # Windows must let go of the card's old drive before it can be written directly
            Clear-Disk -Number $Number -RemoveData -RemoveOEM -Confirm:$false
            Start-Sleep -Seconds 2
        }
        Write-NewCardLayout (Get-CardPath $Number $DiskImage) $layout
        $drive = Open-R36sCard -Number $Number -DiskImage $DiskImage
        if (-not $Root) { $Root = $drive }
        New-Item -ItemType Directory -Force (Join-Path $Root "nzeus") | Out-Null
        Set-Content -LiteralPath (Join-Path $Root $Script:NewCardMarker) -Value "Set up by install-new-card.bat: this card has NZEUS only, not the console's own system."
        Copy-ToPart $BoardFile (Join-Path $Root "cubegm\Bubbles.scr")
        Commit-Parts $Root @("cubegm\Bubbles.scr")
        Write-Log $Root "new card: wrote a fresh R36S card layout and the console's board file"
        Install-NzeusFiles -Root $Root -Release $Release -FreeBytes $FreeBytes -NewCard | Out-Null
        if (-not $DiskImage) {                       # fresh mount: the read-back really reads the card
            Close-R36sCard -Number $Number
            $Root = Open-R36sCard -Number $Number
        }
        Test-NzeusFiles -Root $Root -Release $Release
        if ((Get-Sha (Join-Path $Root "cubegm\Bubbles.scr")) -ne (Get-Sha $BoardFile)) { throw "Bubbles.scr did not read back correctly from the card. The card may be failing or fake: try another card." }
        $msg = "Done. Eject the new card, put it in the console and switch it on. NZEUS starts in about 30 seconds. Keep your original card: it still has the console's own system."
    } catch {
        $msg = $_.Exception.Message
        if ($msg -notlike "*Nothing was changed*") { $msg += " Run install-new-card.bat again: it starts the new card over." }
    } finally {
        $msg += Close-IfOpen $Number $DiskImage "install-new-card.bat"
        Set-StayAwake $false
    }
    return $msg
}

# ---------------- files ----------------

function Get-Sha([string]$p) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower() }

# Copy to NAME.part and check its SHA-256. The real name is only touched later (Commit-Parts),
# so a cut-off run never leaves a half file - or a mix of old and new files - under the real names.
function Copy-ToPart([string]$from, [string]$to) {
    $part = "$to.part"
    New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
    Copy-Item -LiteralPath $from -Destination $part -Force
    if ((Get-Sha $part) -ne (Get-Sha $from)) { Remove-Item -LiteralPath $part -Force; throw "$(Split-Path -Leaf $to) did not copy correctly (the card may be failing)" }
}
# All .part files become the real ones in one quick burst (VERSION last = finished).
function Commit-Parts([string]$root, [string[]]$files) {
    foreach ($f in $files) { Move-Item -LiteralPath (Join-Path $root "$f.part") -Destination (Join-Path $root $f) -Force }
}

function Write-Log([string]$root, [string]$text) {
    Add-Content -LiteralPath (Join-Path $root "nzeus\install-log.txt") -Value ("{0:yyyy-MM-dd HH:mm:ss}  {1}" -f (Get-Date), $text)
}

function Get-FreeBytes([string]$root) {
    return [long]([IO.DriveInfo]::new([IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $root).Path))).AvailableFreeSpace
}

# The files a run copies, in order; the kernel comes after the system, VERSION last.
function Get-NzeusFileList([string]$Root) {
    $files = @("nzeus\nzroot.img")
    if (-not (Test-Path (Join-Path $Root "nzeus\nzdata.img"))) { $files += "nzeus\nzdata.img" }
    return $files + @("cubegm\ApplicationFrame.dll", "cubegm\xgame-logo.bmp", "cubegm\advapi32.dll", "nzeus\VERSION")
}

# Saves the card's stock boot files to nzeus\backup ONCE. Never overwrites a saved file, and never
# saves NZEUS's own files as "stock" (they would replace the maker's files, which nobody can give back).
function Save-StockBackup([string]$Root, [string]$Release) {
    if (Test-Path (Join-Path $Root $Script:NewCardMarker)) { return }   # a new card never had the stock system
    $backup = Join-Path $Root "nzeus\backup"
    $names = @("advapi32.dll", "ApplicationFrame.dll")
    $have = @($names | Where-Object { Test-Path (Join-Path $backup $_) })
    if ($have.Count -eq 0) {
        $kernel = Get-Sha (Join-Path $Root "cubegm\advapi32.dll")
        $nzeusKernels = @(Get-ChildItem -LiteralPath $Release -Recurse -Filter "advapi32.dll" | ForEach-Object { Get-Sha $_.FullName })
        $leftovers = @("VERSION", "nzroot.img", "nzdata.img", "install-log.txt") | Where-Object { Test-Path (Join-Path $Root "nzeus\$_") }
        if ($leftovers -or ($nzeusKernels -contains $kernel)) {
            throw "This card already has NZEUS on it, but the saved stock files (nzeus\backup) are missing, so they can't be saved again. Nothing was changed."
        }
        New-Item -ItemType Directory -Force $backup | Out-Null
        foreach ($n in $names) {
            Copy-ToPart (Join-Path $Root "cubegm\$n") (Join-Path $backup $n)
            Move-Item -LiteralPath (Join-Path $backup "$n.part") -Destination (Join-Path $backup $n)
        }
        Write-Log $Root "saved the stock advapi32.dll and ApplicationFrame.dll to nzeus\backup"
    } elseif ($have.Count -ne 2) {
        throw "Only part of the saved stock files is on this card (nzeus\backup). Nothing was changed."
    }
    if (-not (Test-Path (Join-Path $backup "SHA256SUMS"))) {     # e.g. a card installed by hand
        Set-Content -LiteralPath (Join-Path $backup "SHA256SUMS") -Value @($names | ForEach-Object { "$(Get-Sha (Join-Path $backup $_))  $_" })
    }
    # The start-up picture (cubegm\xgame-logo.bmp, shown at power-on). Added later, so a card set up
    # by an older release has no saved copy yet: it is saved once, and only while the card still has
    # a picture that isn't NZEUS's own (NZEUS's picture must never become the "stock" one).
    $logo = Join-Path $Root "cubegm\xgame-logo.bmp"
    $savedLogo = Join-Path $backup "xgame-logo.bmp"
    $nzeusLogos = @(Get-ChildItem -LiteralPath $Release -Recurse -Filter "xgame-logo.bmp" | ForEach-Object { Get-Sha $_.FullName })
    if (-not (Test-Path $savedLogo) -and (Test-Path $logo) -and ($nzeusLogos -notcontains (Get-Sha $logo))) {
        Copy-ToPart $logo $savedLogo
        Move-Item -LiteralPath "$savedLogo.part" -Destination $savedLogo
        Add-Content -LiteralPath (Join-Path $backup "SHA256SUMS") -Value "$(Get-Sha $savedLogo)  xgame-logo.bmp"
        Write-Log $Root "saved the stock start-up picture (xgame-logo.bmp) to nzeus\backup"
    }
}

# The folders stock R36S cards keep their games in (one per game system). Only these are emptied.
$Script:GameFolders = @("ARCADE", "ATARI", "CPS1", "CPS2", "CPS3", "DOS", "FBA", "FBNEO", "FC", "FDS", "GB", "GBA", "GBC",
    "GG", "GW", "LYNX", "MAME", "MD", "MSX", "N64", "NDS", "NEOGEO", "NES", "NGP", "NGPC", "PCE", "PCECD", "POKEMINI",
    "PS", "PS1", "PSP", "ROMS", "SEGACD", "SFC", "SG1000", "SMS", "SNES", "VB", "WS", "WSC")

# The bytes the game folders hold (what removing the games would give back).
function Get-NzeusGamesSize([string]$Root) {
    $sum = [long]0
    foreach ($n in $Script:GameFolders) {
        $p = Join-Path $Root $n
        if (Test-Path -LiteralPath $p -PathType Container) {
            $s = (Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
            if ($s) { $sum += [long]$s }
        }
    }
    return $sum
}

# Empties every game folder (the folders stay, so the console's menu still finds them).
# Keeps going past items that can't be deleted (stock cards can have damaged entries). Returns bytes freed.
function Remove-NzeusGames([string]$Root) {
    $freed = [long]0; $failed = 0
    foreach ($n in $Script:GameFolders) {
        $p = Join-Path $Root $n
        if (-not (Test-Path -LiteralPath $p -PathType Container)) { continue }
        foreach ($item in @(Get-ChildItem -LiteralPath $p -Force -ErrorAction SilentlyContinue)) {
            $size = if ($item.PSIsContainer) {
                [long](Get-ChildItem -LiteralPath $item.FullName -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
            } else { [long]$item.Length }
            try { Remove-Item -LiteralPath $item.FullName -Recurse -Force -ErrorAction Stop; $freed += $size } catch { $failed++ }
        }
    }
    New-Item -ItemType Directory -Force (Join-Path $Root "nzeus") | Out-Null
    Write-Log $Root ("removed the games to make room ({0:N1} GB freed{1})" -f ($freed / 1GB), $(if ($failed) { ", $failed damaged items left" } else { "" }))
    return $freed
}

# Install or update. Release = the unzipped release folder (it has card\...).
# -RemoveGames: when the card is too full, save the stock boot files, then empty the game folders.
# -NewCard: a card install-new-card.bat just set up (board file only, no stock system to save).
function Install-NzeusFiles([string]$Root, [string]$Release, [long]$FreeBytes = -1, [int]$FailAfterFiles = -1, [switch]$RemoveGames, [switch]$NewCard) {
    $measured = $FreeBytes -lt 0                   # tests pass -FreeBytes; a real run measures the card
    $src = Join-Path $Release "card"
    if ($NewCard) {
        if (-not (Test-Path (Join-Path $Root "cubegm\Bubbles.scr")) -or -not (Test-Path (Join-Path $Root $Script:NewCardMarker))) {
            throw "This is not a card set up by install-new-card.bat. Nothing was changed."
        }
    } elseif (-not (Test-Path (Join-Path $Root "cubegm\advapi32.dll")) -or -not (Test-Path (Join-Path $Root "cubegm\Bubbles.scr"))) {
        throw "This drive is not an R36S card (no cubegm\advapi32.dll and cubegm\Bubbles.scr). Nothing was changed."
    }
    if ($FreeBytes -lt 0) { $FreeBytes = Get-FreeBytes $Root }
    # half copies from an earlier run that was cut off: their space comes back
    foreach ($d in @("nzeus", "cubegm", "nzeus\backup")) {
        $p = Join-Path $Root $d
        if (Test-Path $p) {
            foreach ($f in @(Get-ChildItem -LiteralPath $p -Filter "*.part" -File)) { $FreeBytes += $f.Length; Remove-Item -LiteralPath $f.FullName -Force }
        }
    }
    $files = Get-NzeusFileList $Root
    # space: every new file once (as .part), next to the old copies until the final swap
    $need = $Script:Margin
    foreach ($f in $files) { $need += (Get-Item (Join-Path $src $f)).Length }
    if ($FreeBytes -lt $need) {
        if (-not $RemoveGames) {
            throw ("Not enough free space: NZEUS needs {0:N1} GB, the card has {1:N1} GB free." -f ($need / 1GB), ($FreeBytes / 1GB))
        }
        # never delete the games when that still wouldn't make enough room
        $games = Get-NzeusGamesSize $Root
        if ($FreeBytes + $games -lt $need) {
            throw ("Not enough free space even without the games: NZEUS needs {0:N1} GB, the card would have {1:N1} GB free. Use a bigger SD card. Nothing was changed." -f ($need / 1GB), (($FreeBytes + $games) / 1GB))
        }
        Save-StockBackup $Root $Release              # the stock boot files are safe before anything is deleted
        $freed = Remove-NzeusGames $Root
        $FreeBytes = if ($measured) { Get-FreeBytes $Root } else { $FreeBytes + $freed }
        if ($FreeBytes -lt $need) {
            throw ("Not enough free space even without the games: NZEUS needs {0:N1} GB, the card has {1:N1} GB free. Use a bigger SD card." -f ($need / 1GB), ($FreeBytes / 1GB))
        }
    }

    Save-StockBackup $Root $Release
    # 1. copy + check every file under NAME.part (the card still boots what it booted before)
    $done = 0
    foreach ($f in $files) {
        if ($FailAfterFiles -ge 0 -and $done -ge $FailAfterFiles) { throw "test: stopped after $done files." }
        Copy-ToPart (Join-Path $src $f) (Join-Path $Root $f)
        $done++
    }
    # 2. swap them all in at once
    Commit-Parts $Root $files
    $version = (Get-Content -LiteralPath (Join-Path $src "nzeus\VERSION") -Raw).Trim()
    Write-Log $Root "installed NZEUS Deck $version ($done files, all checked)"
    return $version
}

# Reads every installed file back and compares it with the release. Run on a FRESH mount of the
# card (Windows may otherwise answer from its memory instead of reading the card).
function Test-NzeusFiles([string]$Root, [string]$Release) {
    foreach ($f in @("cubegm\advapi32.dll", "cubegm\ApplicationFrame.dll", "cubegm\xgame-logo.bmp", "nzeus\nzroot.img", "nzeus\VERSION")) {
        $p = Join-Path $Root $f
        if (-not (Test-Path $p) -or (Get-Sha $p) -ne (Get-Sha (Join-Path (Join-Path $Release "card") $f))) {
            throw "$(Split-Path -Leaf $f) did not read back correctly from the card. The card may be failing or fake: try another card."
        }
    }
}

function Uninstall-NzeusFiles([string]$Root, [switch]$DeleteData) {
    $backup = Join-Path $Root "nzeus\backup"
    if (Test-Path (Join-Path $Root $Script:NewCardMarker)) {
        throw "This card was set up by install-new-card.bat, so the console's own system was never on it. Use your original card for that. Nothing was changed."
    }
    if (-not (Test-Path (Join-Path $backup "SHA256SUMS"))) { throw "No NZEUS backup on this card (nzeus\backup), so there is nothing to put back." }
    foreach ($line in (Get-Content -LiteralPath (Join-Path $backup "SHA256SUMS"))) {
        $sha, $n = $line -split "\s+", 2
        if ((Get-Sha (Join-Path $backup $n)) -ne $sha) { throw "the saved $n is damaged. Nothing was changed." }
    }
    $names = @("ApplicationFrame.dll", "advapi32.dll")
    if (Test-Path (Join-Path $backup "xgame-logo.bmp")) { $names += "xgame-logo.bmp" }   # the stock start-up picture
    foreach ($n in $names) { Copy-ToPart (Join-Path $backup $n) (Join-Path $Root "cubegm\$n") }
    Commit-Parts $Root @($names | ForEach-Object { "cubegm\$_" })
    foreach ($f in @("nzeus\VERSION", "nzeus\nzroot.img")) { $p = Join-Path $Root $f; if (Test-Path $p) { Remove-Item -LiteralPath $p -Force } }
    if ($DeleteData -and (Test-Path (Join-Path $Root "nzeus\nzdata.img"))) { Remove-Item -LiteralPath (Join-Path $Root "nzeus\nzdata.img") -Force }
    Write-Log $Root ("uninstalled NZEUS Deck: stock boot files back" + $(if ($DeleteData) { ", data deleted" } else { ", data kept in nzeus\nzdata.img" }))
}

# ---------------- the whole run (install.ps1 / uninstall.ps1 call these) ----------------

# Keeps Windows awake during a long copy (a sleeping PC would cut it off).
try {
    Add-Type -Namespace NzeusNative -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);' -ErrorAction Stop
} catch { }
function Set-StayAwake([bool]$on) {
    try {
        $flags = if ($on) { [uint32]2147483649 } else { [uint32]2147483648 }    # ES_CONTINUOUS (+ ES_SYSTEM_REQUIRED)
        [void][NzeusNative.Power]::SetThreadExecutionState($flags)
    } catch { }
}

# Closes the card if it is open, whatever happened before. Returns "" or a problem to add to the message.
function Close-IfOpen([int]$Number, [string]$DiskImage, [string]$Script) {
    try {
        $size = if ($DiskImage) { (Get-Item $DiskImage).Length } else { [long](Get-Disk -Number $Number -ErrorAction Stop).Size }
        if ((Get-R36sState (Get-CardPath $Number $DiskImage) $size) -eq "open") { Close-R36sCard -Number $Number -DiskImage $DiskImage }
        return ""
    } catch {
        return " The card may have been taken out before it was finished: put it back in the card reader and run $Script again."
    }
}

# Opens the card, installs, reads everything back from a fresh mount, and ALWAYS closes it again.
# Returns the message for the user.
function Invoke-NzeusInstall([int]$Number = -1, [string]$DiskImage, [string]$Release, [string]$Root, [long]$FreeBytes = -1, [int]$FailAfterFiles = -1, [switch]$RemoveGames) {
    Set-StayAwake $true
    try {
        $drive = Open-R36sCard -Number $Number -DiskImage $DiskImage
        if (-not $Root) { $Root = $drive }
        Install-NzeusFiles -Root $Root -Release $Release -FreeBytes $FreeBytes -FailAfterFiles $FailAfterFiles -RemoveGames:$RemoveGames | Out-Null
        if (-not $DiskImage) {                       # fresh mount: the read-back really reads the card
            Close-R36sCard -Number $Number
            $Root = Open-R36sCard -Number $Number
        }
        Test-NzeusFiles -Root $Root -Release $Release
        $msg = "Done. Eject the card, put it in the console and switch it on. NZEUS starts in about 30 seconds. If the screen stays black for 2 minutes, run uninstall.bat."
    } catch {
        $msg = $_.Exception.Message
        if ($msg -notlike "Not enough free space*" -and $msg -notlike "*Nothing was changed*") {
            $msg += " Nothing is broken: put the card back in the card reader (if it was taken out) and run install.bat again."
        }
    } finally {
        $msg += Close-IfOpen $Number $DiskImage "install.bat"
        Set-StayAwake $false
    }
    return $msg
}

function Invoke-NzeusUninstall([int]$Number = -1, [string]$DiskImage, [string]$Root, [switch]$DeleteData) {
    try {
        $drive = Open-R36sCard -Number $Number -DiskImage $DiskImage
        if (-not $Root) { $Root = $drive }
        Uninstall-NzeusFiles -Root $Root -DeleteData:$DeleteData
        $msg = "Done. The console's own system is back. Eject the card and put it in the console."
    } catch {
        $msg = $_.Exception.Message
    } finally {
        $msg += Close-IfOpen $Number $DiskImage "uninstall.bat"
    }
    return $msg
}

# collect-logs.bat: copies NZEUS's own log files off the card into Dest (a new folder) for a bug
# report, and ALWAYS closes the card again. Files are only read; the card is only opened and closed.
function Invoke-NzeusCollectLogs([int]$Number = -1, [string]$DiskImage, [string]$Root, [string]$Dest) {
    try {
        $drive = Open-R36sCard -Number $Number -DiskImage $DiskImage
        if (-not $Root) { $Root = $drive }
        $names = @("boot-log.txt", "boot-log-previous.txt", "install-log.txt", "VERSION")
        $found = @($names | Where-Object { Test-Path -LiteralPath (Join-Path $Root "nzeus\$_") })
        if ($found.Count -eq 0) {
            $msg = "No NZEUS logs on this card (nzeus\boot-log.txt is written each time NZEUS starts). Nothing was copied."
        } else {
            New-Item -ItemType Directory -Force -Path $Dest -ErrorAction Stop | Out-Null
            foreach ($n in $found) { Copy-Item -LiteralPath (Join-Path $Root "nzeus\$n") -Destination (Join-Path $Dest $n) -ErrorAction Stop }
            $msg = "Copied $($found.Count) log files to $Dest - attach them to your bug report."
        }
    } catch {
        $msg = "Could not collect the logs: $($_.Exception.Message) Put the card back in the card reader (if it was taken out) and run collect-logs.bat again."
    } finally {
        $msg += Close-IfOpen $Number $DiskImage "collect-logs.bat"
    }
    return $msg
}

Export-ModuleMember -Function Switch-R36sScramble, Get-R36sCards, Open-R36sCard, Close-R36sCard, Install-NzeusFiles,
    Test-NzeusFiles, Uninstall-NzeusFiles, Invoke-NzeusInstall, Invoke-NzeusUninstall, Remove-NzeusGames, Set-NzeusTestHook, Invoke-NzeusCollectLogs,
    Get-NewCardLayout, Get-NewCardDisks, Test-BoardFile, Invoke-NzeusSaveBoardFile, Invoke-NzeusNewCard
