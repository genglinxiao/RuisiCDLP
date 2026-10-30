[CmdletBinding()]
param(
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [string] $SwitchName = "Ruisi-USB-DLP-Isolated",
    [string] $HostAddress = "192.168.77.1",
    [int] $PrefixLength = 24,
    [string] $ReportPath = ""
)

$ErrorActionPreference = "Stop"
trap {
    $message = ($_ | Out-String)
    if ($ReportPath) {
        @("Status=error", $message) | Set-Content -LiteralPath $ReportPath -Encoding UTF8
    }
    Write-Error $message
    exit 1
}

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell process."
}

$switch = Get-VMSwitch -Name $SwitchName -ErrorAction Stop
if ($switch.SwitchType -ne "Internal") {
    throw "The XP lab switch must be Internal, but '$SwitchName' is $($switch.SwitchType)."
}
$adapter = Get-NetAdapter -Name "vEthernet ($SwitchName)" -ErrorAction Stop
$hostIp = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object IPAddress -eq $HostAddress | Select-Object -First 1
if (-not $hostIp) {
    $hostIp = New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $HostAddress `
        -PrefixLength $PrefixLength
}

$vmAdapter = Get-VMNetworkAdapter -VMName $VmName -ErrorAction Stop | Select-Object -First 1
if (-not $vmAdapter) { throw "The XP VM has no network adapter." }
if ($vmAdapter.SwitchName -ne $SwitchName) {
    Connect-VMNetworkAdapter -VMNetworkAdapter $vmAdapter -SwitchName $SwitchName
    $vmAdapter = Get-VMNetworkAdapter -VMName $VmName | Select-Object -First 1
}

$lines = @(
    "VmName=$VmName",
    "VmState=$((Get-VM -Name $VmName).State)",
    "VmAdapter=$($vmAdapter.Name)",
    "VmSwitch=$($vmAdapter.SwitchName)",
    "SwitchName=$SwitchName",
    "SwitchType=$($switch.SwitchType)",
    "HostAdapter=$($adapter.Name)",
    "HostAddress=$($hostIp.IPAddress)/$($hostIp.PrefixLength)",
    "Status=configured"
)
$lines | ForEach-Object { Write-Output $_ }
if ($ReportPath) { $lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
