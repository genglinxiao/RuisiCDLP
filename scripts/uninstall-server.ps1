[CmdletBinding()]
param([switch] $PurgeData)

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"
$installed = Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" |
    Get-ItemProperty |
    Where-Object DisplayName -eq "Ruisi USB DLP Server" |
    Select-Object -First 1
if (-not $installed) { throw "Ruisi USB DLP Server is not installed." }

$uninstaller = Start-Process msiexec.exe -ArgumentList @("/x", $installed.PSChildName, "/qn", "/norestart") -Wait -PassThru
if ($uninstaller.ExitCode -ne 0) { throw "Uninstall failed with Windows Installer exit code $($uninstaller.ExitCode)." }
Remove-NetFirewallRule -DisplayName "Ruisi USB DLP Server" -ErrorAction SilentlyContinue

$dataDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramData "Ruisi\UsbDlpServer"))
$programData = [IO.Path]::GetFullPath($env:ProgramData)
if ($PurgeData -and $dataDirectory.StartsWith($programData, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $dataDirectory)) {
    Remove-Item -LiteralPath $dataDirectory -Recurse -Force
}
Write-Host "Ruisi USB DLP Server uninstalled. Database retained: $(-not $PurgeData)."
