[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ServerUrl,
    [Parameter(Mandatory = $true)]
    [string] $EnrollmentTokenFile,
    [switch] $EnableLiveEnforcement,
    [switch] $AllowInsecureHttp,
    [ValidateRange(2, 300)]
    [int] $LocalPollSeconds = 2,
    [string] $PackagePath
)

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $PackagePath) {
    $PackagePath = Join-Path $root "artifacts\installer\RuisiUsbDlpAgent-1.1.0-x64.msi"
}
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
$EnrollmentTokenFile = [IO.Path]::GetFullPath($EnrollmentTokenFile)
if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) { throw "Installer package does not exist: $PackagePath" }
if (-not (Test-Path -LiteralPath $EnrollmentTokenFile -PathType Leaf)) { throw "Enrollment token file does not exist: $EnrollmentTokenFile" }
if ($ServerUrl -notmatch '^https://' -and -not $AllowInsecureHttp) {
    throw "HTTP server URLs require -AllowInsecureHttp."
}

$arguments = @(
    "/i", ('"' + $PackagePath + '"'),
    "/qn", "/norestart",
    "SERVERURL=$($ServerUrl.TrimEnd('/'))",
    ('ENROLLMENTTOKENFILE="' + $EnrollmentTokenFile + '"'),
    "DRYRUN=$(-not [bool]$EnableLiveEnforcement)".ToLowerInvariant(),
    "ALLOWINSECUREHTTP=$([bool]$AllowInsecureHttp)".ToLowerInvariant(),
    "LOCALPOLLSECONDS=$LocalPollSeconds"
)
$installer = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -Wait -PassThru
if ($installer.ExitCode -ne 0) { throw "Installation failed with Windows Installer exit code $($installer.ExitCode)." }
Write-Host "Ruisi USB DLP Agent installed. Live enforcement: $([bool]$EnableLiveEnforcement)"
