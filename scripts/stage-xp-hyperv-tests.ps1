[CmdletBinding()]
param(
    [string] $VhdPath = "C:\Hyper-V\Ruisi-USB-DLP-XP-SP3\Virtual Hard Disks\Ruisi-USB-DLP-XP-SP3.vhdx",
    [string] $KitPath = "",
    [string] $BootstrapPath = "",
    [string] $LauncherPath = "",
    [string] $PolicyTestPath = "",
    [string] $StoreTestPath = "",
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

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $KitPath) {
    $KitPath = Join-Path $workspace "artifacts\legacy-kits\RuisiUsbDlp-legacy-xp-x86-1.1.0"
}
if (-not $BootstrapPath) {
    $BootstrapPath = Join-Path $workspace "legacy\packaging\Run-Xp-Lab-Test.cmd"
}
if (-not $LauncherPath) {
    $LauncherPath = Join-Path $workspace "legacy\packaging\Launch-Xp-Lab-Test-Once.cmd"
}
if (-not $PolicyTestPath) {
    $PolicyTestPath = Join-Path $workspace "legacy\build-xp-x86\Release\usb_dlp_legacy_policy_tests.exe"
}
if (-not $StoreTestPath) {
    $StoreTestPath = Join-Path $workspace "legacy\build-xp-x86\Release\usb_dlp_legacy_store_tests.exe"
}

$resolvedVhd = [IO.Path]::GetFullPath($VhdPath)
$expectedRoot = [IO.Path]::GetFullPath("C:\Hyper-V\Ruisi-USB-DLP-XP-SP3")
if (-not $resolvedVhd.StartsWith($expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to mount a VHD outside the expected XP VM directory: $resolvedVhd"
}
foreach ($source in @($resolvedVhd, $KitPath, $BootstrapPath, $LauncherPath, $PolicyTestPath, $StoreTestPath)) {
    if (-not (Test-Path -LiteralPath $source)) { throw "Required staging source not found: $source" }
}
if ((Get-VM -Name "Ruisi-USB-DLP-XP-SP3").State -ne "Off") {
    throw "The XP VM must be off before staging tests."
}

$mounted = $false
try {
    $disk = Mount-VHD -Path $resolvedVhd -Passthru
    $mounted = $true
    $volume = $disk | Get-Partition | Get-Volume | Where-Object DriveLetter | Sort-Object Size -Descending | Select-Object -First 1
    if (-not $volume) { throw "The mounted XP volume did not receive a drive letter." }
    $guestRoot = "$($volume.DriveLetter):\"
    if (-not (Test-Path -LiteralPath (Join-Path $guestRoot "WINDOWS\system32\config\SYSTEM"))) {
        throw "The mounted volume is not the expected XP system volume."
    }

    $destination = Join-Path $guestRoot "RuisiUsbDlpLab"
    $resolvedDestination = [IO.Path]::GetFullPath($destination)
    if (-not $resolvedDestination.StartsWith([IO.Path]::GetFullPath($guestRoot), [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedDestination -Leaf) -ne "RuisiUsbDlpLab") {
        throw "Refusing to replace an unexpected guest path: $resolvedDestination"
    }
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $resolvedDestination -Recurse -Force
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -Path (Join-Path $KitPath "*") -Destination $destination -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $destination "tests") -Force | Out-Null
    Copy-Item -LiteralPath $PolicyTestPath -Destination (Join-Path $destination "tests") -Force
    Copy-Item -LiteralPath $StoreTestPath -Destination (Join-Path $destination "tests") -Force
    Copy-Item -LiteralPath $BootstrapPath -Destination (Join-Path $destination "run-lab-install.cmd") -Force

    @(
        "[Deployment]",
        "ServerUrl=http://127.0.0.1:5080",
        "Port=5080",
        "AdminUsername=admin",
        "Profile=legacy-xp-x86"
    ) | Set-Content -LiteralPath (Join-Path $destination "config\deployment.ini") -Encoding Ascii

    $startup = Join-Path $guestRoot "Documents and Settings\All Users\Start Menu\Programs\Startup"
    if (-not (Test-Path -LiteralPath $startup -PathType Container)) {
        throw "The All Users Startup folder was not found in the XP guest."
    }
    Copy-Item -LiteralPath $LauncherPath -Destination (Join-Path $startup "RuisiUsbDlpLabOnce.cmd") -Force

    $lines = @(
        "VhdPath=$resolvedVhd",
        "GuestVolume=$guestRoot",
        "GuestStage=C:\RuisiUsbDlpLab",
        "GuestAddress=192.168.77.10",
        "StartupLauncher=RuisiUsbDlpLabOnce.cmd",
        "Status=staged"
    )
    $lines | ForEach-Object { Write-Output $_ }
    if ($ReportPath) { $lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
} finally {
    if ($mounted) { Dismount-VHD -Path $resolvedVhd }
}
