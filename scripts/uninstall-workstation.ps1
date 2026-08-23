[CmdletBinding()]
param([switch] $PurgeData)

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$package = Join-Path $root "artifacts\installer\RuisiUsbDlpAgent-1.1.0-x64.msi"
if (Test-Path -LiteralPath $package -PathType Leaf) {
    $target = '"' + $package + '"'
}
else {
    $installed = Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" |
        Get-ItemProperty |
        Where-Object DisplayName -eq "Ruisi USB DLP Agent" |
        Select-Object -First 1
    if (-not $installed) { throw "Ruisi USB DLP Agent is not installed." }
    $target = $installed.PSChildName
}

$uninstaller = Start-Process msiexec.exe -ArgumentList @("/x", $target, "/qn", "/norestart") -Wait -PassThru
if ($uninstaller.ExitCode -ne 0) { throw "Uninstall failed with Windows Installer exit code $($uninstaller.ExitCode)." }

$dataDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramData "UsbDlp"))
if ($PurgeData -and $dataDirectory.StartsWith([IO.Path]::GetFullPath($env:ProgramData), [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $dataDirectory)) {
    Remove-Item -LiteralPath $dataDirectory -Recurse -Force
}
Write-Host "Ruisi USB DLP Agent uninstalled. ProgramData preserved: $(-not $PurgeData)."
