[CmdletBinding()]
param(
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [switch] $OpenConsole,
    [string] $ReportPath = ""
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell process."
}

$vm = Get-VM -Name $VmName
if ($vm.State -eq "Off" -or $vm.State -eq "Saved") {
    Start-VM -VM $vm | Out-Null
}
$vm = Get-VM -Name $VmName
$lines = @(
    "VmName=$($vm.Name)",
    "State=$($vm.State)",
    "Uptime=$($vm.Uptime)",
    "Status=$($vm.Status)"
)
$lines | ForEach-Object { Write-Output $_ }
if ($ReportPath) {
    $lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}
if ($OpenConsole) {
    Start-Process -FilePath "$env:SystemRoot\System32\vmconnect.exe" `
        -ArgumentList @("localhost", ('"' + $VmName + '"'))
}
