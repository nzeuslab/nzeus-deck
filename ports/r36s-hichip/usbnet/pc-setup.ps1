<#
pc-setup.ps1 - PC side of the console's USB network (ports/r36s-hichip/usbnet).

Run it AFTER usbnet-up.sh switched the console to "serial + network" and
usbnet-confirm.sh was accepted:
    powershell -ExecutionPolicy Bypass -File ports\r36s-hichip\usbnet\pc-setup.ps1

What it does:
 1. shows your Windows version and whether the built-in NCM driver exists
 2. lists the console's USB devices Windows sees now
 3. finds the console's network card - ONLY the one whose parent USB device is
    exactly NZEUS-RAM-NCM-01 or NZEUS-RAM-RNDIS-01 (never another card)
 4. shows what it WOULD change (fixed address 192.168.77.2/24, no gateway, so
    your normal internet route is not touched), and changes it only after you
    type YES. Changing it needs "Run as administrator".
 5. pings the console (192.168.77.1) and checks whether SSH (port 22) answers.
Windows remembers the address for this card (same fixed MAC every time), so
step 4 is normally needed only once.
Windows PowerShell 5.1 compatible.
#>
param(
    [string]$ConsoleIp = '192.168.77.1',
    [string]$PcIp = '192.168.77.2',
    [int]$PrefixLength = 24
)
$ErrorActionPreference = 'Stop'
$parents = @('USB\VID_1D6B&PID_0104\NZEUS-RAM-NCM-01', 'USB\VID_1D6B&PID_0104\NZEUS-RAM-RNDIS-01')

function Get-Parent([string]$instanceId) {
    try {
        return [string](Get-PnpDeviceProperty -InstanceId $instanceId -KeyName DEVPKEY_Device_Parent -ErrorAction Stop).Data
    } catch {
        return ''
    }
}

Write-Host '== 1. Windows and its built-in USB network drivers'
$os = Get-CimInstance Win32_OperatingSystem
Write-Host ("Windows: {0} (build {1})" -f $os.Caption, $os.BuildNumber)
$ncmDriver = Join-Path $env:SystemRoot 'System32\drivers\UsbNcm.sys'
if (Test-Path $ncmDriver) {
    Write-Host 'UsbNcm.sys (built-in NCM driver): present'
} else {
    Write-Host 'UsbNcm.sys (built-in NCM driver): NOT present on this Windows.'
    Write-Host '  -> use RNDIS mode on the console:  sh usbnet-up.sh rndis'
}

Write-Host ''
Write-Host '== 2. Console USB devices Windows sees now (VID 1D6B, PID 0104)'
$usb = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_1D6B&PID_0104*' })
if ($usb.Count -eq 0) {
    Write-Host 'None. Is the console on, the cable a DATA cable, and usbnet-up.sh started?'
    exit 1
}
$usb | Sort-Object InstanceId | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-String -Width 200 | Write-Host

Write-Host '== 3. The console network card'
$cards = @()
foreach ($adapter in @(Get-NetAdapter -ErrorAction SilentlyContinue)) {
    if (-not $adapter.PnPDeviceID) { continue }
    $parent = (Get-Parent $adapter.PnPDeviceID).ToUpper()
    if ($parents -contains $parent) { $cards += $adapter }
}
if ($cards.Count -ne 1) {
    Write-Host ("Found {0} console network cards (need exactly 1)." -f $cards.Count)
    Write-Host 'Check: usbnet-up.sh --status on the console; Device Manager for a yellow mark'
    Write-Host 'under "Network adapters" or "Other devices" (a missing driver).'
    exit 1
}
$card = $cards[0]
Write-Host ("Card: '{0}' = {1}" -f $card.Name, $card.InterfaceDescription)
Write-Host ("Status: {0}   MAC: {1}   Interface index: {2}" -f $card.Status, $card.MacAddress, $card.ifIndex)
$current = @(Get-NetIPAddress -InterfaceIndex $card.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue)
foreach ($a in $current) { Write-Host ("Now has: {0}/{1} ({2})" -f $a.IPAddress, $a.PrefixLength, $a.PrefixOrigin) }

Write-Host ''
Write-Host '== 4. Address for this card'
$already = @($current | Where-Object { $_.IPAddress -eq $PcIp -and $_.PrefixLength -eq $PrefixLength })
if ($already.Count -eq 1 -and $current.Count -eq 1) {
    Write-Host ("Already {0}/{1}: nothing to change." -f $PcIp, $PrefixLength)
} else {
    Write-Host ("I WOULD change ONLY the card '{0}' (index {1}):" -f $card.Name, $card.ifIndex)
    Write-Host '  a) switch off DHCP for IPv4 on this card'
    foreach ($a in $current) { Write-Host ("  b) remove its address {0}/{1}" -f $a.IPAddress, $a.PrefixLength) }
    Write-Host ("  c) add {0}/{1} with NO gateway (your internet route stays as it is)" -f $PcIp, $PrefixLength)
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host 'Not changed: this needs PowerShell "Run as administrator". Start it that way and run this again.'
    } else {
        $answer = Read-Host 'Type YES to make this change (anything else = no change)'
        if ($answer -ceq 'YES') {
            Set-NetIPInterface -InterfaceIndex $card.ifIndex -AddressFamily IPv4 -Dhcp Disabled
            foreach ($a in $current) {
                Remove-NetIPAddress -InterfaceIndex $card.ifIndex -IPAddress $a.IPAddress -Confirm:$false
            }
            New-NetIPAddress -InterfaceIndex $card.ifIndex -IPAddress $PcIp -PrefixLength $PrefixLength | Out-Null
            Write-Host ("Done: '{0}' = {1}/{2}" -f $card.Name, $PcIp, $PrefixLength)
            Start-Sleep -Seconds 3
        } else {
            Write-Host 'Nothing changed.'
        }
    }
}

Write-Host ''
Write-Host ("== 5. Can the PC reach the console ({0})?" -f $ConsoleIp)
if (Test-Connection -ComputerName $ConsoleIp -Count 3 -Quiet) {
    Write-Host 'PING OK: the USB network works.'
} else {
    Write-Host 'No ping answer. Check: this card has the address above; on the console:'
    Write-Host '  sh /tmp/nzeus-usbnet/usbnet-up.sh --status'
    exit 1
}
$ssh = Test-NetConnection -ComputerName $ConsoleIp -Port 22 -WarningAction SilentlyContinue
if ($ssh.TcpTestSucceeded) {
    Write-Host 'SSH port 22 answers (dropbear is running). Log in with:'
    Write-Host ("  ssh -i <your private key> -o UserKnownHostsFile=<a known_hosts file just for the console> root@{0}" -f $ConsoleIp)
} else {
    Write-Host 'SSH port 22 does not answer yet: start it with ports\r36s-hichip\usbnet\dropbear\dropbear-start.sh (see README).'
}
exit 0
