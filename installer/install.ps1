# install.ps1 - puts NZEUS Deck on an R36S SD card (started by install.bat).
# Opens the card, saves its stock boot files and power-on picture to nzeus/backup, copies NZEUS
# (every file checked after copying) and closes the card again.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Writing the card's first sector needs administrator rights: ask Windows once.
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    exit
}
Import-Module (Join-Path $here "NzeusCard.psm1") -Force
$release = Split-Path -Parent $here
function Finish([string]$text) { Write-Host ""; Write-Host $text; Write-Host ""; Read-Host "Press Enter to close" | Out-Null; exit }
try {

# Which card? Only removable disks that really are R36S cards are offered.
$cards = @(Get-R36sCards)
if ($cards.Count -eq 0) {
    Finish "No R36S card found. Put the console's SD card in the card reader (or take it out and put it back) and run me again."
}
if ($cards.Count -eq 1) {
    $card = $cards[0]
} else {
    Write-Host "More than one R36S card is plugged in:"
    for ($i = 0; $i -lt $cards.Count; $i++) { Write-Host ("  {0}. {1}, {2:N1} GB" -f ($i + 1), $cards[$i].FriendlyName, ($cards[$i].SizeBytes / 1GB)) }
    $pick = Read-Host "Type the number of the card to use"
    if (-not ($pick -match '^[0-9]+$') -or [int]$pick -lt 1 -or [int]$pick -gt $cards.Count) { Finish "No card chosen. Nothing was changed." }
    $card = $cards[[int]$pick - 1]
}
$version = (Get-Content (Join-Path $release "card/nzeus/VERSION") -Raw).Trim()
Write-Host ""
Write-Host ("NZEUS Deck {0} will be installed on: {1}, {2:N1} GB" -f $version, $card.FriendlyName, ($card.SizeBytes / 1GB))
Write-Host "Your games stay on the card (if it is too full, you will be asked first). The console's own system can come back with uninstall.bat."
$ok = Read-Host "Type Y and press Enter to start"
if ($ok -ne "Y" -and $ok -ne "y") { Finish "Nothing was changed." }
Write-Host "Copying NZEUS (about 4.5 GB). This takes a few minutes - don't take the card out..."
$msg = Invoke-NzeusInstall -Number $card.Number -Release $release
if ($msg -like "Not enough free space:*") {
    # stock cards come full of games, and Windows can't show a closed card: offer to make room here
    Write-Host ""
    Write-Host $msg
    Write-Host "NZEUS can make room by deleting ALL the games on the card (the game folders stay, empty)."
    Write-Host "The console's own system is kept, and uninstall.bat still brings it back - but the deleted games don't come back."
    $ok = Read-Host "Type Y and press Enter to delete the games and install, or just press Enter to stop"
    if ($ok -ne "Y" -and $ok -ne "y") { Finish "Nothing was changed." }
    Write-Host "Deleting the games, then copying NZEUS - don't take the card out..."
    $msg = Invoke-NzeusInstall -Number $card.Number -Release $release -RemoveGames
}
Finish $msg
} catch {
    Finish "Something went wrong: $($_.Exception.Message) Put the card back in the card reader (if it was taken out) and run install.bat again."
}
