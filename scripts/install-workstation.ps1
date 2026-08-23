[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ServerUrl,
    [Parameter(Mandatory = $true)]
    [string] $EnrollmentTokenFile,
    [switch] $EnableLiveEnforcement,
    [switch] $AllowLocalHttp
)

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"
$installer = Join-Path $PSScriptRoot "install-agent-package.ps1"
& $installer `
    -ServerUrl $ServerUrl `
    -EnrollmentTokenFile $EnrollmentTokenFile `
    -EnableLiveEnforcement:$EnableLiveEnforcement `
    -AllowInsecureHttp:$AllowLocalHttp
