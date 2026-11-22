# collect-logs.ps1 - copies NZEUS's log files off the SD card for a bug report (started by collect-logs.bat).
# Opens the card, copies nzeus/boot-log.txt, boot-log-previous.txt, install-log.txt and VERSION into
# a new folder next to this release, and closes the card again. Nothing on the card is changed.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Opening the card (its first sector) needs administrator rights: ask Windows once.
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
$dest = Join-Path $release ("nzeus-logs-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
Write-Host ""
Write-Host ("Copying NZEUS's logs from: {0}, {1:N1} GB" -f $card.FriendlyName, ($card.SizeBytes / 1GB))
Finish (Invoke-NzeusCollectLogs -Number $card.Number -Dest $dest)
} catch {
    Finish "Something went wrong: $($_.Exception.Message) Put the card back in the card reader (if it was taken out) and run collect-logs.bat again."
}
