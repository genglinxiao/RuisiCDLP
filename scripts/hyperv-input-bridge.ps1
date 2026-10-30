[CmdletBinding()]
param(
    [string] $VmName = "Ruisi-USB-DLP-XP-SP3",
    [Parameter(Mandatory = $true)][string] $CommandPath,
    [Parameter(Mandatory = $true)][string] $StatusPath,
    [Parameter(Mandatory = $true)][string] $StopPath
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This bridge must run from an elevated PowerShell process."
}

$namespace = "root\virtualization\v2"
$escapedName = $VmName.Replace("'", "''")
$vm = Get-WmiObject -Namespace $namespace -Query `
    "SELECT * FROM Msvm_ComputerSystem WHERE ElementName = '$escapedName'" | Select-Object -First 1
if (-not $vm) { throw "VM not found: $VmName" }
$keyboard = Get-WmiObject -Namespace $namespace -Query `
    "ASSOCIATORS OF {$($vm.__PATH)} WHERE ResultClass = Msvm_Keyboard" | Select-Object -First 1
if (-not $keyboard) { throw "Virtual keyboard not found for $VmName" }

$lastId = ""
@{ state = "ready"; vmName = $VmName; processedId = ""; returnValue = 0 } |
    ConvertTo-Json -Compress | Set-Content -LiteralPath $StatusPath -Encoding UTF8

while (-not (Test-Path -LiteralPath $StopPath)) {
    if (Test-Path -LiteralPath $CommandPath -PathType Leaf) {
        try {
            $command = Get-Content -LiteralPath $CommandPath -Raw | ConvertFrom-Json
            if ($command.id -and $command.id -ne $lastId) {
                if ($command.method -eq "TypeKey") {
                    $parameters = $keyboard.psbase.GetMethodParameters("TypeKey")
                    $parameters.keyCode = [uint32]$command.value
                    $result = $keyboard.psbase.InvokeMethod("TypeKey", $parameters, $null)
                } elseif ($command.method -eq "PressKey") {
                    $parameters = $keyboard.psbase.GetMethodParameters("PressKey")
                    $parameters.keyCode = [uint32]$command.value
                    $result = $keyboard.psbase.InvokeMethod("PressKey", $parameters, $null)
                } elseif ($command.method -eq "ReleaseKey") {
                    $parameters = $keyboard.psbase.GetMethodParameters("ReleaseKey")
                    $parameters.keyCode = [uint32]$command.value
                    $result = $keyboard.psbase.InvokeMethod("ReleaseKey", $parameters, $null)
                } elseif ($command.method -eq "TypeText") {
                    $parameters = $keyboard.psbase.GetMethodParameters("TypeText")
                    $parameters.asciiText = [string]$command.value
                    $result = $keyboard.psbase.InvokeMethod("TypeText", $parameters, $null)
                } else {
                    throw "Unsupported bridge method: $($command.method)"
                }
                $lastId = [string]$command.id
                @{
                    state = "processed"
                    vmName = $VmName
                    processedId = $lastId
                    returnValue = [uint32]$result.ReturnValue
                    processedUtc = [DateTime]::UtcNow.ToString("O")
                } | ConvertTo-Json -Compress | Set-Content -LiteralPath $StatusPath -Encoding UTF8
            }
        } catch {
            @{
                state = "error"
                vmName = $VmName
                processedId = $lastId
                message = $_.Exception.Message
                processedUtc = [DateTime]::UtcNow.ToString("O")
            } | ConvertTo-Json -Compress | Set-Content -LiteralPath $StatusPath -Encoding UTF8
        }
    }
    Start-Sleep -Milliseconds 100
}

@{ state = "stopped"; vmName = $VmName; processedId = $lastId } |
    ConvertTo-Json -Compress | Set-Content -LiteralPath $StatusPath -Encoding UTF8
