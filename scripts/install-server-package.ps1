[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $AdminPasswordFile,
    [Parameter(Mandatory = $true)]
    [string] $EnrollmentTokenFile,
    [string] $AdminUsername = "admin",
    [string] $ListenUrl = "http://127.0.0.1:5091",
    [switch] $ExposeToLan,
    [switch] $AllowInsecureHttp,
    [string] $PackagePath
)

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $PackagePath) { $PackagePath = Join-Path $root "artifacts\installer\RuisiUsbDlpServer-1.1.0-x64.msi" }
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
$AdminPasswordFile = [IO.Path]::GetFullPath($AdminPasswordFile)
$EnrollmentTokenFile = [IO.Path]::GetFullPath($EnrollmentTokenFile)
foreach ($path in @($PackagePath, $AdminPasswordFile, $EnrollmentTokenFile)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file does not exist: $path" }
}

$uri = [Uri]$ListenUrl
if ($uri.Scheme -notin @("http", "https") -or $uri.Port -le 0) { throw "ListenUrl must be an absolute HTTP or HTTPS URL with a port." }
$loopbackHosts = @("localhost", "127.0.0.1", "[::1]", "::1")
$isLoopback = $uri.Host -in $loopbackHosts
if (-not $isLoopback -and -not $ExposeToLan) { throw "A non-loopback listener requires -ExposeToLan." }
if ($uri.Scheme -eq "http" -and -not $isLoopback -and -not $AllowInsecureHttp) { throw "LAN HTTP requires the explicit -AllowInsecureHttp switch. Prefer HTTPS in production." }
if ((Get-Content -LiteralPath $AdminPasswordFile -Raw).Trim().Length -lt 14) { throw "The administrator password must contain at least 14 characters." }
if ((Get-Content -LiteralPath $EnrollmentTokenFile -Raw).Trim().Length -lt 32) { throw "The enrollment token must contain at least 32 characters." }

$allowHttp = ($uri.Scheme -eq "http")
$arguments = @(
    "/i", ('"' + $PackagePath + '"'), "/qn", "/norestart",
    "LISTENURL=$($ListenUrl.TrimEnd('/'))", "ADMINUSERNAME=$AdminUsername",
    ('ADMINPASSWORDFILE="' + $AdminPasswordFile + '"'),
    ('ENROLLMENTTOKENFILE="' + $EnrollmentTokenFile + '"'),
    "ALLOWINSECUREHTTP=$($allowHttp.ToString().ToLowerInvariant())"
)
$installer = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -Wait -PassThru
if ($installer.ExitCode -ne 0) { throw "Installation failed with Windows Installer exit code $($installer.ExitCode)." }

if ($ExposeToLan) {
    Remove-NetFirewallRule -DisplayName "Ruisi USB DLP Server" -ErrorAction SilentlyContinue
    New-NetFirewallRule -DisplayName "Ruisi USB DLP Server" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $uri.Port -Profile Domain,Private -Program "$env:ProgramFiles\Ruisi USB DLP Server\UsbDlp.Server.exe" | Out-Null
}
Write-Host "Ruisi USB DLP Server installed at $ListenUrl. LAN firewall rule: $([bool]$ExposeToLan)"
