[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutputPath
)

$ErrorActionPreference = "Stop"
$usbDisks = @(Get-Disk | Where-Object { $_.BusType -eq "USB" -or $_.FriendlyName -match "SanDisk|Ventoy" })
$diskNumbers = @($usbDisks | Select-Object -ExpandProperty Number)
$partitions = @(
    foreach ($number in $diskNumbers) {
        Get-Partition -DiskNumber $number
    }
)
$volumePaths = @($partitions.AccessPaths | Where-Object { $_ -like "\\?\Volume{*" })
$volumes = @(
    Get-Volume | Where-Object {
        $_.Path -in $volumePaths -or $_.FileSystemLabel -in @("Ventoy", "VTOYEFI")
    }
)
$pnpDevices = @(
    Get-PnpDevice | Where-Object {
        $_.InstanceId -like "USBSTOR\*" -or
        $_.InstanceId -like "USB\VID_0781&PID_5567\*" -or
        $_.FriendlyName -match "SanDisk|Ventoy|VTOYEFI"
    }
)

[pscustomobject]@{
    CapturedAt = [DateTimeOffset]::Now
    Disks = $usbDisks | Select-Object Number, FriendlyName, SerialNumber, BusType, OperationalStatus, IsOffline, IsReadOnly, PartitionStyle, Size
    Partitions = $partitions | Select-Object DiskNumber, PartitionNumber, DriveLetter, Type, IsActive, IsHidden, Size, AccessPaths
    Volumes = $volumes | Select-Object DriveLetter, FileSystemLabel, FileSystemType, Path, HealthStatus, OperationalStatus
    PnpDevices = $pnpDevices | Select-Object Status, Class, FriendlyName, InstanceId, Problem, ProblemDescription
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
