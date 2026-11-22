# new-card.ps1 - puts NZEUS Deck on a NEW, empty SD card (started by install-new-card.bat).
# Step 1 copies the console's board file (cubegm\Bubbles.scr) off the ORIGINAL card, which is
# only read. Step 2 erases the new card, sets it up the way the console's start-up expects and
# copies NZEUS (every file checked). The original card keeps the console's own system.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Writing a card directly needs administrator rights: ask Windows once.
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    exit
}
Import-Module (Join-Path $here "NzeusCard.psm1") -Force
$release = Split-Path -Parent $here
$board = Join-Path $here "your-console\Bubbles.scr"       # saved once, then reused for more cards
function Finish([string]$text) { Write-Host ""; Write-Host $text; Write-Host ""; Read-Host "Press Enter to close" | Out-Null; exit }
function Pick($list, [string]$what) {
    if ($list.Count -eq 1) { return $list[0] }
    Write-Host "More than one $what is plugged in:"
    for ($i = 0; $i -lt $list.Count; $i++) { Write-Host ("  {0}. {1}, {2:N1} GB" -f ($i + 1), $list[$i].FriendlyName, ($list[$i].SizeBytes / 1GB)) }
    $n = Read-Host "Type the number of the one to use"
    if (-not ($n -match '^[0-9]+$') -or [int]$n -lt 1 -or [int]$n -gt $list.Count) { Finish "Nothing chosen. Nothing was changed." }
    return $list[[int]$n - 1]
}
try {

$version = (Get-Content (Join-Path $release "card/nzeus/VERSION") -Raw).Trim()
Write-Host "NZEUS Deck $version on a NEW SD card. Your console's original card is only read, never changed."
Write-Host ""

# Step 1: the console's board file, from the original card
if (Test-BoardFile $board) {
    Write-Host "Step 1 of 2: put your console's ORIGINAL SD card (the one it came with) in the card reader."
    Read-Host "Press Enter when it's in" | Out-Null
    $cards = @(Get-R36sCards)
    if ($cards.Count -eq 0) { Finish "No R36S card found. Put the console's original card in the card reader (or take it out and put it back) and run me again." }
    $card = Pick $cards "R36S card"
    $msg = Invoke-NzeusSaveBoardFile -Number $card.Number -Dest $board
    Write-Host $msg
    if (Test-BoardFile $board) { Finish "Nothing was changed on any card." }
    Write-Host ""
    Write-Host "Now take the original card OUT of the card reader and keep it safe."
} else {
    Write-Host "Step 1 of 2: done before (the console's board file is saved in installer\your-console)."
}

# Step 2: the new card
Write-Host ""
Write-Host "Step 2 of 2: put the NEW SD card in the card reader (8 GB or bigger)."
Read-Host "Press Enter when it's in" | Out-Null
$disks = @(Get-NewCardDisks | Where-Object { $_.SizeBytes -ge 7GB })
if ($disks.Count -eq 0) { Finish "No SD card of 8 GB or bigger found. Put the new card in the card reader (or take it out and put it back) and run me again." }
$disk = Pick $disks "card"
Write-Host ""
Write-Host ("This card will be ERASED: {0}, {1:N1} GB. Everything on it is deleted." -f $disk.FriendlyName, ($disk.SizeBytes / 1GB))
if ($disk.IsR36s) {
    Write-Host "WARNING: this card already has the console's system or NZEUS on it. If it is your ORIGINAL card, stop now (just press Enter)."
}
$ok = Read-Host "Type ERASE and press Enter to continue"
if ($ok -cne "ERASE") { Finish "Nothing was changed." }
Write-Host "Setting up the card and copying NZEUS (about 4.5 GB). This takes a few minutes - don't take the card out..."
Finish (Invoke-NzeusNewCard -Number $disk.Number -Release $release -BoardFile $board)
} catch {
    Finish "Something went wrong: $($_.Exception.Message) Put the card back in the card reader (if it was taken out) and run install-new-card.bat again."
}
