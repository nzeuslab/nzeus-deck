# uninstall.ps1 - puts the console's own system back (started by uninstall.bat).
# Opens the card, restores the stock boot files (and power-on picture) from nzeus/backup and closes
# the card again. Your NZEUS files (nzeus/nzdata.img) are kept unless you say otherwise.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Writing the card's first sector needs administrator rights: ask Windows once.
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    exit
}
Import-Module (Join-Path $here "NzeusCard.psm1") -Force
# the release folder = the one holding card\ (the scripts sit in more\installer or installer)
$release = Split-Path -Parent $here
if (-not (Test-Path (Join-Path $release "card"))) { $release = Split-Path -Parent $release }
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
Write-Host ""
Write-Host ("The console's own system will come back on: {0}, {1:N1} GB" -f $card.FriendlyName, ($card.SizeBytes / 1GB))
$ok = Read-Host "Type Y and press Enter to start"
if ($ok -ne "Y" -and $ok -ne "y") { Finish "Nothing was changed." }
$del = Read-Host "Also delete your NZEUS files and settings (nzeus/nzdata.img)? Type DELETE to delete them, or just press Enter to keep them"
Finish (Invoke-NzeusUninstall -Number $card.Number -DeleteData:($del -eq "DELETE"))
} catch {
    Finish "Something went wrong: $($_.Exception.Message) Put the card back in the card reader (if it was taken out) and run uninstall.bat again."
}
