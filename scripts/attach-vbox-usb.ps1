[CmdletBinding()]
param(
    [string] $VmName = "WINXP",
    [string] $VendorId = "0781",
    [string] $ProductId = "5567",
    [string] $SerialNumber = "4C531001440929115253",
    [string[]] $DriveLetters = @("D:", "E:"),
    [string] $MountMapPath = "",
    [string] $ReportPath = ""
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell process."
}

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $workspace "artifacts\vbox-xp"))
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
if (-not $MountMapPath) { $MountMapPath = Join-Path $artifactRoot "host-usb-mounts.txt" }
if (-not $ReportPath) { $ReportPath = Join-Path $artifactRoot "usb-attach-report.txt" }
foreach ($path in @($MountMapPath, $ReportPath)) {
    $resolved = [IO.Path]::GetFullPath($path)
    if (-not $resolved.StartsWith($artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside the VirtualBox XP artifact directory: $resolved"
    }
}

$vbox = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
if (-not (Test-Path -LiteralPath $vbox -PathType Leaf)) { throw "VBoxManage was not found." }
if ((& $vbox showvminfo $VmName --machinereadable | Select-String '^VMState="running"$').Count -ne 1) {
    throw "VirtualBox VM '$VmName' is not running."
}

$mounts = @{}
foreach ($drive in $DriveLetters) {
    $mount = ((& mountvol.exe $drive /L) | Out-String).Trim()
    if (-not $mount) { throw "Could not resolve the volume mounted at $drive" }
    $mounts[$drive] = $mount
}
$mounts.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" } |
    Set-Content -LiteralPath $MountMapPath -Encoding Ascii

$attached = $false
try {
    foreach ($drive in $DriveLetters) {
        & mountvol.exe $drive /P
        if ($LASTEXITCODE -ne 0) { throw "Could not safely dismount $drive" }
    }

    $blocks = ((& $vbox list usbhost) -join "`n") -split "(?:`r?`n){2,}"
    $device = $blocks | Where-Object {
        $_ -match "VendorId:\s+0x$VendorId" -and
        $_ -match "ProductId:\s+0x$ProductId" -and
        $_ -match "SerialNumber:\s+$([regex]::Escape($SerialNumber))"
    } | Select-Object -First 1
    if (-not $device) { throw "The requested USB device is not available to VirtualBox." }
    $uuid = [regex]::Match($device, "UUID:\s+([0-9a-f-]+)").Groups[1].Value
    if (-not $uuid) { throw "Could not parse the VirtualBox USB UUID." }

    & $vbox controlvm $VmName usbattach $uuid
    if ($LASTEXITCODE -ne 0) { throw "VirtualBox could not attach the USB device." }
    $attached = $true
    @(
        "VmName=$VmName",
        "UsbUuid=$uuid",
        "VendorId=$VendorId",
        "ProductId=$ProductId",
        "SerialNumber=$SerialNumber",
        "HostVolumes=$($DriveLetters -join ',')",
        "Status=attached"
    ) | Set-Content -LiteralPath $ReportPath -Encoding UTF8
} catch {
    if (-not $attached) {
        foreach ($drive in $DriveLetters) {
            if ($mounts[$drive]) { & mountvol.exe $drive $mounts[$drive] | Out-Null }
        }
    }
    throw
}
