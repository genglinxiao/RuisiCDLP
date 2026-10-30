[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This helper must run as Administrator.'
}

$logDirectory = Split-Path -Parent $LogPath
if (-not (Test-Path -LiteralPath $logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
}

Start-Transcript -LiteralPath $LogPath -Force | Out-Null
try {
    foreach ($serviceName in @('VBoxSDS', 'VBoxUSB', 'VBoxUSBMon')) {
        Write-Output "Stopping $serviceName"
        & sc.exe stop $serviceName
    }

    Start-Sleep -Seconds 3

    foreach ($serviceName in @('VBoxUSBMon', 'VBoxUSB', 'VBoxSDS')) {
        Write-Output "Starting $serviceName"
        & sc.exe start $serviceName
    }

    Start-Sleep -Seconds 3
    Get-CimInstance Win32_SystemDriver |
        Where-Object { $_.Name -in @('VBoxUSB', 'VBoxUSBMon') } |
        Select-Object Name, State, StartMode, PathName |
        Format-List
    Get-Service -Name VBoxSDS | Select-Object Name, Status, StartType | Format-List
} finally {
    Stop-Transcript | Out-Null
}
