[CmdletBinding()]
param(
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [string] $VhdPath = "C:\Hyper-V\Ruisi-USB-DLP-XP-SP3\Virtual Hard Disks\Ruisi-USB-DLP-XP-SP3.vhdx",
    [string] $OutputDirectory = "",
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
$resultsRoot = [IO.Path]::GetFullPath((Join-Path $workspace "artifacts\hyperv-xp"))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $resultsRoot ("results-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $resolvedOutput.StartsWith($resultsRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to write results outside the workspace results directory: $resolvedOutput"
}
if (-not (Test-Path -LiteralPath $resultsRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $resultsRoot -Force | Out-Null
}
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "The result directory already exists: $resolvedOutput"
}

$resolvedVhd = [IO.Path]::GetFullPath($VhdPath)
$expectedVhdRoot = [IO.Path]::GetFullPath("C:\Hyper-V\Ruisi-USB-DLP-XP-SP3")
if (-not $resolvedVhd.StartsWith($expectedVhdRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to mount a VHD outside the expected XP VM directory: $resolvedVhd"
}
if (-not (Test-Path -LiteralPath $resolvedVhd -PathType Leaf)) {
    throw "XP VHD not found: $resolvedVhd"
}
if ((Get-VM -Name $VmName).State -ne "Off") {
    throw "The XP VM must be off before collecting results."
}

New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
$mounted = $false
try {
    $disk = Mount-VHD -Path $resolvedVhd -ReadOnly -Passthru
    $mounted = $true
    $volume = $disk | Get-Partition | Get-Volume | Where-Object DriveLetter |
        Sort-Object Size -Descending | Select-Object -First 1
    if (-not $volume) { throw "The mounted XP volume did not receive a drive letter." }
    $guestRoot = "$($volume.DriveLetter):\"
    if (-not (Test-Path -LiteralPath (Join-Path $guestRoot "WINDOWS\system32\config\SYSTEM"))) {
        throw "The mounted volume is not the expected XP system volume."
    }

    $copyMap = @{
        "RuisiUsbDlpLab\xp-lab-result.txt" = "xp-lab-result.txt"
        "RuisiUsbDlpLab\xp-lab-test.log" = "xp-lab-test.log"
        "Documents and Settings\All Users\Application Data\UsbDlp\legacy-agent.log" = "legacy-agent.log"
    }
    foreach ($relativePath in $copyMap.Keys) {
        $source = Join-Path $guestRoot $relativePath
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $resolvedOutput $copyMap[$relativePath])
        }
    }

    $inventoryRoots = @(
        (Join-Path $guestRoot "Program Files\Ruisi USB DLP Legacy"),
        (Join-Path $guestRoot "Program Files\Ruisi USB DLP Legacy Server"),
        (Join-Path $guestRoot "Documents and Settings\All Users\Application Data\UsbDlp"),
        (Join-Path $guestRoot "RuisiUsbDlpLab")
    )
    $inventory = foreach ($root in $inventoryRoots) {
        if (Test-Path -LiteralPath $root) {
            Get-ChildItem -LiteralPath $root -Recurse -Force | Select-Object @{Name="Path";Expression={
                $_.FullName.Substring($guestRoot.Length)
            }}, Length, LastWriteTime, Attributes
        } else {
            [PSCustomObject]@{ Path = $root.Substring($guestRoot.Length) + " [missing]"; Length = $null; LastWriteTime = $null; Attributes = $null }
        }
    }
    $inventory | Format-Table -AutoSize | Out-String -Width 240 |
        Set-Content -LiteralPath (Join-Path $resolvedOutput "guest-inventory.txt") -Encoding UTF8

    $lines = @(
        "VmName=$VmName",
        "VhdPath=$resolvedVhd",
        "GuestVolume=$guestRoot",
        "OutputDirectory=$resolvedOutput",
        "ResultPresent=$([bool](Test-Path -LiteralPath (Join-Path $resolvedOutput 'xp-lab-result.txt')))"
        "LogPresent=$([bool](Test-Path -LiteralPath (Join-Path $resolvedOutput 'xp-lab-test.log')))"
        "Status=collected"
    )
    $lines | ForEach-Object { Write-Output $_ }
    if ($ReportPath) { $lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
} finally {
    if ($mounted) { Dismount-VHD -Path $resolvedVhd }
}
