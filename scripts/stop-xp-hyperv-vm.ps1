[CmdletBinding()]
param(
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [string] $ReportPath = ""
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell process."
}

$vm = Get-VM -Name $VmName
if ($vm.State -eq "Running") {
    Stop-VM -VM $vm -Shutdown -Force
}
$vm = Get-VM -Name $VmName
$lines = @("VmName=$($vm.Name)", "State=$($vm.State)", "Status=$($vm.Status)")
$lines | ForEach-Object { Write-Output $_ }
if ($ReportPath) {
    $lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}
