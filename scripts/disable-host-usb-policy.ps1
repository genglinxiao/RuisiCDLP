[CmdletBinding()]
param(
    [string] $LogPath = (Join-Path $PSScriptRoot "..\artifacts\disable-host-usb-policy.log")
)

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"
$restrictionsPath = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions"
$allowIdsPath = Join-Path $restrictionsPath "AllowDeviceIDs"
$ownershipPath = "HKLM:\SOFTWARE\UsbDlp\RegistryOwnership"
$targetUsbId = "USB\VID_1058&PID_261B\MSFT30313933384151343030353431"

$logDirectory = Split-Path -Parent $LogPath
if ($logDirectory) {
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
}

Start-Transcript -Path $LogPath -Force | Out-Null
try {
    Write-Output "Disabling Ruisi USB enforcement on $env:COMPUTERNAME"

    $ruisiServices = Get-CimInstance Win32_Service | Where-Object {
        $_.Name -match "UsbDlp|Ruisi" -or
        $_.DisplayName -match "USB DLP|Ruisi" -or
        $_.PathName -match "UsbDlp|Ruisi USB DLP"
    }
    foreach ($service in $ruisiServices) {
        Write-Output "Stopping and disabling service $($service.Name)"
        Stop-Service -Name $service.Name -Force -ErrorAction SilentlyContinue
        Set-Service -Name $service.Name -StartupType Disabled
    }

    if (Test-Path -LiteralPath $ownershipPath) {
        $ownership = Get-ItemProperty -LiteralPath $ownershipPath
        foreach ($name in @($ownership.Scalars)) {
            if ($name) {
                Write-Output "Removing Ruisi-owned policy value $name"
                Remove-ItemProperty -LiteralPath $restrictionsPath -Name $name -ErrorAction SilentlyContinue
            }
        }
        foreach ($name in @($ownership.AllowValues)) {
            if ($name) {
                Write-Output "Removing Ruisi-owned allow-list value $name"
                Remove-ItemProperty -LiteralPath $allowIdsPath -Name $name -ErrorAction SilentlyContinue
            }
        }
        Remove-Item -LiteralPath $ownershipPath -Recurse -Force
    } else {
        Write-Output "No Ruisi registry ownership ledger was found."
    }

    & mountvol.exe /E

    $deviceOutput = & pnputil.exe /enum-devices /instanceid $targetUsbId 2>&1
    if ($deviceOutput -match "CM_PROB_NOT_CONFIGURED|Problem Code:\s+1") {
        Write-Output "Removing the stale My Passport device node before rescan."
        & pnputil.exe /remove-device $targetUsbId
    }

    & pnputil.exe /scan-devices
    Start-Sleep -Seconds 8

    Write-Output "--- Remaining device-install restrictions ---"
    if (Test-Path -LiteralPath $restrictionsPath) {
        Get-ItemProperty -LiteralPath $restrictionsPath | Format-List
    } else {
        Write-Output "None"
    }
    Write-Output "--- Connected disk devices ---"
    & pnputil.exe /enum-devices /connected /class DiskDrive
    Write-Output "--- Windows disks ---"
    Get-Disk | Format-Table Number, FriendlyName, BusType, OperationalStatus, IsOffline, IsReadOnly, Size -AutoSize
    Write-Output "USB policy recovery completed."
} finally {
    Stop-Transcript | Out-Null
}
