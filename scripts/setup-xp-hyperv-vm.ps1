[CmdletBinding()]
param(
    [string] $IsoPath = "C:\Users\Linxiao Geng\Downloads\en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso",
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [string] $VmRoot = "C:\Hyper-V\Ruisi-USB-DLP-XP-SP3",
    [string] $SwitchName = "Ruisi-USB-DLP-Isolated",
    [string] $ReportPath = ""
)

$ErrorActionPreference = "Stop"

function Write-Report {
    param([string[]] $Lines)
    $Lines | ForEach-Object { Write-Output $_ }
    if ($ReportPath) {
        $Lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8
    }
}

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell process."
}
if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) {
    throw "Windows XP ISO not found: $IsoPath"
}
if ((Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All).State -ne "Enabled") {
    throw "Hyper-V is not enabled on this host."
}
if (Get-VM -Name $VmName -ErrorAction SilentlyContinue) {
    throw "A VM named '$VmName' already exists; no existing VM was changed."
}

$switch = Get-VMSwitch -Name $SwitchName -ErrorAction SilentlyContinue
if (-not $switch) {
    $switch = New-VMSwitch -Name $SwitchName -SwitchType Internal
}
if ($switch.SwitchType -ne "Internal" -and $switch.SwitchType -ne "Private") {
    throw "The existing '$SwitchName' switch is not isolated."
}

if ($switch.SwitchType -eq "Internal") {
    $adapter = Get-NetAdapter -Name "vEthernet ($SwitchName)" -ErrorAction SilentlyContinue
    if ($adapter -and -not (Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object IPAddress -eq "192.168.77.1")) {
        New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress "192.168.77.1" -PrefixLength 24 | Out-Null
    }
}

$vhdDirectory = Join-Path $VmRoot "Virtual Hard Disks"
New-Item -ItemType Directory -Path $vhdDirectory -Force | Out-Null
$vhdPath = Join-Path $vhdDirectory "$VmName.vhdx"

$vm = New-VM -Name $VmName -Generation 1 -MemoryStartupBytes 1GB -Path $VmRoot `
    -NewVHDPath $vhdPath -NewVHDSizeBytes 20GB
Set-VM -VM $vm -ProcessorCount 1 -DynamicMemory:$false -AutomaticCheckpointsEnabled:$false `
    -AutomaticStartAction Nothing -AutomaticStopAction Save
Set-VMProcessor -VMName $VmName -CompatibilityForOlderOperatingSystemsEnabled $true

Get-VMNetworkAdapter -VMName $VmName | Remove-VMNetworkAdapter
Add-VMNetworkAdapter -VMName $VmName -Name "XP Legacy Ethernet" -IsLegacy $true -SwitchName $SwitchName

$dvd = Get-VMDvdDrive -VMName $VmName | Select-Object -First 1
if ($dvd) {
    Set-VMDvdDrive -VMDvdDrive $dvd -Path $IsoPath
} else {
    Add-VMDvdDrive -VMName $VmName -ControllerNumber 1 -ControllerLocation 0 -Path $IsoPath
}
Set-VMBios -VMName $VmName -StartupOrder @("CD", "IDE", "LegacyNetworkAdapter", "Floppy")

$created = Get-VM -Name $VmName
Write-Report @(
    "VmName=$($created.Name)",
    "State=$($created.State)",
    "Generation=$($created.Generation)",
    "ProcessorCount=$($created.ProcessorCount)",
    "MemoryMB=$([int]($created.MemoryStartup / 1MB))",
    "VhdPath=$vhdPath",
    "IsoPath=$IsoPath",
    "SwitchName=$SwitchName",
    "SwitchType=$($switch.SwitchType)",
    "HostLabAddress=192.168.77.1/24"
)
