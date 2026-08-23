[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateLength(14, 128)][string] $AdminPassword,
    [Parameter(Mandatory = $true)][ValidateLength(32, 256)][string] $EnrollmentToken,
    [string] $Url = "http://127.0.0.1:5091",
    [string] $DatabasePath = ".run\server\usb-dlp.db"
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$database = [IO.Path]::GetFullPath((Join-Path $root $DatabasePath))
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($database)) -Force | Out-Null
$env:DOTNET_CLI_HOME = Join-Path $root ".dotnet"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_NOLOGO = "1"
$env:UsbDlp__DatabasePath = $database
$env:UsbDlp__BootstrapEnrollmentToken = $EnrollmentToken
$env:UsbDlp__BootstrapAdminUsername = "admin"
$env:UsbDlp__BootstrapAdminPassword = $AdminPassword
dotnet run --project (Join-Path $root "src\UsbDlp.Server\UsbDlp.Server.csproj") --urls $Url
