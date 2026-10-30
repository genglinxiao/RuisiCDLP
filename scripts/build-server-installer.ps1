[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string] $Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$project = Join-Path $root "installer\UsbDlp.Server.Installer\UsbDlp.Server.Installer.wixproj"
$releaseDirectory = Join-Path $root "artifacts\installer"
$env:DOTNET_CLI_HOME = Join-Path $root ".dotnet"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_NOLOGO = "1"

dotnet build $project -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Server installer build failed." }

$builtPackage = Get-ChildItem (Join-Path (Split-Path $project) "bin") -Recurse -Filter "RuisiUsbDlpServer-1.1.0-x64.msi" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $builtPackage) { throw "The installer build completed but no server MSI was produced." }

New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
$destination = Join-Path $releaseDirectory $builtPackage.Name
Copy-Item -LiteralPath $builtPackage.FullName -Destination $destination -Force
$hash = Get-FileHash -LiteralPath $destination -Algorithm SHA256
Write-Host "Package: $destination"
Write-Host "SHA256:  $($hash.Hash)"
