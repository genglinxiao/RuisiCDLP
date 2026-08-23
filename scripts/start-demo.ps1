[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateLength(14, 128)]
    [string] $AdminPassword,
    [switch] $UsePhysicalDevices,
    [switch] $EnableLiveEnforcement,
    [switch] $Reset
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$runDirectory = [IO.Path]::GetFullPath((Join-Path $root ".run\demo"))
if (-not $runDirectory.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved run directory is outside the workspace."
}
$statePath = Join-Path $runDirectory "processes.json"
if (Test-Path -LiteralPath $statePath) {
    throw "A demo state file already exists. Run scripts\stop-demo.ps1 before starting or resetting."
}
if ($EnableLiveEnforcement -and -not $UsePhysicalDevices) {
    throw "Live enforcement requires -UsePhysicalDevices; simulated devices can only run dry-run."
}
if ($EnableLiveEnforcement) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Live enforcement requires an Administrator PowerShell session. Restart PowerShell as Administrator and run this command again."
    }
}
if ($Reset -and (Test-Path -LiteralPath $runDirectory)) {
    Remove-Item -LiteralPath $runDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

$token = ([Guid]::NewGuid().ToString("N") + [Guid]::NewGuid().ToString("N"))
$env:DOTNET_CLI_HOME = Join-Path $root ".dotnet"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_NOLOGO = "1"
$env:UsbDlp__DatabasePath = Join-Path $runDirectory "server.db"
$env:UsbDlp__BootstrapEnrollmentToken = $token
$env:UsbDlp__BootstrapAdminUsername = "admin"
$env:UsbDlp__BootstrapAdminPassword = $AdminPassword
$env:UsbDlp__PolicyPollSeconds = "5"
$env:ASPNETCORE_ENVIRONMENT = "Development"
$env:UsbDlp__ServerBaseUrl = "http://127.0.0.1:5091"
$env:UsbDlp__EnrollmentToken = $token
$env:UsbDlp__DataDirectory = Join-Path $runDirectory "agent"
$env:UsbDlp__DryRun = if ($EnableLiveEnforcement) { "false" } else { "true" }
$env:UsbDlp__AllowInsecureHttp = "true"
$env:UsbDlp__LocalPollSeconds = "2"
$env:UsbDlp__SimulatedDevicePath = if ($UsePhysicalDevices) { "" } else { Join-Path $root "samples\device.usb-storage.json" }
$env:UsbDlp__NotifierExecutablePath = Join-Path $root "src\UsbDlp.Notifier\bin\Debug\net8.0-windows\UsbDlp.Notifier.exe"

dotnet build (Join-Path $root "UsbDlp.sln") --configuration Debug
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$serverOut = Join-Path $runDirectory "server.log"
$serverErr = Join-Path $runDirectory "server.err.log"
$serverExe = Join-Path $root "src\UsbDlp.Server\bin\Debug\net8.0\UsbDlp.Server.exe"
$agentExe = Join-Path $root "src\UsbDlp.Agent\bin\Debug\net8.0-windows\UsbDlp.Agent.exe"
$server = Start-Process -FilePath $serverExe -ArgumentList @("--urls","http://127.0.0.1:5091") -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr

$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    Start-Sleep -Milliseconds 300
    try { $ready = (Invoke-WebRequest -Uri "http://127.0.0.1:5091/Login" -UseBasicParsing).StatusCode -eq 200 } catch { $ready = $false }
    if ($ready) { break }
}
if (-not $ready) { Stop-Process -Id $server.Id -Force; throw "Server did not become ready. See $serverErr" }

$agent = Start-Process -FilePath $agentExe -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDirectory "agent.log") -RedirectStandardError (Join-Path $runDirectory "agent.err.log")
Start-Sleep -Seconds 2
$notifier = Get-Process -Name "UsbDlp.Notifier" -ErrorAction SilentlyContinue |
    Where-Object { $_.SessionId -eq [System.Diagnostics.Process]::GetCurrentProcess().SessionId } |
    Select-Object -First 1

@{ server = $server.Id; agent = $agent.Id; notifier = if ($notifier) { $notifier.Id } else { $null } } | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
Start-Process "http://127.0.0.1:5091/Login"
Write-Host "USB DLP demo is running."
Write-Host "Console: http://127.0.0.1:5091/Login"
Write-Host "Username: admin"
Write-Host "Dry-run: $(-not $EnableLiveEnforcement); simulated device: $(-not $UsePhysicalDevices)"
Write-Host "Logs: $runDirectory"
